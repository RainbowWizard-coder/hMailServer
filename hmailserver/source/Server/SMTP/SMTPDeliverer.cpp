// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"

#include "SMTPDeliverer.h"

#include "../Common/Application/Application.h"
#include "../Common/Application/ObjectCache.h"

#include "../Common/AntiVirus/AntiVirusConfiguration.h"
#include "../Common/AntiVirus/Sandbox.h"
#include "../Common/AntiVirus/VirusScanner.h"
#include "../Common/Util/MessageTrace.h"

#include "../Common/BO/MessageRecipient.h"
#include "../Common/BO/MessageRecipients.h"
#include "../Common/BO/MessageData.h"
#include "../Common/Cache/CacheContainer.h"
#include "../Common/Cache/InboxIDCache.h"
#include "../Common/Util/Time.h"
#include "../Common/Util/ServerStatus.h"
#include "../Common/Application/DomainSamples.h"
#include "../Common/Util/MailerDaemonAddressDeterminer.h"
#include "../Common/TCPIP/TCPConnection.h"

#include "../Common/Util/Utilities.h"
#include "../Common/Util/MessageAttachmentStripper.h"
#include "../Common/Util/MessageUtilities.h"
#include "../Common/Util/OtelTracer.h"
#include "../Common/Util/OtelTraceContext.h"

#include "../Common/Scripting/Events.h"

#include "../Common/Persistence/PersistentMessage.h"
#include "../Common/Persistence/MessageStore.h"
#include "../Common/Persistence/PersistentIMAPFolder.h"
#include "../Common/Persistence/PersistentAccount.h"
#include "../Common/Persistence/PersistentMessageRecipient.h"
#include "../Common/Persistence/PersistentServerMessage.h"

#include "../Common/BO/IMAPFolder.h"
#include "../Common/BO/Domain.h"
#include "../Common/BO/Account.h"
#include "../Common/BO/ServerMessages.h"

#include "LocalDelivery.h"
#include "ExternalDelivery.h"
#include "SMTPConfiguration.h"
#include "../Common/AntiSpam/DKIM/DKIMSigner.h"
#include "SMTPVirusNotifier.h"
#include "RuleApplier.h"
#include <set>
#include "../Common/AntiSpam/QuarantineStore.h"
#include "RuleResult.h"
#include "RecipientParser.h"
#include "MirrorMessage.h"
#include "SMTPForwarding.h"

#include "../Common/Util/AWStats.h"
#include "../Common/Util/Charset.h"
#include "../Common/Mime/Mime.h"

