// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"
#include "../Common/Util/RestrictedDelivery.h"

#include "SMTPForwarding.h"
#include "RuleApplier.h"

#include "../Common/BO/Account.h"
#include "../Common/BO/MessageData.h"
#include "../Common/BO/Message.h"
#include "../Common/BO/MessageRecipients.h"
#include "../Common/BO/RemoteDomainPolicies.h"

#include "../Common/Persistence/PersistentMessage.h"
#include "../Common/Persistence/MessageStore.h"

#include "../Common/Util/SRS.h"
#include "../Common/Util/Utilities.h"
#include "../Common/Util/MessageOrigin.h"
#include "../Common/Application/Configuration.h"
#include "../Common/Application/IniFileSettings.h"
#include "../Common/BO/MessageRecipient.h"
#include "../Common/Persistence/PersistentMessageRecipient.h"
#include "../Common/AntiSpam/DMARC/DMARC.h"
#include "../Common/AntiSpam/QuarantineStore.h"
#include "../Common/Cache/CacheContainer.h"
#include "../Common/BO/Domain.h"
#include "../Common/Mime/Mime.h"


#include "RecipientParser.h"
#include "DeliveryQueue.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   bool 
   SMTPForwarding::PerformForwarding(std::shared_ptr<const Account> pRecipientAccount, std::shared_ptr<Message> pOriginalMessage)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Apply forwarding for the message.
   //---------------------------------------------------------------------------()
   {
      if (!pRecipientAccount->GetForwardEnabled())
         return true;
      
      if (pRecipientAccount->GetForwardAddress().IsEmpty())
      {
         // Configuration error. Forward was enabled, but no address specified.
         return true;
      }

      // Don't forward when the message is classified as spam
      if (pRecipientAccount->GetForwardAbortSpamFlagged() && pOriginalMessage->GetFlagSpam())
      {
         LOG_DEBUG("SMTPForwarding::PerformForwarding aborted, message marked as spam");
         return true;
      }

      if (!pRecipientAccount->GetForwardAddress().CompareNoCase(pRecipientAccount->GetAddress()))
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 4334, "SMTPDeliverer::_ApplyForwarding", "Could not forward message since target address as same as account address.");
         return true;
      }

      // The remote domain policy's second Exchange switch. An administrator who
      // has said mail for a domain may not be forwarded there has said it about
      // every account that has typed that address into its forwarding box, which
      // is the point: a per-account setting cannot be a per-organisation rule.
      // Answered true, as every other refusal here is, because the message HAS
      // been delivered to the mailbox - only the copy onward is withheld.
      if (!RemoteDomainPolicies::ForwardingPermitted(pRecipientAccount->GetForwardAddress()))
      {
         LOG_DEBUG("SMTPForwarding::PerformForwarding aborted: the remote domain policy for " +
            StringParser::ExtractDomain(pRecipientAccount->GetForwardAddress()) + " does not permit forwarding.");
         return true;
      }

      // Restricted delivery (Common/Util/RestrictedDelivery.h): a restricted
      // account's forward to a domain it may not write to is not made, and the
      // message is still delivered to the mailbox - true, as every refusal
      // above answers. A forward is the account sending, one step removed.
      if (RestrictedDelivery::Check(pRecipientAccount, pRecipientAccount->GetForwardAddress(), RestrictedDelivery::Outbound) == RestrictedDelivery::Refused)
      {
         LOG_DEBUG("SMTPForwarding::PerformForwarding aborted: restricted delivery - " + pRecipientAccount->GetAddress() +
            " may send to named domains only, and " + pRecipientAccount->GetForwardAddress() + " is not in one. The message is delivered to the mailbox.");
         return true;
      }

      String sErrorMessage;
      bool bTreatSecurityAsLocal = false;
      RecipientParser recipientParser;
      if (recipientParser.CheckDeliveryPossibility(false, 
                                                    pRecipientAccount->GetAddress(), 
                                                    pRecipientAccount->GetForwardAddress(), 
                                                    sErrorMessage, 
                                                    bTreatSecurityAsLocal, 
                                                    0) != RecipientParser::DP_Possible)
      {
         String sLogMessage = Formatter::Format("Could not forward message from {0} to {1}. Reason: {2}",
                        pRecipientAccount->GetAddress(), pRecipientAccount->GetForwardAddress(), sErrorMessage);

         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 4382, "SMTPDeliverer::_ApplyForwarding", sLogMessage);

         return true;
      }


      String originalFileName = MessageStore::WorkingFile(pRecipientAccount, pOriginalMessage);

      // Check that rule loop count is not yet reached. Header only when the message
      // is too large to be read whole (MessageData::MaxParseBytes): the count is a
      // header field, and until the large-message row such a message's load failed,
      // so its count read as 0 and a forwarding loop of it was never stopped.
      std::shared_ptr<MessageData> pOldMsgData  = std::shared_ptr<MessageData>(new MessageData());
      pOldMsgData->LoadFromMessage(originalFileName, pOriginalMessage, MessageData::LargeMessage::HeaderOnly);

      // false = check only loop counter not AutoSubmitted header because forward not rule
      if (!RuleApplier::IsGeneratedResponseAllowed(pOldMsgData, false))
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 4333, "SMTPDeliverer::_ApplyForwarding", "Could not forward message. Maximum forward loop count reached.");

         return true;
      }

      // The envelope sender the copy is sent with (EnvelopeOfCopy): SRS or the account's
      // own address as the settings ask, or the message's own. A copy that may not be
      // sent on with the one it came with - the null sender under this server's EHLO
      // name, or one at that name or at a domain hosted here - goes to no address
      // outside (below).
      String envelope;
      const bool maySendOn = ForwardEnvelope_(pRecipientAccount, pOriginalMessage, pOldMsgData, envelope);

      LOG_DEBUG(_T("Forwarding message"));

      // Create a copy of the message
      std::shared_ptr<Message> pNewMessage = PersistentMessage::CopyToQueue(pRecipientAccount, pOriginalMessage);

      // A failed spool copy (full disk, locked file) must not crash the delivery
      // task - that leaves the original message locked in the queue forever.
      if (!pNewMessage)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::High, 5210, "SMTPForwarding::PerformForwarding",
            "Could not forward the message because a copy of it could not be created.");
         return false;
      }

      if (pNewMessage->GetFromAddress().Compare(envelope) != 0)
         pNewMessage->SetFromAddress(envelope);

      pNewMessage->SetState(Message::Delivering);

      // Increase the number of rule-deliveries made - on the header alone for a copy
      // too large to be read whole, whose body is copied behind it as it is, where
      // the write of the count was refused (HM6010) and the copy went without it.
      std::shared_ptr<MessageData> pNewMsgData = std::shared_ptr<MessageData>(new MessageData());
      const String newFileName = MessageStore::WorkingFile(pNewMessage);
      pNewMsgData->LoadFromMessage(newFileName, pNewMessage, MessageData::LargeMessage::HeaderOnly);
      pNewMsgData->IncreaseRuleLoopCount();
      pNewMsgData->WriteReported(newFileName, "The rule-loop counter on a forwarded message");

      // Add new recipients
      bool recipientOK = false;
      recipientParser.CreateMessageRecipientList(pRecipientAccount->GetForwardAddress(), pNewMessage->GetRecipients(), recipientOK);

      // A copy that may not be sent on goes to the addresses here alone, and when there
      // are none it is not made: the message is in the mailbox - true, as every refusal
      // above answers, whatever the account's "keep original" says, since nothing was
      // forwarded.
      if (!maySendOn && WithholdOutsideRecipients_(pNewMessage, pOriginalMessage, pRecipientAccount, _T("forward"), envelope) &&
          pNewMessage->GetRecipients()->GetCount() == 0)
      {
         FileUtilities::DeleteFile(newFileName);
         return true;
      }

      // Check that there are recipients of the letter. If not, we should skip delivery.
      if (pNewMessage->GetRecipients()->GetCount() == 0)
      {
         // Delete the file since the message cannot be delivered.
         FileUtilities::DeleteFile(newFileName);
         
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 4332, "SMTPDeliverer::_ApplyForwarding", "Could not forward message; no recipients.");

         return true;
      }

      // This one could lose the message outright, which the other unchecked saves in
      // the delivery path could not. The return value decides whether the original is
      // kept, and an account configured to forward *without* keeping a copy answers
      // false - so a failed save meant the forwarded copy was never queued and the
      // original was deleted immediately afterwards. Nothing was delivered anywhere
      // and nothing said so.
      //
      // On failure the original is kept whatever the account setting says. That is
      // deliberately not what the administrator asked for: they asked not to keep a
      // copy of a message that was forwarded, and this one was not.
      if (!PersistentMessage::SaveObject(pNewMessage))
      {
         FileUtilities::DeleteFile(newFileName);

         ErrorManager::Instance()->ReportError(ErrorManager::High, 6087, "SMTPDeliverer::_ApplyForwarding",
            "Could not forward message; the forwarded copy could not be saved. The original has been kept regardless of the account's 'keep original' setting, so that the message is not lost.");

         return true;
      }

      // Wake the delivery manager, as the redirect below and a rule's forward
      // do: the copy is queued from inside the delivery of the original, after
      // the manager last read the queue, and untold it waited for the manager's
      // next look, up to a minute later.
      DeliveryQueue::StartDelivery();

      bool bKeepOriginal = pRecipientAccount->GetForwardKeepOriginal();

      return bKeepOriginal;
   }

   bool
   SMTPForwarding::RedirectToAddress(std::shared_ptr<const Account> pRecipientAccount, std::shared_ptr<Message> pOriginalMessage, const String &targetAddress)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Queues a copy of the message for delivery to an arbitrary address, as used by
   // the Sieve "redirect" action. Mirrors the copy/loop-guard/SRS handling of
   // PerformForwarding but takes an explicit target and ignores the account's own
   // forward settings.
   //---------------------------------------------------------------------------()
   {
      if (targetAddress.IsEmpty())
         return false;

      // Avoid an obvious self-redirect loop.
      if (targetAddress.CompareNoCase(pRecipientAccount->GetAddress()) == 0)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 4334, "SMTPForwarding::RedirectToAddress", "Could not redirect message since target address is the same as the account address.");
         return false;
      }

      // A Sieve redirect is a forward with a different name on it, and the same
      // remote domain policy applies. Without this line the switch above would be
      // a rule an account could walk around with a three-line filter script.
      if (!RemoteDomainPolicies::ForwardingPermitted(targetAddress))
      {
         LOG_DEBUG("SMTPForwarding::RedirectToAddress aborted: the remote domain policy for " +
            StringParser::ExtractDomain(targetAddress) + " does not permit forwarding.");
         return false;
      }

      // And restricted delivery, for the same reason: a redirect is the
      // account sending, and a three-line script must not walk around what
      // the account's forward box cannot.
      if (RestrictedDelivery::Check(pRecipientAccount, targetAddress, RestrictedDelivery::Outbound) == RestrictedDelivery::Refused)
      {
         LOG_DEBUG("SMTPForwarding::RedirectToAddress aborted: restricted delivery - " + pRecipientAccount->GetAddress() +
            " may send to named domains only, and " + targetAddress + " is not in one.");
         return false;
      }

      String sErrorMessage;
      bool bTreatSecurityAsLocal = false;
      RecipientParser recipientParser;
      if (recipientParser.CheckDeliveryPossibility(false,
                                                   pRecipientAccount->GetAddress(),
                                                   targetAddress,
                                                   sErrorMessage,
                                                   bTreatSecurityAsLocal,
                                                   0) != RecipientParser::DP_Possible)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 4382, "SMTPForwarding::RedirectToAddress",
            Formatter::Format("Could not redirect message from {0} to {1}. Reason: {2}", pRecipientAccount->GetAddress(), targetAddress, sErrorMessage));
         return false;
      }

      const String originalFileName = MessageStore::WorkingFile(pRecipientAccount, pOriginalMessage);

      // Header only when too large to be read whole, as PerformForwarding reads it.
      std::shared_ptr<MessageData> pOldMsgData = std::shared_ptr<MessageData>(new MessageData());
      pOldMsgData->LoadFromMessage(originalFileName, pOriginalMessage, MessageData::LargeMessage::HeaderOnly);

      // Guard against redirect loops via the rule loop counter.
      if (!RuleApplier::IsGeneratedResponseAllowed(pOldMsgData, false))
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 4333, "SMTPForwarding::RedirectToAddress", "Could not redirect message. Maximum loop count reached.");
         return false;
      }

      // The envelope sender, as for a forward.
      String envelope;
      const bool maySendOn = ForwardEnvelope_(pRecipientAccount, pOriginalMessage, pOldMsgData, envelope);

      std::shared_ptr<Message> pNewMessage = PersistentMessage::CopyToQueue(pRecipientAccount, pOriginalMessage);

      // See PerformForwarding: a failed copy must be reported, not dereferenced.
      if (!pNewMessage)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::High, 5210, "SMTPForwarding::RedirectToAddress",
            "Could not redirect the message because a copy of it could not be created.");
         return false;
      }

      if (pNewMessage->GetFromAddress().Compare(envelope) != 0)
         pNewMessage->SetFromAddress(envelope);

      pNewMessage->SetState(Message::Delivering);

      std::shared_ptr<MessageData> pNewMsgData = std::shared_ptr<MessageData>(new MessageData());
      const String newFileName = MessageStore::WorkingFile(pNewMessage);
      pNewMsgData->LoadFromMessage(newFileName, pNewMessage, MessageData::LargeMessage::HeaderOnly);
      pNewMsgData->IncreaseRuleLoopCount();
      pNewMsgData->WriteReported(newFileName, "The rule-loop counter on a forwarded message");

      bool recipientOK = false;
      recipientParser.CreateMessageRecipientList(targetAddress, pNewMessage->GetRecipients(), recipientOK);

      // As for a forward. A copy not made is "not redirected", and the caller delivers
      // the message to the mailbox.
      if (!maySendOn && WithholdOutsideRecipients_(pNewMessage, pOriginalMessage, pRecipientAccount, _T("redirect"), envelope) &&
          pNewMessage->GetRecipients()->GetCount() == 0)
      {
         FileUtilities::DeleteFile(newFileName);
         return false;
      }

      if (pNewMessage->GetRecipients()->GetCount() == 0)
      {
         FileUtilities::DeleteFile(newFileName);
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 4332, "SMTPForwarding::RedirectToAddress", "Could not redirect message; no recipients.");
         return false;
      }

      // Returning false here means "not redirected", which is what the no-recipients
      // case above returns and what the caller needs in order to fall back to normal
      // delivery rather than assume the message has gone somewhere.
      if (!PersistentMessage::SaveObject(pNewMessage))
      {
         FileUtilities::DeleteFile(newFileName);

         ErrorManager::Instance()->ReportError(ErrorManager::High, 6088, "SMTPForwarding::RedirectToAddress",
            "Could not redirect message; the redirected copy could not be saved. The file has been removed rather than left on disk with nothing referring to it.");

         return false;
      }

      // Wake the delivery manager so the redirected copy is sent promptly rather
      // than waiting for the next queue poll.
      DeliveryQueue::StartDelivery();

      return true;
   }

   // ---- The envelope a copy is sent on with ----------------------------------------

   namespace
   {
      // A domain as its organisation is read: no blanks or dots round it.
      String TrimmedDomain(const String &domain)
      {
         String trimmed = domain;
         trimmed.Trim();
         trimmed.TrimLeft(L'.');
         trimmed.TrimRight(L'.');
         return trimmed;
      }
   }

   std::vector<String>
   SMTPForwarding::FromFieldValues(MimeHeader &header)
   {
      std::vector<String> values;

      values.push_back(header.GetUnicodeFieldValue("From"));

      for (MimeField &field : header.Fields())
      {
         if (AnsiString(field.GetName()).CompareNoCase("From") == 0)
            values.push_back(String(AnsiString(field.GetValue())));
      }

      return values;
   }

   bool
   SMTPForwarding::FromNamesOrganisationOf(const std::vector<String> &fromFieldValues, const String &identityDomain)
   {
      const String identity = TrimmedDomain(identityDomain);
      if (identity.IsEmpty())
         return false;

      const String organisation = DMARC::GetOrganizationalDomainByPublicSuffixList(identity);

      for (const String &value : fromFieldValues)
      {
         // The address DMARC here reads in the field.
         const String fromDomain = TrimmedDomain(StringParser::ExtractDomain(DMARC::ExtractAddressFromHeaderValue(value)));
         if (!fromDomain.IsEmpty() && DMARC::GetOrganizationalDomainByPublicSuffixList(fromDomain).CompareNoCase(organisation) == 0)
            return true;

         // And every domain written after an @ anywhere in it - a second address, one
         // in a group or in a comment - and in every From field the message has: a
         // receiver may take any of them for the author.
         String lowered = value;
         lowered.MakeLower();

         for (int at = lowered.Find(_T("@")); at >= 0; at = lowered.Find(_T("@"), at + 1))
         {
            int end = at + 1;
            while (end < lowered.GetLength())
            {
               const wchar_t character = lowered[end];
               const bool ascii = character < 0x80;
               if (ascii && !((character >= L'a' && character <= L'z') || (character >= L'0' && character <= L'9') ||
                              character == L'.' || character == L'-'))
                  break;
               end++;
            }

            const String domain = TrimmedDomain(lowered.Mid(at + 1, end - at - 1));
            if (!domain.IsEmpty() && DMARC::GetOrganizationalDomainByPublicSuffixList(domain).CompareNoCase(organisation) == 0)
               return true;
         }
      }

      return false;
   }

   bool
   SMTPForwarding::DmarcPassedInOrganisationOf(const String &authDomains, const String &identityDomain)
   {
      const String identity = TrimmedDomain(identityDomain);
      if (authDomains.IsEmpty() || identity.IsEmpty())
         return false;

      const String organisation = DMARC::GetOrganizationalDomainByPublicSuffixList(identity);

      // Whole tokens (AuthenticationResults::PassedDomains), between commas or blanks.
      String tokens = authDomains;
      tokens.Replace(_T(","), _T(" "));
      tokens.Replace(_T("\t"), _T(" "));

      const String prefix = _T("dmarc=");
      for (String token : StringParser::SplitString(tokens, _T(" ")))
      {
         token.Trim();
         if (token.GetLength() <= prefix.GetLength() || token.Left(prefix.GetLength()).CompareNoCase(prefix) != 0)
            continue;

         const String passed = TrimmedDomain(token.Mid(prefix.GetLength()));
         if (!passed.IsEmpty() && DMARC::GetOrganizationalDomainByPublicSuffixList(passed).CompareNoCase(organisation) == 0)
            return true;
      }

      return false;
   }

   bool
   SMTPForwarding::MaySendOnWithIdentity(bool fromOutside, const String &authDomains, const std::vector<String> &fromFieldValues, const String &identityDomain)
   {
      // The From of a message this server is the submission server for, or made, is
      // one it vouches for already (DKIMSigner::SignsAsAuthorDomain).
      if (!fromOutside)
         return true;

      if (!FromNamesOrganisationOf(fromFieldValues, identityDomain))
         return true;

      return DmarcPassedInOrganisationOf(authDomains, identityDomain);
   }

   bool
   SMTPForwarding::IsOwnEnvelopeDomain(const String &senderDomain, const String &heloName, bool senderOrganisationHosted)
   {
      const String sender = TrimmedDomain(senderDomain);
      if (sender.IsEmpty())
         return false;

      if (senderOrganisationHosted)
         return true;

      const String helo = TrimmedDomain(heloName);
      return !helo.IsEmpty() && sender.CompareNoCase(helo) == 0;
   }

   bool
   SMTPForwarding::IsHostedDomain(const String &domain)
   {
      const String name = TrimmedDomain(domain);
      if (name.IsEmpty())
         return false;

      // A domain hosted here, or a domain alias of one, as MessageOrigin::IsHostedAddress
      // reads an address's domain.
      return MessageOrigin::IsHostedAddress(_T("postmaster@") + name);
   }

   bool
   SMTPForwarding::IsHostedOrganisation(const String &domain)
   {
      const String name = TrimmedDomain(domain);
      if (name.IsEmpty())
         return false;

      // The name itself, or the organisational domain a subdomain of a hosted domain
      // shares with it (the public suffix list's answer). Both are names this server's
      // DNS owner publishes SPF for, so an envelope at either makes a receiver's SPF
      // check read a record that can name this server. A third party's own domain, or
      // a subdomain of it, has a different organisation and is not caught.
      if (IsHostedDomain(name))
         return true;

      const String organisation = DMARC::GetOrganizationalDomainByPublicSuffixList(name);
      return !organisation.IsEmpty() && organisation.CompareNoCase(name) != 0 && IsHostedDomain(organisation);
   }

   bool
   SMTPForwarding::EnvelopeOfCopy(bool fromOutside, const String &authDomains, const String &envelopeSender, bool senderOrganisationHosted,
                                  const std::vector<String> &fromFieldValues, const String &rewritten, const String &heloName,
                                  String &envelope, bool &rewriteWithheld)
   {
      rewriteWithheld = false;
      envelope = envelopeSender;

      if (!rewritten.IsEmpty())
      {
         if (MaySendOnWithIdentity(fromOutside, authDomains, fromFieldValues, StringParser::ExtractDomain(rewritten)))
         {
            envelope = rewritten;
            return true;
         }

         rewriteWithheld = true;
      }

      if (!envelope.IsEmpty())
      {
         // The envelope sender the message came with: its sender's, whose own SPF
         // record a receiver reads - unless it is at this server's EHLO name or at a
         // domain whose organisation is hosted here, whose records name this server as
         // a rewritten one's do.
         const String senderDomain = StringParser::ExtractDomain(envelope);
         if (!IsOwnEnvelopeDomain(senderDomain, heloName, senderOrganisationHosted))
            return true;

         return MaySendOnWithIdentity(fromOutside, authDomains, fromFieldValues, senderDomain);
      }

      // A null sender: what SPF reads of it is the EHLO name (RFC 7208 2.4).
      return MaySendOnWithIdentity(fromOutside, authDomains, fromFieldValues, heloName);
   }

   String
   SMTPForwarding::RewrittenEnvelope_(std::shared_ptr<const Account> account, const String &envelopeFrom)
   {
      if (!account || envelopeFrom.IsEmpty())
         return String();

      if (IniFileSettings::Instance()->GetSRSEnabled())
      {
         // SRS (Sender Rewriting Scheme): when forwarding mail from an external
         // sender, rewrite the envelope MAIL FROM to a signed, reversible
         // address at the local forwarding domain so SPF stays aligned. Local
         // senders are left untouched (our own SPF already covers them). While SRS
         // is on, the plain rewrite below is not consulted.
         const String fromDomain = StringParser::ExtractDomain(envelopeFrom);
         if (CacheContainer::Instance()->GetDomain(fromDomain))
            return String();

         const String forwarderDomain = StringParser::ExtractDomain(account->GetAddress());
         return SRS::Forward(envelopeFrom, forwarderDomain, IniFileSettings::Instance()->GetSRSSecret());
      }

      if (IniFileSettings::Instance()->GetRewriteEnvelopeFromWhenForwarding())
         return account->GetAddress();

      return String();
   }

   bool
   SMTPForwarding::ForwardEnvelope_(std::shared_ptr<const Account> account, std::shared_ptr<Message> message,
                                    std::shared_ptr<MessageData> messageData, String &envelope)
   {
      const String envelopeSender = message->GetFromAddress();
      const String rewritten = RewrittenEnvelope_(account, envelopeSender);

      // The From fields matter only for mail from outside, and are read only for it,
      // from the header loaded for the loop count.
      std::vector<String> fromFieldValues;
      if (message->GetFromOutside() && messageData && messageData->GetMimeMessage())
         fromFieldValues = FromFieldValues(*messageData->GetMimeMessage());

      // Whether the envelope sender it came with is at a domain whose organisation is
      // hosted here, asked only where the answer can matter.
      const bool senderOrganisationHosted = message->GetFromOutside() && !envelopeSender.IsEmpty() &&
                                            IsHostedOrganisation(StringParser::ExtractDomain(envelopeSender));

      bool rewriteWithheld = false;
      const bool maySendOn = EnvelopeOfCopy(message->GetFromOutside(), message->GetAuthDomainsOnArrival(), envelopeSender, senderOrganisationHosted,
                                            fromFieldValues, rewritten, Utilities::ComputerName(), envelope, rewriteWithheld);

      if (rewriteWithheld)
      {
         LOG_DEBUG(Formatter::Format("SMTPForwarding: the copy of message {0} by {1} keeps the envelope sender it came with, <{2}>, in place of {3}: the message came from outside with a From of that address's organisation, with no DMARC pass for it recorded when it arrived.",
            message->GetID(), account->GetAddress(), envelopeSender, rewritten));
      }

      return maySendOn;
   }

   bool
   SMTPForwarding::WithholdOutsideRecipients_(std::shared_ptr<Message> copy, std::shared_ptr<const Message> message,
                                              std::shared_ptr<const Account> account, const String &what, const String &envelope)
   {
      return WithholdOutsideRecipients(copy, message->GetID(), what + _T(" by ") + account->GetAddress(), envelope);
   }

   namespace
   {
      // What a receiver's SPF check would have read of a copy sent with envelope, for
      // the application log: the envelope sender, or for a null one the host name.
      String SentWith(const String &envelope)
      {
         if (envelope.IsEmpty())
            return Formatter::Format("a null envelope sender, under this server's host name {0}", Utilities::ComputerName());

         return Formatter::Format("the envelope sender <{0}>", envelope);
      }
   }

   bool
   SMTPForwarding::WithholdOutsideRecipients(std::shared_ptr<Message> copy, __int64 messageId, const String &what, const String &envelope)
   {
      // owned by this call: the recipients of the copy its caller made and has not saved
      std::vector<std::shared_ptr<MessageRecipient> > &recipients = copy->GetRecipients()->GetVector();

      bool withheld = false;
      auto iterator = recipients.begin();
      while (iterator != recipients.end())
      {
         std::shared_ptr<MessageRecipient> recipient = *iterator;
         if (!recipient || recipient->GetIsLocalName())
         {
            iterator++;
            continue;
         }

         LOG_APPLICATION(Formatter::Format("SMTPForwarding: the {0} of message {1} was not sent to {2}: it would have been sent with {3}, and the message came from outside with a From of that organisation and no DMARC pass for it recorded when it arrived.",
            what, messageId, recipient->GetAddress(), SentWith(envelope)));

         withheld = true;
         iterator = recipients.erase(iterator);
      }

      return withheld;
   }

   bool
   SMTPForwarding::IsOutwardExpansion(const MessageRecipient &recipient, const String &mirrorAddress)
   {
      if (recipient.GetIsLocalName())
         return false;

      const String address = recipient.GetAddress();
      const String original = recipient.GetOriginalAddress();
      if (original.IsEmpty() || original.CompareNoCase(address) == 0)
         return false;

      if (!mirrorAddress.IsEmpty() && address.CompareNoCase(mirrorAddress) == 0)
         return false;

      return MessageOrigin::IsHostedAddress(original);
   }

   void
   SMTPForwarding::WithholdOutwardExpansions(std::shared_ptr<Message> message)
   {
      if (!message || !message->GetFromOutside())
         return;

      // owned by this call: the recipients of the message this delivery holds, and the loop below erases from it on purpose
      std::vector<std::shared_ptr<MessageRecipient> > &recipients = message->GetRecipients()->GetVector();

      const String mirrorAddress = Configuration::Instance()->GetMirrorAddress();

      bool anyExpansion = false;
      for (const std::shared_ptr<MessageRecipient> &recipient : recipients)
      {
         if (recipient && IsOutwardExpansion(*recipient, mirrorAddress))
         {
            anyExpansion = true;
            break;
         }
      }

      if (!anyExpansion)
         return;

      // The identity a receiver's SPF check reads of the message as it is sent on: the
      // EHLO name for a null envelope sender, and the domain of any other only when it
      // is an identity of this server's own (IsOwnEnvelopeDomain) - the sender's
      // otherwise, which this server does not vouch for.
      const String heloName = Utilities::ComputerName();
      const String envelopeSender = message->GetFromAddress();
      String identity = heloName;
      if (!envelopeSender.IsEmpty())
      {
         identity = StringParser::ExtractDomain(envelopeSender);
         if (!IsOwnEnvelopeDomain(identity, heloName, IsHostedOrganisation(identity)))
            return;
      }

      const AnsiString header = PersistentMessage::LoadHeader(MessageStore::WorkingFile(message));
      MimeHeader mimeHeader;
      mimeHeader.Load(header.c_str(), header.GetLength(), true);

      if (MaySendOnWithIdentity(true, message->GetAuthDomainsOnArrival(), FromFieldValues(mimeHeader), identity))
         return;

      auto iterator = recipients.begin();
      while (iterator != recipients.end())
      {
         std::shared_ptr<MessageRecipient> recipient = *iterator;
         if (!recipient || !IsOutwardExpansion(*recipient, mirrorAddress))
         {
            iterator++;
            continue;
         }

         // Gone from this pass either way, as a recipient the suppression list takes is
         // (BounceTracker::ApplyAtDelivery): a row that could not be deleted is read
         // back when the message is tried again for another recipient, and meets this
         // again.
         if (!PersistentMessageRecipient::DeleteObject(recipient))
         {
            LOG_APPLICATION(Formatter::Format("SMTPDeliverer - Message {0}: the row of recipient {1} could not be removed from the queue.",
               message->GetID(), recipient->GetAddress()));
         }

         LOG_APPLICATION(Formatter::Format("SMTPDeliverer - Message {0} was not sent on to {1}, which {2} expanded to: it would have been sent with {3}, and the message came from outside with a From of that organisation and no DMARC pass for it recorded when it arrived.",
            message->GetID(), recipient->GetAddress(), recipient->GetOriginalAddress(), SentWith(envelopeSender)));

         iterator = recipients.erase(iterator);
      }
   }

   void
   CopyEnvelopeTester::Test()
   {
      typedef std::vector<String> Values;

      // ---- The organisations a From names ----

      struct NamesCase { Values from; const wchar_t *identity; bool names; const char *what; };
      const NamesCase names[] =
      {
         { Values{ _T("The CEO <ceo@example.test>") }, L"example.test", true, "the From's own domain" },
         { Values{ _T("The CEO <ceo@example.test>") }, L"mail.example.test", true, "a name in the From's organisation" },
         { Values{ _T("The CEO <ceo@example.test>") }, L"MAIL.Example.TEST.", true, "a name in the From's organisation, in capitals with a dot after it" },
         { Values{ _T("The CEO <ceo@sub.example.test>") }, L"example.test", true, "the organisation of a From at a subdomain" },
         { Values{ _T("The CEO <ceo@example.test>") }, L"other.test", false, "another organisation" },
         { Values{ _T("x@other.example"), _T("ceo@example.test") }, L"example.test", true, "a second From field" },
         { Values{ _T("x@other.example, ceo@example.test") }, L"example.test", true, "a second address in the field" },
         { Values{ _T("\"ceo@example.test\" <x@other.example>") }, L"example.test", true, "an address in the display name" },
         { Values{ _T("x@other.example (ceo@example.test)") }, L"example.test", true, "an address in a comment" },
         { Values{ _T("Board: ceo@example.test;") }, L"example.test", true, "an address in a group" },
         { Values{ _T("x@other.example") }, L"example.test", false, "a From of another organisation alone" },
         { Values{ _T("someone@example.co.uk") }, L"other.co.uk", false, "two organisations under one public suffix" },
         { Values{ _T("someone@mail.example.co.uk") }, L"example.co.uk", true, "one organisation under a public suffix of two labels" },
         { Values{ _T("") }, L"example.test", false, "no From" },
         { Values{ _T("ceo@example.test") }, L"", false, "no identity" },
      };

      for (const NamesCase &c : names)
      {
         if (SMTPForwarding::FromNamesOrganisationOf(c.from, c.identity) != c.names)
            throw std::logic_error(std::string("CopyEnvelopeTester: FromNamesOrganisationOf read ") + c.what + (c.names ? " as naming no such organisation." : " as naming it."));
      }

      // ---- The DMARC passes a message's record holds ----

      struct PassCase { const wchar_t *recorded; const wchar_t *identity; bool passed; const char *what; };
      const PassCase passes[] =
      {
         { L"dmarc=example.test", L"example.test", true, "a pass for the domain" },
         { L"dmarc=example.test", L"mail.example.test", true, "a pass for the organisation of a name in it" },
         { L"dmarc=sub.example.test", L"example.test", true, "a pass for a subdomain of the organisation" },
         { L"DMARC=Example.Test", L"example.test", true, "a pass written in capitals" },
         { L"spf=example.test dmarc=example.test", L"example.test", true, "a pass among other tokens" },
         { L"spf=other.test,dmarc=example.test", L"example.test", true, "a pass after a comma" },
         { L"dmarc=example.test.other", L"example.test", false, "a pass for a domain that only begins with it" },
         { L"dmarc=other.test", L"example.test", false, "a pass for another organisation" },
         { L"spf=example.test", L"example.test", false, "an SPF result" },
         { L"xdmarc=example.test", L"example.test", false, "a token that only ends with the mark" },
         { L"dmarc=", L"example.test", false, "a mark with no domain" },
         { L"", L"example.test", false, "no record" },
         { L"dmarc=example.test", L"", false, "no identity" },
      };

      for (const PassCase &c : passes)
      {
         if (SMTPForwarding::DmarcPassedInOrganisationOf(c.recorded, c.identity) != c.passed)
            throw std::logic_error(std::string("CopyEnvelopeTester: DmarcPassedInOrganisationOf read ") + c.what + (c.passed ? " as no pass." : " as a pass."));
      }

      // ---- Which envelope senders are this server's own identities ----
      // orgHosted is what IsHostedOrganisation answers for the sender's domain (a
      // database call, so it is given here rather than computed): true for a hosted
      // domain and for a subdomain of one.

      struct OwnCase { const wchar_t *sender; const wchar_t *helo; bool orgHosted; bool own; const char *what; };
      const OwnCase owns[] =
      {
         { L"mail.example.test", L"mail.example.test", false, true, "the EHLO name" },
         { L"MAIL.Example.TEST.", L"mail.example.test", false, true, "the EHLO name in capitals with a dot after it" },
         { L"example.test", L"mail.example.test", true, true, "a domain hosted here" },
         { L"bounces.example.test", L"mail.example.test", true, true, "a subdomain of a hosted domain, whose organisation is hosted here" },
         { L"example.test", L"other.test", false, false, "a domain whose organisation is not hosted here and is not the EHLO name" },
         { L"sender.example", L"mail.example.test", false, false, "the sender's own domain" },
         { L"", L"mail.example.test", true, false, "no domain" },
         { L"sender.example", L"", false, false, "no EHLO name" },
      };

      for (const OwnCase &c : owns)
      {
         if (SMTPForwarding::IsOwnEnvelopeDomain(c.sender, c.helo, c.orgHosted) != c.own)
            throw std::logic_error(std::string("CopyEnvelopeTester: IsOwnEnvelopeDomain read ") + c.what + (c.own ? " as none of this server's." : " as one of this server's."));
      }

      // ---- The envelope of a copy ----

      const Values ours = { _T("The CEO <ceo@example.test>") };
      const Values theirs = { _T("A Partner <someone@partner.example>") };
      const String srs = _T("SRS0=abcd=IE=sender.example=x@example.test");

      struct EnvelopeCase
      {
         bool outside; const wchar_t *recorded; const wchar_t *sender; bool orgHosted; const Values *from; String rewritten; const wchar_t *helo;
         bool sentOn; const wchar_t *expected; bool withheld; const char *what;
      };

      const EnvelopeCase envelopes[] =
      {
         { true, L"", L"x@sender.example", false, &ours, srs, L"mail.example.test", true, L"x@sender.example", true,
           "an SRS forward of mail from outside with a From of the forwarder's organisation and no DMARC pass" },
         { true, L"", L"x@sender.example", false, &ours, _T("forwarder@example.test"), L"mail.example.test", true, L"x@sender.example", true,
           "a rewritten forward of mail from outside with a From of the forwarder's organisation and no DMARC pass" },
         { true, L"dmarc=example.test", L"bounce@bounces.example.test", true, &ours, srs, L"mail.example.test", true, srs.c_str(), false,
           "an SRS forward of mail that passed DMARC for the From's organisation" },
         { true, L"dmarc=other.test", L"x@sender.example", false, &ours, srs, L"mail.example.test", true, L"x@sender.example", true,
           "an SRS forward of mail that passed DMARC for another organisation only" },
         { true, L"", L"someone@partner.example", false, &theirs, srs, L"mail.example.test", true, srs.c_str(), false,
           "an SRS forward of mail from outside with a From of another organisation" },
         { false, L"", L"x@sender.example", false, &ours, srs, L"mail.example.test", true, srs.c_str(), false,
           "an SRS forward of mail from inside" },
         { true, L"", L"x@sender.example", false, &ours, String(), L"mail.example.test", true, L"x@sender.example", false,
           "a forward that keeps the sender's own envelope, of another organisation than the From" },
         { true, L"", L"x@bounces.example.test", true, &ours, String(), L"mail.example.test", false, L"x@bounces.example.test", false,
           "a forward of mail from outside that keeps an envelope sender at a subdomain of a hosted domain, with a From of its organisation" },
         { true, L"", L"x@mail.example.test", true, &ours, String(), L"mail.example.test", false, L"x@mail.example.test", false,
           "a forward of mail from outside that keeps an envelope sender at a hosted organisation (also the EHLO name), with a From of its organisation" },
         { true, L"", L"x@mail.example.test", true, &ours, srs, L"mail.example.test", false, L"x@mail.example.test", true,
           "an SRS forward withheld from mail from outside whose envelope sender is at a hosted organisation" },
         { true, L"", L"x@example.test", true, &ours, String(), L"mx.forwarding-host.example", false, L"x@example.test", false,
           "a forward of mail from outside that keeps an envelope sender at a domain hosted here, with a From of its organisation" },
         { true, L"dmarc=example.test", L"x@mail.example.test", true, &ours, String(), L"mail.example.test", true, L"x@mail.example.test", false,
           "a forward of mail with an envelope sender at a hosted organisation that passed DMARC for the From's organisation" },
         { true, L"dmarc=example.test", srs.c_str(), true, &ours, String(), L"mail.example.test", true, srs.c_str(), false,
           "a second forward of mail that passed DMARC for the From's organisation, which keeps the envelope the first forward gave it" },
         { true, L"", srs.c_str(), true, &ours, String(), L"mail.example.test", false, srs.c_str(), false,
           "a second forward of mail from outside with a From of the forwarder's organisation and no DMARC pass, which keeps the envelope the first forward gave it" },
         { true, L"", L"x@mail.example.test", true, &theirs, String(), L"mail.example.test", true, L"x@mail.example.test", false,
           "a forward of mail from outside with an envelope sender at a hosted organisation and a From of another organisation" },
         { false, L"", L"x@mail.example.test", true, &ours, String(), L"mail.example.test", true, L"x@mail.example.test", false,
           "a forward of mail from inside with an envelope sender at a hosted organisation" },
         { true, L"", L"", false, &ours, String(), L"mail.example.test", false, L"", false,
           "a null-sender copy of mail from outside under an EHLO name of the From's organisation" },
         { true, L"", L"", false, &ours, String(), L"mx.forwarding-host.example", true, L"", false,
           "a null-sender copy of mail from outside under an EHLO name of another organisation" },
         { true, L"", L"", false, &theirs, String(), L"mail.example.test", true, L"", false,
           "a null-sender copy of mail from outside with a From of another organisation" },
         { true, L"dmarc=example.test", L"", false, &ours, String(), L"mail.example.test", true, L"", false,
           "a null-sender copy of mail that passed DMARC for the From's organisation" },
         { false, L"", L"", false, &ours, String(), L"mail.example.test", true, L"", false,
           "a null-sender copy of mail from inside" },
      };

      for (const EnvelopeCase &c : envelopes)
      {
         String envelope = _T("left over");
         bool withheld = !c.withheld;
         const bool sentOn = SMTPForwarding::EnvelopeOfCopy(c.outside, c.recorded, c.sender, c.orgHosted, *c.from, c.rewritten, c.helo, envelope, withheld);

         if (sentOn != c.sentOn)
            throw std::logic_error(std::string("CopyEnvelopeTester: ") + c.what + (c.sentOn ? " may not be sent on." : " may be sent on."));

         if (envelope.Compare(c.expected) != 0)
            throw std::logic_error(std::string("CopyEnvelopeTester: ") + c.what + " was given the wrong envelope sender.");

         if (withheld != c.withheld)
            throw std::logic_error(std::string("CopyEnvelopeTester: ") + c.what + (c.withheld ? " did not say the rewrite was withheld." : " said a rewrite was withheld."));
      }

      // ---- A copy that may not be sent on goes to the addresses here alone ----

      std::shared_ptr<Message> copy(new Message(false));
      const wchar_t *const addresses[] = { L"outside@partner.example", L"local@example.test", L"another@partner.example" };
      for (const wchar_t *address : addresses)
      {
         std::shared_ptr<MessageRecipient> recipient(new MessageRecipient());
         recipient->SetAddress(address);
         recipient->SetOriginalAddress(address);
         recipient->SetIsLocalName(String(address).Find(_T("@example.test")) >= 0);
         copy->GetRecipients()->Add(recipient);
      }

      if (!SMTPForwarding::WithholdOutsideRecipients(copy, 0, _T("tester's copy"), String()) || copy->GetRecipients()->GetCount() != 1)
         throw std::logic_error("CopyEnvelopeTester: WithholdOutsideRecipients did not leave a copy the address here alone.");

      // owned by this call: the one recipient the call left on the tester's own copy
      std::vector<std::shared_ptr<MessageRecipient> > &left = copy->GetRecipients()->GetVector();
      if (left[0]->GetAddress().Compare(_T("local@example.test")) != 0)
         throw std::logic_error("CopyEnvelopeTester: WithholdOutsideRecipients did not leave the address here alone.");

      if (SMTPForwarding::WithholdOutsideRecipients(copy, 0, _T("tester's copy"), _T("x@mail.example.test")) || copy->GetRecipients()->GetCount() != 1)
         throw std::logic_error("CopyEnvelopeTester: WithholdOutsideRecipients took an address here off a copy.");

      // ---- What DMARC passed for when the mail arrived, on the message and on a copy ----

      {
         Message arrived(false);
         arrived.SetAuthDomains(_T("dmarc=example.test"));
         if (arrived.GetAuthDomainsOnArrival().Compare(_T("dmarc=example.test")) != 0)
            throw std::logic_error("CopyEnvelopeTester: a message that arrived does not answer its own record as what DMARC passed for on arrival.");

         Message carried(false);
         carried.SetCopiedAuthDomains(_T("dmarc=example.test"));
         if (carried.GetAuthDomainsOnArrival().Compare(_T("dmarc=example.test")) != 0 || !carried.GetAuthDomains().IsEmpty())
            throw std::logic_error("CopyEnvelopeTester: a copy does not answer what it carries of the mail that arrived, or answers it as a record of its own.");

         Message twice(false);
         twice.SetAuthDomains(_T("dmarc=example.test"));
         twice.SetCopiedAuthDomains(_T("DMARC=Example.Test"));
         if (twice.GetAuthDomainsOnArrival().Compare(_T("dmarc=example.test")) != 0)
            throw std::logic_error("CopyEnvelopeTester: a record held by a message and carried by it as well is not answered once.");

         Message differing(false);
         differing.SetAuthDomains(_T("dmarc=example.test"));
         differing.SetCopiedAuthDomains(_T("dmarc=partner.example"));
         if (differing.GetAuthDomainsOnArrival().Compare(_T("dmarc=example.test,dmarc=partner.example")) != 0)
            throw std::logic_error("CopyEnvelopeTester: a record of a message's own and a different one it carries are not both answered.");

         Message none(false);
         if (!none.GetAuthDomainsOnArrival().IsEmpty())
            throw std::logic_error("CopyEnvelopeTester: a message with no record answers one.");
      }

      // ---- Every copy carries it, apart from the record of the message that arrived ----

      {
         std::shared_ptr<Message> arrived(new Message(false));
         arrived->SetFromOutside(true);
         arrived->SetAuthDomains(_T("dmarc=example.test"));

         std::shared_ptr<Message> forwarded = PersistentMessage::CreateCopy_(arrived, 0);
         if (!forwarded || !forwarded->GetFromOutside() || !forwarded->GetAuthDomains().IsEmpty() ||
             forwarded->GetCopiedAuthDomains().Compare(_T("dmarc=example.test")) != 0)
            throw std::logic_error("CopyEnvelopeTester: a forward's copy does not carry what DMARC passed for when the mail arrived, apart from the record of the message that arrived.");

         std::shared_ptr<Message> forwardedAgain = PersistentMessage::CreateCopy_(forwarded, 0);
         if (!forwardedAgain || forwardedAgain->GetAuthDomainsOnArrival().Compare(_T("dmarc=example.test")) != 0)
            throw std::logic_error("CopyEnvelopeTester: a copy of a forward's copy does not carry what DMARC passed for when the mail arrived.");

         std::shared_ptr<Message> recipientsCopy = PersistentMessage::CreateCopy_(arrived, 17);
         if (!recipientsCopy || recipientsCopy->GetAuthDomainsOnArrival().Compare(_T("dmarc=example.test")) != 0)
            throw std::logic_error("CopyEnvelopeTester: a recipient's copy does not carry what DMARC passed for when the mail arrived.");

         std::shared_ptr<Message> unchecked(new Message(false));
         unchecked->SetFromOutside(true);
         std::shared_ptr<Message> uncheckedCopy = PersistentMessage::CreateCopy_(unchecked, 0);
         if (!uncheckedCopy || !uncheckedCopy->GetAuthDomainsOnArrival().IsEmpty())
            throw std::logic_error("CopyEnvelopeTester: a copy of mail that passed DMARC for nothing carries a pass.");
      }

      // ---- On a queue row ----

      {
         Message forwarded(false);
         forwarded.SetFromOutside(true);
         forwarded.SetCopiedAuthDomains(_T("dmarc=example.test"));

         const String column = PersistentMessage::OriginColumn(forwarded);
         if (column.Compare(_T("outside,copied=dmarc=example.test")) != 0)
            throw std::logic_error("CopyEnvelopeTester: the queue row of a forward's copy is \"" + std::string(AnsiString(column).c_str()) + "\".");

         Message read(false);
         PersistentMessage::ReadOriginColumn(column, read);
         if (!read.GetFromOutside() || !read.GetAuthDomains().IsEmpty() || read.GetCopiedAuthDomains().Compare(_T("dmarc=example.test")) != 0)
            throw std::logic_error("CopyEnvelopeTester: the queue row of a forward's copy does not read back as what it carries.");

         Message both(false);
         both.SetFromOutside(true);
         both.SetAuthDomains(_T("dmarc=example.test"));
         both.SetCopiedAuthDomains(_T("dmarc=partner.example,dmarc=other.example"));

         Message bothRead(false);
         PersistentMessage::ReadOriginColumn(PersistentMessage::OriginColumn(both), bothRead);
         if (bothRead.GetAuthDomains().Compare(_T("dmarc=example.test")) != 0 ||
             bothRead.GetCopiedAuthDomains().Compare(_T("dmarc=partner.example,dmarc=other.example")) != 0)
            throw std::logic_error("CopyEnvelopeTester: a queue row holding a record and what a copy carries does not read back as both.");

         // Too long for the column: dropped whole, and the mark of mail from outside kept.
         Message crowded(false);
         crowded.SetFromOutside(true);
         crowded.SetCopiedAuthDomains(String(_T("dmarc=")) + String(std::wstring(245, L'd')) + _T(".example"));
         if (PersistentMessage::OriginColumn(crowded).Compare(_T("outside")) != 0)
            throw std::logic_error("CopyEnvelopeTester: what a copy carries, too long for the queue row, was not dropped whole with the mark kept.");

         // A row written before the marks existed: the domains alone, read as they were.
         Message legacy(false);
         PersistentMessage::ReadOriginColumn(_T("dmarc=example.net"), legacy);
         if (legacy.GetFromOutside() || legacy.GetAuthDomains().Compare(_T("dmarc=example.net")) != 0 || !legacy.GetCopiedAuthDomains().IsEmpty())
            throw std::logic_error("CopyEnvelopeTester: a queue row of the domains alone does not read as it did.");
      }

      // ---- In the quarantine ----

      {
         const String guid = _T("{0F8E4C2A-6B1D-4E5F-9A3C-7D2B8E1F4A6C}");

         QuarantineStore::StoredMarks marks;
         marks.from_outside = true;
         marks.dmarc_passed_for_from = true;
         const String name = QuarantineStore::StoredName(guid, marks);
         if (name.Compare(guid + _T(".dmarc.outside.eml")) != 0)
            throw std::logic_error("CopyEnvelopeTester: the stored name of a held message that passed DMARC for its From's domain is \"" + std::string(AnsiString(name).c_str()) + "\".");

         const QuarantineStore::StoredMarks read = QuarantineStore::MarksOfStoredName(_T("2026-10-15\\") + name);
         if (!read.known || !read.from_outside || !read.dmarc_passed_for_from)
            throw std::logic_error("CopyEnvelopeTester: the stored name of a held message that passed DMARC for its From's domain does not read back as one.");

         if (QuarantineStore::MarksOfStoredName(_T("2026-10-15/") + guid + _T(".spf-pass.outside.eml")).dmarc_passed_for_from)
            throw std::logic_error("CopyEnvelopeTester: a stored name without the word reads as a DMARC pass.");

         if (QuarantineStore::FromDomainAsDmarcReadsIt("From: The CEO <CEO@Example.Test>\r\nTo: someone@example.com\r\n\r\n").Compare(_T("example.test")) != 0)
            throw std::logic_error("CopyEnvelopeTester: FromDomainAsDmarcReadsIt did not read the From's domain.");

         if (!QuarantineStore::FromDomainAsDmarcReadsIt("To: someone@example.com\r\n\r\n").IsEmpty() || !QuarantineStore::FromDomainAsDmarcReadsIt("").IsEmpty())
            throw std::logic_error("CopyEnvelopeTester: FromDomainAsDmarcReadsIt read a domain from a header with no From.");

         struct RecordCase { const wchar_t *recorded; const wchar_t *domain; bool records; const char *what; };
         const RecordCase records[] =
         {
            { L"dmarc=example.test", L"example.test", true, "a pass for the domain" },
            { L"DMARC=Example.Test", L"example.test", true, "a pass written in capitals" },
            { L"dmarc=partner.example,dmarc=example.test", L"example.test", true, "a pass after a comma" },
            { L"dmarc=sub.example.test", L"example.test", false, "a pass for a subdomain of it" },
            { L"dmarc=example.test", L"sub.example.test", false, "a pass for the domain a subdomain is of" },
            { L"dmarc=example.testing", L"example.test", false, "a pass for a domain that only begins with it" },
            { L"xdmarc=example.test", L"example.test", false, "a word that only ends with the mark" },
            { L"", L"example.test", false, "no record" },
            { L"dmarc=example.test", L"", false, "no domain" },
         };

         for (const RecordCase &c : records)
         {
            if (QuarantineStore::RecordsDmarcPassFor(c.recorded, c.domain) != c.records)
               throw std::logic_error(std::string("CopyEnvelopeTester: RecordsDmarcPassFor read ") + c.what + (c.records ? " as no pass." : " as a pass."));
         }
      }
   }
}