#include "DeliveryFailure.h"
#include "BounceTracker.h"
#include "ComplaintTracker.h"
#include "../Common/Util/DomainReports.h"
#include "../Common/Util/ListManager.h"
#include "../Common/Util/InboundWebhooks.h"
#include "../Common/Util/WebhookManager.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // Every delete of the queue row in DeliverMessage went unchecked, and a queue
      // row that survives is not a tidiness problem. The delivery manager selects
      // rows out of hm_messages, so one that is still there is picked up on the next
      // pass and delivered again - and again after that, for as long as the delete
      // keeps failing.
      //
      // Which of the four it is decides how bad that is. The delete after a
      // successful delivery turns into duplicate mail arriving repeatedly in every
      // recipient's mailbox, and no amount of looking at the server explains it. The
      // three on the abort paths mean a message that was rejected is reconsidered
      // instead of dropped, which is milder but still an unbounded retry.
      //
      // Nothing here can undo the delivery that already happened, so the response is
      // to say so, once, with enough detail to act on.
      void DeleteQueuedMessageOrReport(std::shared_ptr<Message> message, const String &consequence)
      {
         // Read first. DeleteObject resets the id on the way out, and although it does
         // so only after the statement has succeeded, an error message that depends on
         // that ordering is one refactor away from reporting message 0.
         const __int64 messageId = message->GetID();

         if (PersistentMessage::DeleteObject(message))
            return;

         ErrorManager::Instance()->ReportError(ErrorManager::High, 6104, "SMTPDeliverer::DeliverMessage",
            Formatter::Format("Message {0} could not be removed from the delivery queue. {1}",
               messageId, consequence));
      }

      //---------------------------------------------------------------------
      // RFC 3464 delivery status notification.
      //
      // Everything below turns the failure records into the machine-readable
      // half of the bounce. The human-readable half is untouched - it is a
      // translatable server message and it stays exactly as it was, first in
      // the report, which is where a person's mail client will show it.
      //---------------------------------------------------------------------

      bool ContainsNonAscii_(const String &value)
      {
         for (int i = 0; i < value.GetLength(); i++)
         {
            if (value.GetAt(i) > 127)
               return true;
         }

         return false;
      }

      bool ContainsNonAscii_(const AnsiString &value)
      {
         for (int i = 0; i < value.GetLength(); i++)
         {
            if ((unsigned char) value.GetAt(i) > 127)
               return true;
         }

         return false;
      }

      // A field value that must survive as ONE header field in the DSN.
      //
      // Two things are being prevented. A raw CR or LF inside the value would end
      // the field early and everything after it would be read as a new field
      // name - that is how a remote server's reply text becomes header injection
      // in a report addressed to a third party. And a value long enough to push
      // the line past the RFC 5322 998-octet limit makes the whole report
      // malformed; a remote reply is capped at 512 octets by RFC 5321 4.5.3.1.5,
      // but nothing forces a remote server to obey that and this server should
      // not emit an invalid message when one does not.
      //
      // Both are answered by RFC 5322 2.2.3 folding: a CRLF followed by
      // whitespace, which unfolds back to exactly what came in.
      String FoldForDsnField_(const String &value)
      {
         // Deliberately short of 78. The field name and its prefix sit in front
         // of the first line, and folding a few characters early costs nothing.
         const int maxLineLength = 70;

         String folded;
         int lineLength = 0;

         for (int i = 0; i < value.GetLength(); i++)
         {
            wchar_t c = value.GetAt(i);

            if (c == '\r' || c == '\n')
            {
               // A CRLF pair, or a lone CR or LF: either way one fold.
               if (c == '\r' && i + 1 < value.GetLength() && value.GetAt(i + 1) == '\n')
                  i++;

               folded += _T("\r\n ");
               lineLength = 1;
               continue;
            }

            // Other C0 controls have no meaning in a field body and some of them
            // (a bare NUL in particular) confuse everything downstream.
            if (c < 32 && c != '\t')
               continue;

            // Fold at a space rather than inside a word, so that unfolding gives
            // the original text back character for character.
            if (c == ' ' && lineLength >= maxLineLength)
            {
               folded += _T("\r\n ");
               lineLength = 1;
               continue;
            }

            folded += c;
            lineLength++;
         }

         folded.TrimRight(_T("\r\n "));

         return folded;
      }

      // The per-message and per-recipient fields of RFC 3464 section 2.
      String BuildDeliveryStatusContent_(const std::vector<DeliveryFailure> &failures)
      {
         String content;

         // RFC 3464 2.2.2. The only per-message field this server can fill
         // honestly: Arrival-Date is stored as a database timestamp rather than
         // an RFC 5322 date and there is no converter for it, and an
         // Original-Envelope-Id needs the ENVID this server accepts but does not
         // retain. Both are optional, and an omitted field asserts nothing.
         content += _T("Reporting-MTA: dns; ") + Utilities::ComputerName() + _T("\r\n");

         for (const DeliveryFailure &failure : failures)
         {
            content += _T("\r\n");

            // RFC 3464 2.3.1 requires Final-Recipient; RFC 6533 3.2 gives the
            // "utf-8" address type for an internationalized address, which is
            // the only correct label for one that will not fit in "rfc822".
            //
            // Original-Recipient is deliberately absent: it must come from the
            // RFC 3461 ORCPT parameter, and SMTPConnection validates ORCPT
            // without retaining it. Reconstructing it from the address this
            // server resolved to would report an alias expansion as the
            // sender's own spelling, which is exactly the mistake the field
            // exists to prevent.
            content += ContainsNonAscii_(failure.GetRecipient()) ? _T("Final-Recipient: utf-8; ") : _T("Final-Recipient: rfc822; ");
            content += failure.GetRecipient() + _T("\r\n");

            // RFC 3464 2.3.3. Always "failed" here, and never "delayed": every
            // record reaching this function belongs to a message this server has
            // stopped trying to deliver. A delay report would need its own
            // trigger, and this server does not send one - see the RFC 3461
            // NOTIFY=DELAY note in the release notes.
            content += _T("Action: failed\r\n");
            content += _T("Status: ") + failure.GetEnhancedStatusCode() + _T("\r\n");

            if (!failure.GetRemoteMta().IsEmpty())
               content += _T("Remote-MTA: dns; ") + failure.GetRemoteMta() + _T("\r\n");

            // RFC 3464 2.3.6: the diagnostic given by the remote MTA, in its own
            // words. Present only when there was one.
            if (!failure.GetRemoteSmtpReply().IsEmpty())
            {
               String diagnostic = FoldForDsnField_(failure.GetRemoteSmtpReply());

               if (!diagnostic.IsEmpty())
                  content += _T("Diagnostic-Code: smtp; ") + diagnostic + _T("\r\n");
            }

            if (!failure.GetLastAttemptDate().IsEmpty())
               content += _T("Last-Attempt-Date: ") + failure.GetLastAttemptDate() + _T("\r\n");
         }

         return content;
      }
   }

   SMTPDeliverer::SMTPDeliverer()
   {

   }

   SMTPDeliverer::~SMTPDeliverer()
   {
   }

   void
   SMTPDeliverer::DeliverMessage(std::shared_ptr<Message> pMessage)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Submits the next message in the database. 
   //---------------------------------------------------------------------------()
   {
      LOG_DEBUG("Delivering message...");

      if (!pMessage)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 4217, "SMTPDeliverer::DeliverMessage()", "The message to deliver was not given.");
         return;
      }

      // Fetch the message ID before we delete the message and it's reset.
      __int64 messageID = pMessage->GetID();

      // Taken out of the queue after it was picked up? The compromised-account
      // detection holds an account's other queued mail from the hour when it
      // locks the account (Util/CompromiseDetector.h), and the row it holds may
      // be one this deliverer has already selected: the pending list is read
      // once and walked, so "messagelocked = 0", which is all the hold's update
      // can see, was still true when the deliverer chose this message and is no
      // longer proof that nobody has it. Delivering it anyway put the message in
      // the recipient's mailbox AND on the review page, where releasing it would
      // deliver a second copy and discarding it would take nothing back.
      //
      // The lock is taken before the delivery task is queued and this runs in
      // the task, so a hold that succeeded happened before this read: whichever
      // of the two reached the row first now decides, and the other stands down.
      // The lock goes back, since a released message is found again by the
      // deliverer's "messagelocked = 0" select.
      if (PersistentMessage::IsHeldForReview(messageID))
      {
         PersistentMessage::UnlockObject(pMessage);

         LOG_DEBUG("Message " + StringParser::IntToString(messageID) + " was held for review after it was queued for delivery. It is left for the review page.");
         return;
      }

      // Before we do anything else, check that the message file exists. If the file
      // does not exist, there is nothing to deliver. So if the file does not exist,
      // delete pMessage from the database and report in the application log.
      const String messageFileName = MessageStore::WorkingFile(pMessage);
      if (!FileUtilities::Exists(messageFileName))
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5006, "SMTPDeliverer::DeliverMessage()", "Message " + StringParser::IntToString(pMessage->GetID()) + " could not be delivered since the data file does not exist.");
         DeleteQueuedMessageOrReport(pMessage, "Its data file is missing, so it will be reconsidered for delivery on every pass until the row is removed.");
         return;
      }

      if (pMessage->GetRecipients()->GetCount() == 0)
      {
         // No remaining recipients.
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5007, "SMTPDeliverer::DeliverMessage()", "Message " + StringParser::IntToString(pMessage->GetID()) + " could not be delivered. No remaining recipients. File: " + messageFileName);

         DeleteQueuedMessageOrReport(pMessage, "It has no remaining recipients, so it will be reconsidered for delivery on every pass until the row is removed.");
         return;
      }

      auto sSendersIP = MessageUtilities::GetSendersIP(pMessage);
      String preprocessingFailureReason;
      RuleResult globalRuleResult;

      // OpenTelemetry: one span per delivery attempt, with milestone events at
      // each stage and the terminal outcome. Parents any DB-query spans run during
      // delivery. The span joins the trace named by the message's own traceparent
      // header - the one reception prepended, or a fresh trace when the stored
      // message carries none - so every attempt for a message, and every
      // downstream hop, share one trace. No-op unless OtelEndpoint is configured;
      // FromMessageFile reads nothing when tracing is disabled.
      const bool otelEnabled = OtelTracer::Instance()->IsEnabled();
      OtelSpanScope otelDelivery("delivery", OtelSpanKindInternal,
                                 OtelTraceContext::FromMessageFile(messageFileName));
      if (otelEnabled)
      {
         AnsiString otelMessageId;
         otelMessageId.Format("%I64d", messageID);
         otelDelivery.AddAttribute("hmailserver.message.id", otelMessageId);
      }
      otelDelivery.AddEvent("delivery.start");

      bool deferDelivery = false;
      bool sandboxHeld = false;

      if (!PreprocessMessage_(pMessage, globalRuleResult, preprocessingFailureReason, deferDelivery, sandboxHeld))
      {
         if (sandboxHeld)
         {
            // Held for a sandbox's verdict, or kept in the queue for a minute
            // because the hold could not be recorded: either way the message is
            // KEPT, unlocked, and nothing here may delete it. The poller puts it
            // back in the queue when the hold is decided (Sandbox.h).
            ForgetUnscannableHold_(messageID);
            otelDelivery.AddEvent("delivery.held");
            return;
         }

         if (deferDelivery)
         {
            // Not an abort: the message is being KEPT for a later attempt, so
            // nothing here may delete it. Reached only when AVFailAction is 1 and
            // the scanner could not examine the message.
            otelDelivery.AddEvent("delivery.deferred");
            ServerStatus::Instance()->OnMessageDeferred();
            HandleUnscannableMessage_(pMessage);
            return;
         }

         // Message delivery was aborted during preprocessing.
         ForgetUnscannableHold_(messageID);
         otelDelivery.AddEvent("delivery.aborted");
         otelDelivery.SetOk(false);
         LogAwstatsMessageRejected_(sSendersIP, pMessage, preprocessingFailureReason);
         DeleteQueuedMessageOrReport(pMessage, "It was rejected during preprocessing, so it will be reconsidered on every pass until the row is removed.");
         return;
      }

      // Past scanning: whatever this message's AV hold history was, it is spent
      // and the entry can go rather than sitting in the table until a restart.
      ForgetUnscannableHold_(messageID);

      otelDelivery.AddEvent("delivery.preprocessed");

      // A delivery report for one of this installation's senders is read here,
      // before local delivery takes its recipients away, and is then delivered
      // unchanged like any other message. Returns at once unless bounce processing
      // is on and the message has a null sender. See BounceTracker.
      BounceTracker::InspectIncomingReport(pMessage);

      // An RFC 5965 abuse report that a provider's feedback loop posted to the
      // complaint mailbox is read here, for the same reason and in the same place:
      // before local delivery takes its recipients away, so the address it was
      // addressed to can still be seen. Returns at once unless complaint processing
      // is on and a mailbox is named. The message is delivered unchanged either
      // way. See ComplaintTracker.
      ComplaintTracker::InspectIncomingComplaint(pMessage);

      // A DMARC aggregate or TLS-RPT report posted to a hosted domain's report
      // mailbox is read here too, in the same place and for the same reason.
      // Returns at once unless the one local recipient is such a mailbox. The
      // message is delivered unchanged either way. See DomainReports.
      DomainReports::InspectIncomingReport(pMessage);

      // An address or a domain routed to a URL (Util/InboundWebhooks.h). Its
      // recipients are taken off the message here, after the rules, the scan
      // and the sandbox, and before the list manager and local delivery - the
      // order RCPT TO asks in, so that a route on a managed list's address
      // takes its mail as it took the RCPT. Each route's share is queued with a
      // copy of the message for the inbound webhook pass to POST - never from
      // this thread. A route that delivers as well puts back what the address
      // reaches without it, a managed list among them, for the list manager
      // below. A recipient whose copy could not be kept stays through local
      // delivery, so that the message is never taken for a single local
      // recipient's copy, is set aside after it, and is tried again with the
      // message; the list manager is told to leave it alone. A message with no
      // recipient a route could have made costs one look at its recipients.
      std::vector<std::shared_ptr<MessageRecipient> > heldForInboundWebhooks;
      InboundWebhooks::Instance()->TakeRoutedRecipients(pMessage, heldForInboundWebhooks);

      // A managed list, and the addresses it answers of its own - subscribe,
      // unsubscribe, request, bounces - are recipients with no account behind
      // them. The list manager answers each here, where a list's moderation
      // addresses have always been answered, and takes it off the message, so
      // that neither local nor external delivery ever sees one. Returns at once
      // when no list is managed. See ListManager.
      //
      // A posting the manager could not read is held rather than answered: its
      // row stays, and it stays in the message through local delivery - so
      // that the message is never taken for a single local recipient's copy,
      // which would move its file away - and is set aside before anything is
      // sent outside. The message is then tried again below.
      std::vector<std::shared_ptr<MessageRecipient> > heldForLists;
      ListManager::TakeListRecipients(pMessage, heldForLists, &heldForInboundWebhooks);

      std::vector<DeliveryFailure> saErrorMessages;

      // Perform deliver to local recipients.
      LocalDelivery localDeliverer(sSendersIP, pMessage, globalRuleResult);
      bool messageReused = localDeliverer.Perform(saErrorMessages);
      otelDelivery.AddEvent("delivery.local");

      ListManager::SetAside(pMessage, heldForLists);
      InboundWebhooks::SetAside(pMessage, heldForInboundWebhooks);

      bool messageRescheduled = false;

      // A message from outside is not sent on to an address outside that an alias or a
      // list of this server's expanded to, with a null envelope sender under this
      // server's host name or one at that name or a domain hosted here, when its From
      // names that organisation and it did not pass DMARC for it when it arrived - as
      // an account's forward of it is not made (SMTPForwarding).
      if (pMessage->GetRecipients()->GetCount() > 0)
         SMTPForwarding::WithholdOutwardExpansions(pMessage);

      // The suppression list, at the one point every outbound path meets: a
      // submission, an alias expanding outwards, a forward, a rule, a scheduled
      // send. A listed recipient is taken off the message here, before anything is
      // sent to it.
      if (pMessage->GetRecipients()->GetCount() > 0)
         BounceTracker::ApplyAtDelivery(pMessage, sSendersIP, saErrorMessages);

      if (pMessage->GetRecipients()->GetCount() > 0)
      {
         // Perform deliveries to external recipients.
         ExternalDelivery externalDeliverer(sSendersIP, pMessage, globalRuleResult);
         messageRescheduled = externalDeliverer.Perform(saErrorMessages);
         otelDelivery.AddEvent("delivery.external");

         // Every recipient still carries what happened to it - delivered, refused
         // and in whose words, or deferred - so the bounces are counted from the
         // recipients themselves, and a delivery's report key is kept.
         BounceTracker::RecordDeliveryPass(pMessage, messageRescheduled);

         // The complaint rate's denominator: the recipients a remote accepted in
         // this pass, added to the sending domain's tally for today. One write per
         // pass, and none at all unless a complaint mailbox is named.
         ComplaintTracker::RecordDeliveryPass(pMessage);
      }

      // The local recipients whose mailbox's encryption at rest was not known
      // (LocalDelivery::GetHeldForKeys): held as the list postings below are -
      // the message stays in the queue for them, with its outside recipients'
      // deferral or on the retry schedule of its own, and the sender is told
      // only when the retries run out. Never a bounce while retries are left,
      // never a discard.
      if (!localDeliverer.GetHeldForKeys().empty())
         messageRescheduled = LocalDelivery::HoldForRetry(pMessage, localDeliverer.GetHeldForKeys(), messageRescheduled, saErrorMessages);

      // The postings the list manager could not read: the message stays in the
      // queue for them - with its outside recipients' deferral when it has one,
      // or on the retry schedule of its own - and when the retries run out the
      // sender is told, below, as for any failure.
      if (!heldForLists.empty())
         messageRescheduled = ListManager::HoldForRetry(pMessage, heldForLists, messageRescheduled, saErrorMessages);

      // The routed recipients whose copy could not be kept, the same way.
      if (!heldForInboundWebhooks.empty())
         messageRescheduled = InboundWebhooks::HoldForRetry(pMessage, heldForInboundWebhooks, messageRescheduled, saErrorMessages);

      // If an error has occurred, now is the time to send an error
      // message back to the author of the email message.
      if (saErrorMessages.size() > 0)
         SubmitErrorLog_(pMessage, saErrorMessages);

      // Record the delivery outcome of this pass for the metrics endpoint. A
      // bounce (permanent failure) is counted separately inside SubmitErrorLog_;
      // here we count the terminal delivered/deferred outcomes.
      if (messageRescheduled)
      {
         ServerStatus::Instance()->OnMessageDeferred();
         otelDelivery.AddEvent("delivery.deferred");
      }
      else if (saErrorMessages.empty())
      {
         ServerStatus::Instance()->OnMessageDelivered();
         otelDelivery.AddEvent("delivery.delivered");
      }
      else
      {
         otelDelivery.AddEvent("delivery.failed");
         otelDelivery.SetOk(false);
      }

      // Unless the message has been re-used, or has been rescheduled for
      // later delivery, we should delete it now.
      bool deleteMessageNow = !messageReused && !messageRescheduled;
      if (deleteMessageNow)
      {
         // Check that we haven't reused this message before we delete it.
         if (pMessage->GetAccountID() > 0)
         {
            String errorMessage;
            errorMessage.Format(_T("Attempting to delete message %I64d even though it's been delivered to user account %I64d ."), messageID, pMessage->GetAccountID());
            ErrorManager::Instance()->ReportError(ErrorManager::High, 5208, "SMTPDeliverer::DeliverMessage", errorMessage);
            return;
         }

         DeleteQueuedMessageOrReport(pMessage, "It has already been DELIVERED, so it will be delivered again on the next pass - recipients will receive duplicates until the row is removed.");
      }

      String logText;
      logText.Format(_T("SMTPDeliverer - Message %I64d: Message delivery thread completed."), messageID);
      LOG_APPLICATION(logText);

      return;   
   }

   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Performs preprocessing of the message. Checks that the file exists, runs
   // global rules, virus scanning and mirroring.
   //
   // Returns true if delivery should continue. False if it should be aborted.
   //---------------------------------------------------------------------------()
   bool
   SMTPDeliverer::PreprocessMessage_(std::shared_ptr<Message> pMessage, RuleResult &globalRuleResult, String &preprocessingFailureReason, bool &deferDelivery, bool &sandboxHeld)
   {
      preprocessingFailureReason = "";
      deferDelivery = false;
      sandboxHeld = false;

      // Create recipient list.
      String sRecipientList = pMessage->GetRecipients()->GetCommaSeperatedRecipientList();

      String log_from_address = pMessage->GetFromAddress().IsEmpty() ? "<Empty>" : pMessage->GetFromAddress();

      auto messageFileName = MessageStore::WorkingFile(pMessage);
      String sMessage = Formatter::Format("SMTPDeliverer - Message {0}: Delivering message from {1} to {2}. File: {3}",
                                                pMessage->GetID(), log_from_address, sRecipientList, messageFileName);

      LOG_APPLICATION(sMessage);

      // Run the first event in the delivery chain
      if (!Events::FireOnDeliveryStart(pMessage))
      {
         preprocessingFailureReason = "Delivery cancelled by OnDeliveryStart-event";
         return false;
      }

      // Run virus protection.
      if (!RunVirusProtection_(pMessage, deferDelivery))
      {
         preprocessingFailureReason = deferDelivery
            ? "Message held: the virus scanner could not examine it"
            : "Message delivery cancelled during virus scanning";
         return false;
      }

      // The sandbox, after the anti-virus scan and before the rules: a message
      // the scanner deleted never gets here, and a held message has had no rule
      // run on it, no signature and no mirror copy - all of that happens on the
      // pass that delivers it.
      if (!RunSandbox_(pMessage, sandboxHeld))
      {
         preprocessingFailureReason = sandboxHeld
            ? "Message held for the sandbox's verdict on its attachments"
            : "Message not delivered on the sandbox's verdict";
         return false;
      }

      // Apply rules on this message.
      if (!RunGlobalRules_(pMessage, globalRuleResult))
      {
         preprocessingFailureReason = "Message delivery cancelled during global rules";
         return false;
      }

      // Run the OnDeliverMessage-event
      if (!Events::FireOnDeliverMessage(pMessage))
      {
         preprocessingFailureReason = "Message delivery cancelled during OnDeliverMessage-event";
         return false;
      }

      // DKIM-sign the message before it is split between local and external
      // delivery paths. On the first pass only: the counter says so, and so
      // does the mark a shaping deferral leaves, because such a deferral keeps
      // the counter at nought (Message::FlagFirstPassDone) and DKIM's own
      // "already signed" check does not cover the ARC seal that follows it.
      if (pMessage->GetNoOfRetries() == 0 && !pMessage->GetFirstPassDone())
      {
         DKIMSigner signer;
         signer.Sign(pMessage);
      }

      // Run mirroring. The same guard: MirrorMessage has no "already mirrored"
      // check, and one copy per held pass was what the counter alone allowed.
      if (pMessage->GetNoOfRetries() == 0 && !pMessage->GetFirstPassDone())
      {
         MirrorMessage mirrorer(pMessage);
         mirrorer.Send();
      }

      return true;

   }


   void
   SMTPDeliverer::ReturnToSender(std::shared_ptr<Message> originalMessage, std::vector<DeliveryFailure> &failures)
   {
      if (!originalMessage || failures.empty())
         return;

      SubmitErrorLog_(originalMessage, failures);
   }

   void
   SMTPDeliverer::SubmitErrorLog_(std::shared_ptr<Message> pOrigMessage, std::vector<DeliveryFailure> &saErrorMessages)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Some of the messages was not sent to the recipient. We have to
   // send a mail back to the sender to tell him this.
   //
   // The report is a multipart/report; report-type=delivery-status (RFC 3462)
   // with the three parts RFC 3464 describes: the human-readable explanation
   // this server has always sent, unchanged and still first; the per-recipient
   // machine-readable record; and the failed message's own headers.
   //
   // There is no setting to turn this off, and there should not be. A
   // multipart/report whose first part is the same plain text as before reads
   // identically in every mail client - a client that knows nothing about RFC
   // 3464 shows the first part, which is what it used to show - and RFC 3464 has
   // been the interoperable form of a bounce since 2003. What was there before
   // was the thing with the compatibility problem: nothing could parse it.
   //---------------------------------------------------------------------------()
   {
      LOG_DEBUG("SD::SubmitErrorLog_");

      // Every caller has at least one failed recipient, and this is the
      // invariant rather than a case to handle: RFC 3464 2.3 requires at least
      // one per-recipient group, and a report naming nobody tells the sender
      // nothing while still looking like a delivery-status notification.
      if (saErrorMessages.empty())
         return;

      if (MailerDaemonAddressDeterminer::IsMailerDaemonAddress(pOrigMessage->GetFromAddress()))
      {
         LOG_DEBUG("SD::~SubmitErrorLog_");      
         return; // Avoid bounce-bounce
      }

      if (pOrigMessage->GetFromAddress().IsEmpty())
      {
         // No sender address specified, hence no place to
         // deliver the error log.
         return;
      }

      const String fileName = MessageStore::WorkingFile(pOrigMessage);

      // Header only when the message is too large to be read whole: the report
      // carries its header and none of its body, and the loop count and
      // Auto-Submitted are header fields - until the large-message row such a
      // message's load failed, and a bounce of it read neither.
      std::shared_ptr<MessageData> pMsgData = std::shared_ptr<MessageData> (new MessageData());
      pMsgData->LoadFromMessage(fileName, pOrigMessage, MessageData::LargeMessage::HeaderOnly);

      // true because we don't want to send bounces if AutoSubmitted header
      if (!RuleApplier::IsGeneratedResponseAllowed(pMsgData, true))
      {
         String sMessage;
         sMessage.Format(_T("Did not submit bounce message for message %I64d from %s since rule loop count was reached or Auto-Submitted header."), pOrigMessage->GetID(), String(pOrigMessage->GetFromAddress()).c_str());
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 4404, "SMTPDeliverer::_ApplyForwarding", sMessage);

         return;
      }

      int iRuleLoopCount = pMsgData->GetRuleLoopCount();

      String sMailerDaemonAddress = MailerDaemonAddressDeterminer::GetMailerDaemonAddress(pOrigMessage);
      
      String sMessageUndeliverable;
      sMessageUndeliverable.Format(_T("%s"), Configuration::Instance()->GetServerMessages()->GetMessage("MESSAGE_UNDELIVERABLE").c_str());
      String sErrMsg = Configuration::Instance()->GetServerMessages()->GetMessage("SEND_FAILED_NOTIFICATION");

      sErrMsg.Replace(_T("%MACRO_SENT%"), pMsgData->GetSentTime());
      sErrMsg.Replace(_T("%MACRO_SUBJECT%"), pMsgData->GetSubject());

      // Ability to include original headers in undeliverable message
      // https://www.progressiverobot.com/forum/viewtopic.php?f=2&t=19635
      sErrMsg.Replace(_T("%MACRO_ORIGINAL_HEADER%"), pMsgData->GetHeader());      

      String sCollectedErrors;
      for (unsigned int i=0;i<saErrorMessages.size();i++)
         sCollectedErrors += saErrorMessages[i].GetText();

      sErrMsg.Replace(_T("%MACRO_RECIPIENTS%"), sCollectedErrors);

      // Submit an error log (delivery failed)
      std::shared_ptr<Message> pMsg = std::shared_ptr<Message>(new Message());
      pMsg->SetState(Message::Delivering);

      const String newFileName = MessageStore::WorkingFile(pMsg);

      std::shared_ptr<MessageData> pNewMsgData = std::shared_ptr<MessageData>(new MessageData());
      pNewMsgData->LoadFromMessage(newFileName, pMsg);

      // Required headers
      pNewMsgData->GenerateMessageID();
      pNewMsgData->SetSentTime(Time::GetCurrentMimeDate());

      // Optional headers
      pNewMsgData->SetFrom(sMailerDaemonAddress);
      pNewMsgData->SetTo(pOrigMessage->GetFromAddress());
      pNewMsgData->SetSubject(sMessageUndeliverable + ": " + pMsgData->GetSubject());
      pNewMsgData->SetAutoGenerated();
      pNewMsgData->SetRuleLoopCount(iRuleLoopCount + 1);

      // The body. SetBody is deliberately NOT used: it rebuilds the message
      // around a single text/plain part, which is what made every bounce this
      // server has ever sent unparseable. The three parts are assembled directly
      // on the MIME tree instead, which MessageData::Write then serialises.
      {
         std::shared_ptr<MimeBody> reportMessage = pNewMsgData->GetMimeMessage();

         // Content-Type is written as one raw value rather than through
         // SetParameter, which quotes what it adds: report-type is a token in
         // every other MTA's bounce and there is no reason for this one to be
         // the odd one out. SetBoundary is left to generate the delimiter, since
         // it is the same generator every other multipart in this server uses,
         // and it appends to the value rather than replacing it because the
         // value already begins with "multipart".
         reportMessage->SetContentType("multipart/report; report-type=delivery-status", "");
         reportMessage->SetBoundary(nullptr);

         // The preamble, for a reader that shows the raw message. Same sentence
         // MessageData::CreatePart writes when it builds a multipart, so a
         // multipart this server produces looks the same however it was made.
         reportMessage->SetRawText("This is a multi-part message.\r\n\r\n");

         std::shared_ptr<MimeBody> emptyPosition;

         // Part 1, the human-readable notification (RFC 3464 2.1: "the first
         // component ... a human-readable description"). Byte-for-byte what the
         // whole body used to be - the same server message, the same macro
         // substitutions, the same UTF-8 quoted-printable encoding SetBody
         // produced - and first, so a mail client that has never heard of
         // multipart/report shows exactly what it showed before.
         String humanReadableBody = sErrMsg;
         if (humanReadableBody.Right(2) != _T("\r\n"))
            humanReadableBody += _T("\r\n");

         std::shared_ptr<MimeBody> humanReadablePart = reportMessage->CreatePart("text/plain", emptyPosition);
         humanReadablePart->SetContentType("text/plain", "");
         humanReadablePart->SetUnicodeText(humanReadableBody);

         // Part 2, the delivery status (RFC 3464 2.1). RFC 6533 3.1 gives
         // message/global-delivery-status for a report that must carry a UTF-8
         // address; message/delivery-status is a 7-bit-only type, so labelling
         // one that way and then writing UTF-8 into it would be a lie the
         // receiving parser acts on.
         String deliveryStatus = BuildDeliveryStatusContent_(saErrorMessages);

         std::shared_ptr<MimeBody> deliveryStatusPart = reportMessage->CreatePart("message/delivery-status", emptyPosition);

         // ToMultiByte in both branches, including the pure-ASCII one: the UTF-8
         // encoding of ASCII is the same bytes, and going through the same
         // converter means the wide-to-narrow step can never be done against the
         // machine's ANSI code page by accident.
         const AnsiString encodedDeliveryStatus = Charset::ToMultiByte(deliveryStatus, "utf-8");

         if (ContainsNonAscii_(deliveryStatus))
         {
            deliveryStatusPart->SetContentType("message/global-delivery-status", "");
            deliveryStatusPart->SetTransferEncoding("8bit");
         }
         else
         {
            deliveryStatusPart->SetContentType("message/delivery-status", "");
            deliveryStatusPart->SetTransferEncoding("7bit");
         }

         deliveryStatusPart->SetRawText(encodedDeliveryStatus);

         // Part 3, the original message's headers (RFC 3462 3, which names
         // text/rfc822-headers for the headers-only form).
         //
         // MimeHeader::Store rather than MessageData::GetHeader: GetHeader
         // rebuilds each field from its parsed name and value, which unfolds
         // them, and a DKIM-Signature or a long Received chain unfolded onto one
         // line is both a lossy copy and a line over the RFC 5322 limit. Store
         // writes back the original bytes of every field that was not modified,
         // which is what "the original headers" means. The explicit
         // qualification is needed because MimeBody declares its own two-argument
         // Store, which hides the base class's.
         AnsiString originalHeaders = pMsgData->GetMimeMessage()->MimeHeader::Store();

         std::shared_ptr<MimeBody> originalHeadersPart = reportMessage->CreatePart("text/rfc822-headers", emptyPosition);
         originalHeadersPart->SetContentType("text/rfc822-headers", "");

         // The bytes are copied verbatim, so the transfer encoding has to
         // describe what they are rather than what would be convenient. No
         // charset is declared with the 8-bit case: the original header block's
         // charset is not knowable here, and guessing one would misdecode it.
         originalHeadersPart->SetTransferEncoding(ContainsNonAscii_(originalHeaders) ? "8bit" : "7bit");
         originalHeadersPart->SetRawText(originalHeaders);
      }

      // Write message data. A first write of a message built from nothing, so a failure
      // means there is no bounce to deliver - queueing it anyway leaves a row whose file
      // is missing for the delivery to trip over. The function already returns early to
      // avoid bounce-bounce.
      if (!pNewMsgData->WriteReported(MessageStore::WorkingFile(pMsg), "A delivery-failure notification"))
         return;

      // Add recipients
      bool recipientOK = false;
      RecipientParser recipientParser;
      recipientParser.CreateMessageRecipientList(pOrigMessage->GetFromAddress(), pMsg->GetRecipients(), recipientOK);

      if (pMsg->GetRecipients()->GetCount() > 0)
      {
         // Save message.
         //
         // Unchecked, and the counter below it fired regardless - so a failed save
         // meant the delivery-failure notification was never queued while the metrics
         // said a bounce had been sent. The sender's message had already failed; this
         // is the notification telling them so, and losing it silently is how a
         // message disappears with nobody anywhere knowing.
         if (PersistentMessage::SaveObject(pMsg))
         {
            // A bounce/NDR was actually generated and queued for the sender.
            ServerStatus::Instance()->OnMessageBounced();

            // And delivered now: the delivery manager re-reads the queue when it is
            // told to or once a minute, and a report written from inside a delivery
            // - a sandbox decision, a policy refusal - otherwise waited for that
            // minute, as every other message this server queues itself does not.
            Application::Instance()->SubmitPendingEmail();

            // The outbound webhooks, here and nowhere else, because THIS is what a
            // bounce is: a non-delivery report that was written and queued for the
            // sender. A failed delivery attempt is not one - the message is retried
            // - and a recipient refused at RCPT TO is not one either, since the
            // sending server was told at the time and this server sent nothing. One
            // event per failed recipient, with the status the report carries.
            for (const DeliveryFailure &failure : saErrorMessages)
            {
               WebhookManager::Event event;
               event.name = WebhookManager::EventMessageBounced;
               event.sender = pOrigMessage->GetFromAddress();
               event.recipient = failure.GetRecipient();
               event.queue_id = pOrigMessage->GetID();
               event.status = failure.GetEnhancedStatusCode();
               event.reason = failure.GetRemoteSmtpReply().IsEmpty()
                  ? failure.GetText()
                  : failure.GetRemoteSmtpReply();

               WebhookManager::ScopeToLocalDomain(event.sender, event.recipient, event.domain, event.account);

               WebhookManager::Instance()->Raise(event);
            }
         }
         else
         {
            FileUtilities::DeleteFile(newFileName);

            String errorMessage;
            errorMessage.Format(_T("The delivery-failure notification for message %I64d could not be saved, so the sender has not been told that their message failed."),
               pOrigMessage->GetID());

            ErrorManager::Instance()->ReportError(ErrorManager::High, 6085, "SMTPDeliverer::SubmitErrorLog_", errorMessage);
         }
      }
      else
      {
         // Nobody to send the bounce to (a null sender, or an address that no
         // longer resolves). The file was already written, so remove it rather
         // than leaving an orphan in the queue directory with no database row.
         const String orphanedFile = MessageStore::WorkingFile(pMsg);

         if (!FileUtilities::DeleteFile(orphanedFile))
         {
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5212, "SMTPDeliverer::SubmitErrorLog_",
               "Could not delete the unsent bounce message file: " + orphanedFile);
         }
      }

      LOG_DEBUG("SD::~SubmitErrorLog_");

   }

   
   bool
   SMTPDeliverer::HandleInfectedMessage_(std::shared_ptr<Message> pMessage, const String &virusName)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Called if a virus is found in an email message. Checks in the configuration
   // what we should do with infected messages and does just that.
   //---------------------------------------------------------------------------()
   {
      AntiVirusConfiguration &antiVirusConfig = Configuration::Instance()->GetAntiVirusConfiguration();

      // Every recipient of the message is sent the "message deleted" notice
      // (SMTPVirusNotifier, VIRUS_NOTIFICATION): made from the message's header
      // as read here and once, when originalHeader is given, and otherwise from
      // the file, which each notice reads for itself.
      auto notifyEveryRecipient = [&pMessage](const AnsiString *originalHeader)
      {
         // owned by this call: the recipients of the message this delivery holds
         std::vector<std::shared_ptr<MessageRecipient> > &vecRecipients = pMessage->GetRecipients()->GetVector();
         auto iterRecipient = vecRecipients.begin();

         while (iterRecipient != vecRecipients.end())
         {
            if (originalHeader != nullptr)
               SMTPVirusNotifier::CreateMessageDeletedNotification(pMessage, (*iterRecipient)->GetAddress(), *originalHeader);
            else
               SMTPVirusNotifier::CreateMessageDeletedNotification(pMessage, (*iterRecipient)->GetAddress());

            iterRecipient++;
         }
      };

      if (antiVirusConfig.AVAction() == AntiVirusConfiguration::ActionDelete)
      {
         // The message should be deleted.

         // Should we notify the recipient?
         if (antiVirusConfig.AVNotifyReceiver())
            notifyEveryRecipient(nullptr);

         if (antiVirusConfig.AVNotifySender())
            SMTPVirusNotifier::CreateMessageDeletedNotification(pMessage, pMessage->GetFromAddress());

         String logMessage = Formatter::Format("SMTPDeliverer - Message {0}: Message will be deleted (contained virus {1}).",
            pMessage->GetID(), virusName);

         LOG_APPLICATION(logMessage);

         return false; // do not continue delivery

      }
      else if (antiVirusConfig.AVAction() == AntiVirusConfiguration::ActionStripAttachments)
      {
         std::shared_ptr<MessageAttachmentStripper> pStripper = std::shared_ptr<MessageAttachmentStripper>(new MessageAttachmentStripper());

         // The header as the message arrived, for the notice that takes the
         // message's place when it cannot be stripped (below): read before the
         // stripper writes over the file - a rewrite that fails part of the way
         // leaves a part of it there - once, however many recipients the message
         // has, and with nothing reported when the file cannot be read now: the
         // stripper and the line below say that themselves, where each notice's
         // own read of the file wrote HM4403 to the error log for every recipient
         // of a message the stripper could not read, and made a notice of empty
         // fields. A header with nothing in it is a notice of the envelope.
         const AnsiString originalHeader = PersistentMessage::LoadHeader(MessageStore::WorkingFile(pMessage), false);

         // The line says what the stripper did, not what it was asked to do.
         // "Message attachments stripped" was written unconditionally, so a
         // message the stripper had just declined (and said so, one line
         // earlier) was recorded as stripped in the next.
         const MessageAttachmentStripper::Result stripped = pStripper->Strip(pMessage);

         switch (stripped)
         {
         case MessageAttachmentStripper::Result::Stripped:
            LOG_APPLICATION(Formatter::Format("SMTPDeliverer - Message {0}: Message attachments stripped (contained virus {1}).",
               pMessage->GetID(), virusName));
            break;

         case MessageAttachmentStripper::Result::ReplacedByNotice:
            LOG_APPLICATION(Formatter::Format("SMTPDeliverer - Message {0}: Message attachments stripped (contained virus {1}): the message was one attachment, so the notice is all that is delivered.",
               pMessage->GetID(), virusName));
            break;

         case MessageAttachmentStripper::Result::NothingToStrip:
            LOG_APPLICATION(Formatter::Format("SMTPDeliverer - Message {0}: delivered as it is with virus {1}: the message is a single text part and has no attachments to strip. Set the anti-virus action to delete the message if such a message must not be delivered.",
               pMessage->GetID(), virusName));
            break;

         case MessageAttachmentStripper::Result::LeftAloneEncrypted:
            LOG_APPLICATION(Formatter::Format("SMTPDeliverer - Message {0}: delivered unstripped (encrypted end to end) with virus {1}. Set the anti-virus action to delete the message if such a message must not be delivered.",
               pMessage->GetID(), virusName));
            break;

         case MessageAttachmentStripper::Result::LeftAloneUnreadable:
         case MessageAttachmentStripper::Result::RewriteFailed:
            // Nothing of the message can be kept, so it is not delivered: it is
            // deleted, as the delete action deletes it. Deleted rather than held
            // for another try: what made it unreadable - another program holding
            // or removing its file, most often an antivirus, or a parse that
            // failed - is not something a later pass can count on, and a hold
            // would end in a return to its sender, which on virus mail is an
            // address the sender forged.
            //
            // So is a message whose stripped form could not be written over its
            // file, for the same reasons: the file is what the delivery below
            // would deliver, and it is the message as it arrived, or a part of the
            // rewrite. Such a message was delivered as it was, and logged above as
            // stripped, while the stripper did not say that its write had failed.
            //
            // The recipients are told in place of the note this action puts in
            // the messages it delivers, as the delete action tells them when its
            // notices are on; the sender is not, as this action never tells the
            // sender. The two notice settings are the delete action's, and the
            // Control Panel says so beside them.
            notifyEveryRecipient(&originalHeader);

            LOG_APPLICATION(Formatter::Format("SMTPDeliverer - Message {0}: Message will be deleted (contained virus {1}): the message could not be {2}, so its attachments could not be stripped. Its recipients are sent the message-deleted notice in place of the note.",
               pMessage->GetID(), virusName,
               String(stripped == MessageAttachmentStripper::Result::RewriteFailed ? _T("rewritten") : _T("read"))));

            return false; // do not continue delivery
         }

         return true; // continue delivery
      }

      // Weird configuration?
      HM_ASSERT(0);
      
      return false;
   }

   namespace
   {
      // How many times AVFailAction has held each message, kept here rather than
      // in the message's own try counter.
      //
      // The try counter would have been the obvious place, and it is wrong: the
      // DKIM signing and mirroring steps in PreprocessMessage_ run only when
      // GetNoOfRetries() == 0 - they act once, on the first pass - so a hold that
      // incremented it would make the pass that finally scans the message skip
      // signing entirely, and the message would go out UNSIGNED and fail DMARC at
      // every receiver that checks. A security feature that silently disables
      // another security feature is worse than the gap it closes.
      //
      // In memory, like the other policy counters in this server. The limit that
      // buys is worth stating rather than glossing: a restart resets the counts
      // while everything else about the message persists, so an administrator who
      // configures a hold window LONGER than the interval between their restarts
      // (say AVFailRetryMinutes 180 x AVFailMaxHolds 16, a 48-hour window, on a
      // host that reboots nightly) has a message that is held again and again and
      // never reaches its budget, so its sender is never told. Nothing is lost -
      // the message is intact, it is delivered on the first pass where any scanner
      // answers, and every hold is logged with the message id and the hold number
      // - but at the shipped defaults (16 x 15 minutes, four hours) a nightly
      // restart cannot reach this, and persisting the count means a schema change
      // that this one integer does not justify yet.
      boost::mutex av_hold_mutex_;
      std::map<__int64, int> av_hold_counts_;
      time_t av_hold_full_reported_at_ = 0;

      const size_t kMaxTrackedAvHolds = 100000;
      const time_t kAvHoldFullReportIntervalSeconds = 3600;

      // Returns the number of holds already recorded for this message, or -1 when
      // the table is full and this message is not one of the tracked ones.
      int TakeAvHold_(__int64 messageID, bool &reportTableFull)
      {
         reportTableFull = false;

         boost::lock_guard<boost::mutex> guard(av_hold_mutex_);

         auto existing = av_hold_counts_.find(messageID);
         if (existing != av_hold_counts_.end())
            return existing->second;

         if (av_hold_counts_.size() >= kMaxTrackedAvHolds)
         {
            // An earlier version cleared the WHOLE table here, on the reasoning
            // that a fresh budget for everyone beats an arbitrary eviction. That
            // was wrong in a way an adversarial review had to point out: with the
            // held set above the cap the clear recurs every retry cycle, so no
            // tracked message's count ever climbs to its budget and the bound
            // stops bounding for every message rather than for the overflow.
            //
            // The tracked counts are kept. This message is held WITHOUT being
            // counted, which is the fail-safe direction - it holds rather than
            // bounces - and it is reported, repeatedly rather than once, because a
            // condition that persists needs to stay visible.
            const time_t now = time(nullptr);

            if (av_hold_full_reported_at_ == 0 ||
                av_hold_full_reported_at_ > now ||
                now - av_hold_full_reported_at_ >= kAvHoldFullReportIntervalSeconds)
            {
               av_hold_full_reported_at_ = now;
               reportTableFull = true;
            }

            return -1;
         }

         return 0;
      }

      void RecordAvHold_(__int64 messageID, int holds)
      {
         boost::lock_guard<boost::mutex> guard(av_hold_mutex_);
         av_hold_counts_[messageID] = holds;
      }

      void ForgetAvHolds_(__int64 messageID)
      {
         boost::lock_guard<boost::mutex> guard(av_hold_mutex_);
         av_hold_counts_.erase(messageID);
      }
   }

   void
   SMTPDeliverer::ForgetUnscannableHold_(__int64 messageID)
   {
      ForgetAvHolds_(messageID);
   }

   void
   SMTPDeliverer::HandleUnscannableMessage_(std::shared_ptr<Message> pMessage)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // AVFailAction 1: the scanner could not examine this message, and the
   // administrator has said such a message must not be delivered.
   //
   // Held and retried on ITS OWN schedule - AVFailRetryMinutes apart, at most
   // AVFailMaxHolds times - then bounced. Not held for ever, because a message
   // nobody is told about is the failure mode this server keeps finding in its own
   // history; and not delivered at the end either, because delivering it
   // eventually is just fail-open with extra steps - the administrator asked for
   // the opposite.
   //
   // The bound is a COUNT OF HOLDS THIS POLICY KEEPS ITSELF, and the three things
   // it is not are all versions this was written as first:
   //
   //   - Not the SMTP delivery retry budget. `smtpnooftries` ships as 0, so
   //     borrowing it would have made this policy bounce on the very first
   //     scanner blip and never hold at all on a stock installation.
   //   - Not the message's age. That is anchored to ARRIVAL, so any message
   //     already older than the window would have had its entire budget spent
   //     before the scanner ever failed: a backlog released after an outage, an
   //     ETRN batch, or POP3-fetched mail carrying an upstream Received date
   //     would be bounced on the first attempt with no hold whatsoever.
   //     Restarting a server whose scanner comes up a minute later than its mail
   //     queue would have mass-bounced the whole backlog.
   //   - Not the message's own try counter either, which is what "a count this
   //     policy advances" first became: PreprocessMessage_ signs with DKIM and
   //     mirrors only while GetNoOfRetries() == 0, so incrementing it here would
   //     have meant the pass that finally scanned the message skipped signing,
   //     and it went out UNSIGNED to fail DMARC at every receiver that checks.
   //
   // So the count lives in this file, in memory, and the try counter is left
   // exactly as the rest of the delivery core expects to find it.
   //
   // The caller has already counted the deferral and must NOT delete the message
   // on this path.
   //---------------------------------------------------------------------------()
   {
      const int retryMinutes = IniFileSettings::Instance()->GetAVFailRetryMinutes();
      const int maxHolds = IniFileSettings::Instance()->GetAVFailMaxHolds();

      bool reportHoldTableFull = false;
      const int holdsSoFar = TakeAvHold_(pMessage->GetID(), reportHoldTableFull);

      // -1 means the hold table is full and this message is not tracked, so its
      // holds cannot be counted. It is held anyway rather than bounced.
      const bool counted = holdsSoFar >= 0;

      if (reportHoldTableFull)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6195, "SMTPDeliverer::HandleUnscannableMessage_",
            "100,000 messages are already being held because the virus scanner cannot examine them, which is as many as this server tracks hold counts for. Messages beyond that are still held - never delivered unscanned - but their holds are not counted, so they will not be returned to their senders while this lasts. The scanner needs attention.");
      }

      // maxHolds == 0 is "never queue unscanned mail", and it means that even when
      // the hold table is full: an administrator who asked for no holds does not
      // get one because this server ran out of counters.
      if (maxHolds > 0 && (!counted || holdsSoFar < maxHolds))
      {
         String holdProgress;
         if (counted)
         {
            RecordAvHold_(pMessage->GetID(), holdsSoFar + 1);
            holdProgress.Format(_T("this was hold %d of %d before it is returned to the sender"), holdsSoFar + 1, maxHolds);
         }
         else
         {
            holdProgress = _T("its holds are not being counted because the hold table is full, so it will keep being held rather than returned to the sender - see HM6195");
         }

         LOG_APPLICATION(Formatter::Format("SMTPDeliverer - Message {0}: The virus scanner could not examine this message, and AVFailAction is 1, so it has NOT been delivered. Holding it for another attempt in {1} minute(s); {2}.",
            pMessage->GetID(), retryMinutes, holdProgress));

         // bUpdateNoOfTries is false on purpose - see the DKIM note above. The
         // result is checked like every other write on the mail path: a failed
         // update leaves the old next-try time, so the message stays queued and is
         // reconsidered sooner than intended, which is the safe direction and the
         // reason this reports rather than deletes.
         if (!PersistentMessage::SetNextTryTime(pMessage->GetID(), false, retryMinutes))
         {
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6192, "SMTPDeliverer::HandleUnscannableMessage_",
               Formatter::Format("The next-try time of held message {0} could not be updated. It stays queued and will be retried, but sooner and more often than AVFailAction intends.", pMessage->GetID()));
         }

         if (!PersistentMessage::UnlockObject(pMessage))
         {
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6193, "SMTPDeliverer::HandleUnscannableMessage_",
               Formatter::Format("Held message {0} could not be unlocked, so no delivery thread will pick it up until the server restarts.", pMessage->GetID()));
         }

         return;
      }

      // The hold window is spent and the scanner is still not answering. Tell the
      // sender, using the same error-log/DSN machinery every other permanent
      // delivery failure uses, and then let the queue row go.
      //
      // SubmitErrorLog_ declines to report to a mailer-daemon address, to an empty
      // sender, or for a message carrying Auto-Submitted - so for those the
      // message is discarded here with nobody notified, and the log line says so
      // rather than claiming a report that was never raised. Discarding is the
      // right answer and not merely the convenient one: delivering unscanned mail
      // whenever a report cannot be raised would make `MAIL FROM:<>` a one-line
      // bypass of this entire policy.
      ForgetAvHolds_(pMessage->GetID());

      LOG_APPLICATION(Formatter::Format("SMTPDeliverer - Message {0}: The virus scanner has not been able to examine this message in {1} hold(s). AVFailAction is 1, so it is not being delivered. A non-delivery report is raised for its sender; a message with no reportable sender - a bounce, or one marked Auto-Submitted - is discarded here instead, never delivered unscanned.",
         pMessage->GetID(), maxHolds));

      // The two cases SubmitErrorLog_ declines outright are known here, so the
      // one outcome that must never be quiet - mail destroyed with NOBODY
      // notified - is reported rather than left as a log line an operator has to
      // go looking for. (Its third case, an Auto-Submitted message or a spent
      // rule-loop count, reports HM4404 itself.) The alternative to discarding is
      // delivering, and that would make an empty envelope sender a one-line
      // bypass of the whole policy, so this reports instead of relenting.
      const String fromAddress = pMessage->GetFromAddress();

      if (fromAddress.IsEmpty() || MailerDaemonAddressDeterminer::IsMailerDaemonAddress(fromAddress))
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6194, "SMTPDeliverer::HandleUnscannableMessage_",
            Formatter::Format("Message {0} could not be virus scanned within the AVFailAction hold window and has been DISCARDED: it has no sender that a non-delivery report can be addressed to (an empty envelope sender or a mailer-daemon address), and AVFailAction is 1, so delivering it unscanned was not an option. Recipients: {1}",
               pMessage->GetID(), pMessage->GetRecipients()->GetCommaSeperatedRecipientList()));
      }

      // One record per recipient, where this used to be one record naming all of
      // them in a comma-separated list. RFC 3464 2.3.1 wants Final-Recipient to
      // be one address, and a list in that field is not a recipient - it is a
      // string no parser can use. The wording of the sentence a person reads is
      // unchanged; for the single-recipient case, which is nearly all of them,
      // so is every byte of it.
      //
      // RFC 3463 X.7.0, "other or undefined security status": the message is
      // being refused for a security reason of this installation's choosing
      // (AVFailAction 1), and RFC 3463 registers nothing more specific for
      // "could not be scanned". Class 4 rather than 5 because the condition is a
      // scanner that is not answering, which is temporary by nature even though
      // it outlasted the hold window - the recipient's address was never in
      // question and a report claiming otherwise would be false.
      std::vector<DeliveryFailure> errorMessages;

      // owned by this call: the recipients of the message this delivery holds
      for (std::shared_ptr<MessageRecipient> recipient : pMessage->GetRecipients()->GetVector())
      {
         errorMessages.push_back(DeliveryFailure(recipient->GetAddress(), _T("4.7.0"),
            Formatter::Format("{0}\r\n   Error Type: SMTP\r\n   Error Description: Delivery failed\r\n   Additional information: The message could not be checked for viruses - the scanner did not respond on any delivery attempt - and this server is configured not to deliver unscanned mail. The server administrator should check the hMailServer error log.\r\n\r\n",
               recipient->GetAddress())));
      }

      SubmitErrorLog_(pMessage, errorMessages);

      DeleteQueuedMessageOrReport(pMessage, "It has been returned to the sender because it could not be virus scanned, so it will be bounced again on every pass until the row is removed.");
   }

   bool
   SMTPDeliverer::RunVirusProtection_(std::shared_ptr<Message> pMessage, bool &deferDelivery)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Runs virus scanning. If a virus is found, it's handled according to the
   // configuration. (Either the attachments are stripped, or the entire message
   // is deleted.
   //---------------------------------------------------------------------------()
   {

      // First check for blocked attachments.
      if (Configuration::Instance()->GetAntiVirusConfiguration().GetEnableAttachmentBlocking())      
      {
         VirusScanner::BlockAttachments(pMessage);
      }

      if (!pMessage->GetFlagVirusScan())
         return true;

      if (!VirusScanner::GetVirusScanningEnabled())
         return true;

      // Virus scanning
      String virusName;
      bool scannerFailed = false;

      if (VirusScanner::Scan(pMessage, virusName, &scannerFailed))
      {
         // Virus found: the server's counter, and the recipient domains'.
         ServerStatus::Instance()->OnVirusRemoved();
         DomainSamples::Instance()->RecordFor(pMessage->GetRecipients(), false);

         if (!HandleInfectedMessage_(pMessage, virusName))
            return false;

         return true;
      }

      // No virus found - but if no scanner actually managed to look, then "no
      // virus found" is not a verdict, and what happens next is the
      // administrator's policy rather than this server's guess. The default
      // remains what it has always been: deliver, having reported the scanner
      // error (HM5406). Note the ordering - a virus FOUND is acted on above even
      // if another scanner errored, because a positive result from one scanner is
      // not made less true by a second one being down.
      if (scannerFailed && IniFileSettings::Instance()->GetAVFailAction() == 1)
      {
         deferDelivery = true;
         return false;
      }

      return true;

   }

   bool
   SMTPDeliverer::RunSandbox_(std::shared_ptr<Message> pMessage, bool &sandboxHeld)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Attachments opened somewhere safe. Sandbox::Examine decides; this carries
   // out the two outcomes that need the deliverer - the quarantine, whose
   // failure must not deliver what a sandbox called malicious, and the report
   // to the sender.
   //---------------------------------------------------------------------------()
   {
      sandboxHeld = false;

      const Sandbox::Result result = Sandbox::Examine(pMessage);

      switch (result.outcome)
      {
      case Sandbox::OutcomeHeld:
         sandboxHeld = true;
         return false;

      case Sandbox::OutcomeQuarantine:
      {
         // The quarantine is the end state of a verdict: the recipients' digest
         // lists it and a release delivers it, without a second submission
         // (Message::FlagReleasedFromSandboxQuarantine, which the release sets
         // for this entry because its reason is the sandbox's, and which
         // Sandbox::Examine reads). Sandbox::Examine asks for a quarantine only
         // while QuarantineEnabled is on; with it off, the answer is a return.
         if (QuarantineStore::Quarantine(pMessage, result.reason, result.score))
         {
            Sandbox::Forget(pMessage->GetID());

            LOG_APPLICATION(Formatter::Format("SMTPDeliverer - Message {0}: quarantined on the sandbox's verdict. {1}",
               pMessage->GetID(), result.reason));

            MessageTrace::Record(pMessage->GetID(), MessageTrace::EventQuarantined, pMessage->GetFromAddress(),
               pMessage->GetRecipients()->GetCommaSeperatedRecipientList().Mid(0, 255), MessageUtilities::GetSendersIP(pMessage), 0, result.reason);

            return false;
         }

         // Not delivered, and not lost: back on hold with its decision kept, so
         // the next poll puts it in the queue again and the quarantine is asked
         // again. An administrator can discard it or release it meanwhile.
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7272, "SMTPDeliverer::RunSandbox_",
            Formatter::Format("The sandbox called message {0} malicious and the quarantine could not store it. It is not delivered: it is held again and the quarantine is tried at the next poll. {1}",
               pMessage->GetID(), result.reason));

         Sandbox::Rehold(pMessage);
         sandboxHeld = true;
         return false;
      }

      case Sandbox::OutcomeBounce:
      {
         // RFC 3463 X.7.1, "delivery not authorized, message refused": this
         // server's own policy refuses the message - SandboxTimeoutAction 1 for
         // one whose attachments were not given a verdict, or a malicious one
         // while the quarantine is off. The address was never in question.
         //
         // SubmitErrorLog_ declines a null sender and a mailer-daemon quietly,
         // so for those the message is DISCARDED, and the log says so here
         // rather than claim a return that never happened. (Its third case, an
         // Auto-Submitted message, reports HM4404 itself.) Delivering instead
         // would make an empty envelope sender a way past the sandbox.
         const String fromAddress = pMessage->GetFromAddress();
         if (fromAddress.IsEmpty() || MailerDaemonAddressDeterminer::IsMailerDaemonAddress(fromAddress))
         {
            LOG_APPLICATION(Formatter::Format("SMTPDeliverer - Message {0}: discarded: no sender to return it to. {1}",
               pMessage->GetID(), result.reason));
            return false;
         }

         LOG_APPLICATION(Formatter::Format("SMTPDeliverer - Message {0}: returned to its sender with 5.7.1. {1}",
            pMessage->GetID(), result.reason));

         std::vector<DeliveryFailure> errorMessages;

         // owned by this call: the recipients of the message this delivery holds
         for (std::shared_ptr<MessageRecipient> recipient : pMessage->GetRecipients()->GetVector())
         {
            if (!recipient)
               continue;

            errorMessages.push_back(DeliveryFailure(recipient->GetAddress(), _T("5.7.1"),
               Formatter::Format("{0}\r\n   Error Type: SMTP\r\n   Error Description: Delivery failed\r\n   Additional information: {1}\r\n\r\n",
                  recipient->GetAddress(), result.reason)));
         }

         if (!errorMessages.empty())
            SubmitErrorLog_(pMessage, errorMessages);

         return false;
      }

      case Sandbox::OutcomeContinue:
      default:
         return true;
      }
   }

   bool 
   SMTPDeliverer::RunGlobalRules_(std::shared_ptr<Message> pMessage, RuleResult &ruleResult)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Performs global rules. We will do this on every retry of the message.
   //---------------------------------------------------------------------------()
   {
      std::shared_ptr<RuleApplier> pRuleApplier = std::shared_ptr<RuleApplier>(new RuleApplier);
      std::shared_ptr<Account> emptyAccount;
      pRuleApplier->ApplyRules(ObjectCache::Instance()->GetGlobalRules(), emptyAccount, pMessage, ruleResult);

      // The domains' own rules, after the server's, each once: the sending
      // domain's on the whole message, and each recipient domain's on the
      // whole message when every recipient is its own and on its own
      // recipients otherwise (RuleApplier::DomainRuleScopes).
      if (!ruleResult.GetDeleteEmail())
      {
         for (const RuleApplier::DomainRuleScope &scope : RuleApplier::DomainRuleScopes(pMessage))
         {
            if (scope.whole_message)
            {
               pRuleApplier->ApplyRules(ObjectCache::Instance()->GetDomainRules(scope.domain_id), emptyAccount, pMessage, ruleResult);
               if (ruleResult.GetDeleteEmail())
                  break;

               continue;
            }

            // A result of its own: a route, a bound address or required TLS
            // from these rules is not taken (RuleApplier::SetRecipientScope),
            // and a delete, a reject or a quarantine is the domain's recipients
            // taken off, not the message.
            RuleApplier scoped;
            scoped.SetRecipientScope(scope.domain_id);
            RuleResult scopedResult;
            scoped.ApplyRules(ObjectCache::Instance()->GetDomainRules(scope.domain_id), emptyAccount, pMessage, scopedResult);

            if (scopedResult.GetDeleteEmail())
               TakeDomainsRecipientsOff_(pMessage, scope.domain_id, false, scopedResult.GetDeleteRuleName());
            else if (scopedResult.GetQuarantine() && !pMessage->GetReleasedFromQuarantine())
               TakeDomainsRecipientsOff_(pMessage, scope.domain_id, true, scopedResult.GetQuarantineRuleName());
         }

         if (!ruleResult.GetDeleteEmail() && pMessage->GetRecipients()->GetCount() == 0)
         {
            ruleResult.SetDeleteEmail(true);
            ruleResult.SetDeleteRuleName(_T("every recipient was taken off by its domain's rules"));
         }
      }

      if (ruleResult.GetDeleteEmail())
      {
         String sDeleteRuleName = ruleResult.GetDeleteRuleName();

         String sMessage;
         sMessage.Format(_T("SMTPDeliverer - Message %I64d: ")
            _T("Message will be deleted. Action triggered by a global rule (%s). "),
            pMessage->GetID(),
            sDeleteRuleName.c_str());
         LOG_APPLICATION(sMessage);

         return false;
      }

      if (ruleResult.GetQuarantine() && pMessage->GetReleasedFromQuarantine())
      {
         // A message the quarantine released has been judged by a person, and
         // the rule that held it would hold it again on the way back: the same
         // text, the same recipients, the same match, for ever. The release
         // mark is what says the judgement has been made - as it says it to
         // the recipients' own lists in local delivery - so the hold is
         // skipped and the review can end in delivery. The rule's other
         // actions have run as usual; the mark is cleared before the message
         // reaches a mailbox.
         String sMessage;
         sMessage.Format(_T("SMTPDeliverer - Message %I64d: A rule (%s) asked for the message to be quarantined, and it was released from the quarantine by a person, so it is being delivered."),
            pMessage->GetID(), ruleResult.GetQuarantineRuleName().c_str());
         LOG_APPLICATION(sMessage);
      }
      else if (ruleResult.GetQuarantine())
      {
         // Held rather than delivered, for an administrator to release or
         // delete; the copy the store keeps is the message as it is now, rules
         // applied. A store that could not keep it delivers the message rather
         // than lose it, and says so.
         if (QuarantineStore::Quarantine(pMessage, _T("Rule: ") + ruleResult.GetQuarantineRuleName(), 0))
         {
            String sMessage;
            sMessage.Format(_T("SMTPDeliverer - Message %I64d: Message quarantined. Action triggered by a rule (%s)."),
               pMessage->GetID(), ruleResult.GetQuarantineRuleName().c_str());
            LOG_APPLICATION(sMessage);

            return false;
         }

         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6261, "SMTPDeliverer::RunGlobalRules_",
            "A rule asked for a message to be quarantined and the quarantine could not store it; the message is being delivered instead.");
      }

      return true;

   }

   void
   SMTPDeliverer::TakeDomainsRecipientsOff_(std::shared_ptr<Message> pMessage, __int64 domainId, bool quarantine, const String &ruleName)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // A recipient domain's rule deleted, rejected or quarantined a message that
   // also goes to recipients outside the domain: the domain's recipients are
   // taken off it, and the rest of it goes on. A quarantine keeps a copy for
   // them first, under the rule's name; a quarantine that cannot keep it
   // leaves them on the message, as the whole-message quarantine delivers a
   // message it could not store rather than lose it.
   //---------------------------------------------------------------------------()
   {
      // owned by this call: the recipients of the message this delivery holds
      std::vector<std::shared_ptr<MessageRecipient> > &recipients = pMessage->GetRecipients()->GetVector();

      std::vector<std::shared_ptr<MessageRecipient> > taken;
      for (std::shared_ptr<MessageRecipient> recipient : recipients)
      {
         if (!recipient || !recipient->GetIsLocalName())
            continue;

         std::shared_ptr<const Domain> domain = CacheContainer::Instance()->GetDomain(StringParser::ExtractDomain(recipient->GetAddress()));
         if (domain && domain->GetID() == domainId)
            taken.push_back(recipient);
      }

      if (taken.empty())
         return;

      String addresses;
      for (std::shared_ptr<MessageRecipient> recipient : taken)
         addresses += (addresses.IsEmpty() ? _T("") : _T(", ")) + recipient->GetAddress();

      if (quarantine)
      {
         std::shared_ptr<Message> held = std::shared_ptr<Message>(new Message(false));
         held->SetFromAddress(pMessage->GetFromAddress());

         // Where the message came from, which the quarantine keeps with the copy for
         // its release (QuarantineStore::StoredName): the message's own record, and
         // not what its Received field says by now - a domain's rule ran on it.
         held->SetFromOutside(pMessage->GetFromOutside());
         held->SetSubmittedBySignedInUser(pMessage->GetSubmittedBySignedInUser());
         held->SetReceivedSpfResult(pMessage->GetReceivedSpfResult());
         held->SetReceivedSpfDomain(pMessage->GetReceivedSpfDomain());
         held->SetCopiedAuthDomains(pMessage->GetAuthDomainsOnArrival());
         held->SetRelayingDomainID(DKIMSigner::RelayingDomainID(pMessage, false));
         for (std::shared_ptr<MessageRecipient> recipient : taken)
         {
            std::shared_ptr<MessageRecipient> copy = std::shared_ptr<MessageRecipient>(new MessageRecipient());
            copy->CopyFrom(recipient);
            copy->SetMessageID(0);
            held->GetRecipients()->Add(copy);
         }

         if (!QuarantineStore::Quarantine(held, _T("Rule: ") + ruleName, 0, MessageStore::WorkingFile(pMessage)))
         {
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6261, "SMTPDeliverer::TakeDomainsRecipientsOff_",
               "A domain's rule asked for its recipients' copy of a message to be quarantined and the quarantine could not store it; the message is being delivered to them instead.");
            return;
         }
      }

      for (std::shared_ptr<MessageRecipient> recipient : taken)
      {
         if (!PersistentMessageRecipient::DeleteObject(recipient))
         {
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6106, "SMTPDeliverer::TakeDomainsRecipientsOff_",
               Formatter::Format("Message {0}: {1} was taken off by its domain's rule but that recipient could not be removed from the database.",
                  pMessage->GetID(), recipient->GetAddress()));
         }

         for (auto live = recipients.begin(); live != recipients.end(); ++live)
         {
            if (*live == recipient)
            {
               recipients.erase(live);
               break;
            }
         }
      }

      LOG_APPLICATION(Formatter::Format("SMTPDeliverer - Message {0}: not delivered to {1}: {2} by a rule of their domain ({3}). The message goes on to its other recipients.",
         pMessage->GetID(), addresses, quarantine ? String(_T("quarantined")) : String(_T("deleted")), ruleName));
   }

   void
   SMTPDeliverer::LogAwstatsMessageRejected_(const String &sSendersIP, std::shared_ptr<Message> pMessage, const String &sReason)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // If awstats logging is enabled, this function goes through all the recipients
   // of the message, and logs to the awstats log that they have been rejected.
   // This is used if a message is rejected after it has been transferred from the
   // client to the server.
   //---------------------------------------------------------------------------()
   {
      // Check that message exists
      if (!pMessage)
         return;

      // Go through the recipients and log one row for each of them.
      String sFromAddress = pMessage->GetFromAddress();     

      // owned by this call: the recipients of the message this delivery holds
      const std::vector<std::shared_ptr<MessageRecipient> > vecRecipients = pMessage->GetRecipients()->GetVector();
      std::vector<std::shared_ptr<MessageRecipient> >::const_iterator iterRecipient = vecRecipients.begin();
      while (iterRecipient != vecRecipients.end())
      {
         String sRecipientAddress = (*iterRecipient)->GetAddress();

         // Log the error message
         AWStats::LogDeliveryFailure(sSendersIP, pMessage->GetFromAddress(), sRecipientAddress,  550);
         Events::FireOnDeliveryFailed(pMessage, sSendersIP, sRecipientAddress, sReason);

         iterRecipient++;
      }

   }


}


