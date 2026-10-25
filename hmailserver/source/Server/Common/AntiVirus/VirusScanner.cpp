// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#include "stdafx.h"
#include "VirusScanner.h"

#include "../../SMTP/SMTPConfiguration.h"
#include "../Mime/Mime.h"
#include "../BO/Message.h"
#include "../Util/GUIDCreator.h"
#include "../Util/File.h"
#include "../BO/MessageData.h"

// The scanners:

#include "ClamAVVirusScanner.h"
#include "ClamWinVirusScanner.h"
#include "CustomVirusScanner.h"

#include "../Util/MessageUtilities.h"
#include "../Util/SealedMessage.h"

// For attachment blocking

#include "../BO/BlockedAttachments.h"
#include "../BO/BlockedAttachment.h"
#include "../BO/Attachment.h"
#include "../BO/Attachments.h"
#include "../BO/MessageData.h"
#include "../BO/ServerMessages.h"

#include "../Persistence/PersistentServerMessage.h"
#include "../Persistence/PersistentMessage.h"
#include "../Persistence/MessageStore.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   LONG VirusScanner::running_scanners_ = 0;

   VirusScanner::VirusScannerAutoCount::VirusScannerAutoCount() :
      holds_slot_(VirusScanner::WaitForFreeScanner_())
   {

   }

   VirusScanner::VirusScannerAutoCount::~VirusScannerAutoCount()
   {
      // Give back the slot only if we took one. If WaitForFreeScanner_ gave up, no
      // slot was ever allocated and there is nothing to release.
      if (holds_slot_)
      {
         VirusScanner::DecreaseCounter();
      }
   }

   bool
   VirusScanner::VirusScannerAutoCount::GetHoldsSlot() const
   {
      return holds_slot_;
   }

   void
   VirusScanner::ResetCounter()
   {
      InterlockedExchange(&running_scanners_, 0);
   }

   bool
   VirusScanner::GetVirusScanningEnabled()
   {
      AntiVirusConfiguration &antiVirusConfig = Configuration::Instance()->GetAntiVirusConfiguration();

      if (antiVirusConfig.GetClamAVEnabled() ||
          antiVirusConfig.ClamWinEnabled() ||
          antiVirusConfig.GetCustomScannerEnabled())
          return true;
      else
         return false;
   }

   /*
      This function will wait until there's a free virus scanner available.
      At most, hMailServer runs 10 virus scanners in parallel.

      Returns true if a scanner slot was allocated. The caller then holds one of
      the MaxRunningScanners slots and must give it back through DecreaseCounter().

      Returns false if we gave up waiting - either the wait timed out or the
      allocation lost the race too many times. No slot has been allocated in that
      case, so the caller must NOT give one back.

      Giving up means the scan runs anyway, over the cap. That is deliberate and
      unchanged: the alternative is to fail the scan, and for a mail server it is
      much worse to bounce or hold a message because the scanners are busy than it
      is to run a few more scanners than the cap allows for a while. The bug that
      was fixed here was the accounting, not the give-up policy.
   */
   bool
   VirusScanner::WaitForFreeScanner_()
   {
      int waitTime = 0;

      for (int failedCount = 0; failedCount < 10; failedCount++)
      {
         // Read through an interlocked operation like every other access to this
         // counter. A plain read races with the increments and decrements below,
         // and a stale value here lets more scanners run than the cap allows.
         LONG currentCount = InterlockedCompareExchange(&running_scanners_, 0, 0);

         while (currentCount >= MaxRunningScanners)
         {
            if (waitTime % 5 == 0)
            {
               LOG_DEBUG("Waiting for free virus scanner...");
            }
            
            waitTime++;
            Sleep(1000);

            if (waitTime > 60)
            {
               LOG_DEBUG("Waiting more than 60 seconds for an available virus scanner. Giving up waiting.");
               return false;
            }

            currentCount = InterlockedCompareExchange(&running_scanners_, 0, 0);
         }

         // make sure that no other thread is changing the value at the same time.
         // if that happens, we need to continue waiting for a new thread. unless
         // a compare was used here, this function could first wait and then call
         // InterlockedIncrement twice even though only one was allowed.
         LONG result = InterlockedCompareExchange(&running_scanners_, currentCount + 1, currentCount);
         if (result == currentCount)
         {
            // woho.
            return true;
         }
      }

      LOG_DEBUG("Waited for available virus scanner, tried to allocate, failed 10 times. Giving up waiting.");

      return false;
   }

   void
   VirusScanner::DecreaseCounter()
   {
      // Never let the counter drop below zero. VirusScannerAutoCount makes the
      // increment and the decrement symmetric, but ResetCounter() can still zero
      // the counter while scans are in progress - it runs when the server is
      // (re)initialized - and the guards for those scans will decrement afterwards.
      // Without a floor, those decrements would leave the counter negative for the
      // rest of the process lifetime and the cap would stop limiting anything.
      //
      // Read and write through interlocked operations only, like every other access
      // to this counter.
      for (;;)
      {
         LONG currentCount = InterlockedCompareExchange(&running_scanners_, 0, 0);

         if (currentCount <= 0)
         {
            // Nothing to give back.
            return;
         }

         if (InterlockedCompareExchange(&running_scanners_, currentCount - 1, currentCount) == currentCount)
            return;

         // Another thread changed the counter while we were looking at it. Try again.
      }
   }

   bool
   VirusScanner::Scan(std::shared_ptr<Message> pMessage, String &virusName, bool *scannerFailed)
   {
      if (scannerFailed)
         *scannerFailed = false;

      AntiVirusConfiguration &antiVirusConfig = Configuration::Instance()->GetAntiVirusConfiguration();

      int iMaxVirusScanSizeKB = antiVirusConfig.GetVirusScanMaxSize();
      int iMessageSizeKB = pMessage->GetSize() / 1024;
      if (iMaxVirusScanSizeKB > 0 &&
          iMessageSizeKB > iMaxVirusScanSizeKB)
      {
         // Message is to big. Don't scan.
         return false;
      }

      // Prevent too many scanners from running at once. The guard waits for a free
      // scanner slot, takes it, and gives it back when it goes out of scope - if and
      // only if it managed to take one.
      VirusScannerAutoCount autoCounter;

      if (!autoCounter.GetHoldsSlot())
      {
         // We gave up waiting and are scanning without a slot, so more than
         // MaxRunningScanners scans may be running right now. The message is still
         // scanned - see WaitForFreeScanner_ - but the scanners are clearly backed up
         // and that is worth having in the log.
         LOG_DEBUG("Scanning message without an available virus scanner slot.");
      }

      // First scan the entire file.
      String sLongFilename = MessageStore::WorkingFile(pMessage);

      bool scannerError = false;

      VirusScanningResult result = ScanFile_(sLongFilename, scannerError);
      if (result.GetVirusFound())
      {
         virusName = result.GetDetails();
         return true;
      }

      // A scanner that errored on the whole message file means this message has
      // not been examined, whatever the per-attachment pass below goes on to say.
      if (scannerError && scannerFailed)
         *scannerFailed = true;


      // Read message, extract attachments,
      std::shared_ptr<MimeBody> pMimeBody = std::shared_ptr<MimeBody>(new MimeBody);

      // The result used to be discarded, and a message that could not be parsed then
      // produced an EMPTY attachment list - so this second pass silently scanned
      // nothing and the message was reported clean by it. The whole file has already
      // been scanned above, so this is not "delivered unscanned"; what is lost is the
      // per-attachment pass, which is the one that matters for a scanner that does not
      // decode MIME itself. Reported rather than made fatal: refusing the message on a
      // transient file lock would turn a degraded scan into rejected mail.
      const MimeLoadResult loadResult = pMimeBody->LoadFromFile(sLongFilename);

      if (loadResult != MimeLoadResult::Loaded)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6001, "VirusScanner::Scan",
            Formatter::Format("The message {0} could not be parsed for per-attachment virus scanning, so only the whole message file was scanned. Any attachment the scanner cannot decode by itself has therefore not been examined separately.",
               sLongFilename));

         return false;
      }

      // Read once here, written up at the end: see the comment there.
      const char *topLevelContentType = pMimeBody->GetContentType();
      const bool opaqueMessage = SealedMessage::IsOpaque(pMimeBody);

      std::list<std::shared_ptr<MimeBody> > oList;
      pMimeBody->GetAttachmentList(pMimeBody, oList);

      auto iter = oList.begin();

      while (iter != oList.end())
      {
         std::shared_ptr<MimeBody> pBody = (*iter);
         
         // Create a temporary filename.
         sLongFilename = FileUtilities::Combine(IniFileSettings::Instance()->GetTempDirectory(), GUIDCreator::GetGUID() + ".tmp");

         // Likewise discarded before this. A failed write leaves the temp file missing
         // or empty and ScanFile_ then scans that - and reports it clean, which is the
         // scanner agreeing that nothing is wrong with a file it never saw.
         if (!pBody->WriteToFile(sLongFilename))
         {
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6002, "VirusScanner::Scan",
               Formatter::Format("An attachment of {0} could not be written to {1} for scanning, so it has not been examined. The rest of the message has been.",
                  MessageStore::WorkingFile(pMessage), sLongFilename));

            FileUtilities::DeleteFile(sLongFilename);
            iter++;
            continue;
         }

         bool attachmentScannerError = false;

         VirusScanningResult result = ScanFile_(sLongFilename, attachmentScannerError);
         if (result.GetVirusFound())
         {
            virusName = result.GetDetails();
            FileUtilities::DeleteFile(sLongFilename);
            return true;
         }

         if (attachmentScannerError && scannerFailed)
            *scannerFailed = true;

         FileUtilities::DeleteFile(sLongFilename);
         iter++;
      }

      // "No virus found" in a message this server cannot read means "nobody
      // looked", and until now the two were written down identically - which is
      // to say not at all. A scanner handed the ciphertext of an S/MIME or
      // PGP/MIME message examines base64, finds nothing, and the message is
      // delivered with a clean verdict that was never a verdict.
      //
      // Deliberately NOT routed to scannerFailed. That flag means an enabled
      // scanner could not complete, and AVFailAction acts on it: setting it
      // here would make an installation configured to hold unscannable mail
      // hold every encrypted message it receives, which is not a decision this
      // fix gets to take on an administrator's behalf. One line in the ordinary
      // log, and a paragraph in hmailserver/docs/SealedMail.md saying that an
      // encrypted message is not scanned and cannot be.
      //
      // Written here, after the whole file AND every part have been scanned, so
      // that it is only written for a message that really was scanned and
      // really did come back clean: a virus found in the covering parts returns
      // true above and this line is never reached. And it asks IsOpaque, not
      // IsSealed: a multipart/signed message carries its text in the clear and
      // the scanner read it, so a clean verdict on one is a verdict, and saying
      // otherwise would be the same lie in the other direction.
      if (opaqueMessage)
      {
         LOG_APPLICATION(Formatter::Format("VirusScanner - Message {0} is encrypted ({1}), so its content COULD NOT BE EXAMINED for viruses. What was scanned was the ciphertext. A clean result here means nothing was found in bytes nobody can read, not that the message is clean.",
            pMessage->GetID(), String(topLevelContentType)));
      }

      return false;
   }

   void
   VirusScanner::ReportVirusFound(std::shared_ptr<Message> pMessage)
   {
      const String fileName = MessageStore::WorkingFile(pMessage);

      std::shared_ptr<MessageData> pMsgData = std::shared_ptr<MessageData> (new MessageData());
      pMsgData->LoadFromMessage(fileName, pMessage);

      String sMessage;
      sMessage.Format(_T("Virus found in message from %s. Taking actions"), pMsgData->GetFrom().c_str());

      Logger::Instance()->LogApplication(sMessage);
   }

   void
   VirusScanner::BlockAttachments(std::shared_ptr<Message> message)
   {
      std::shared_ptr<BlockedAttachments> blocked_attachments_config = HM::Configuration::Instance()->GetBlockedAttachments();
      std::vector<std::shared_ptr<BlockedAttachment> > blocked_attachment_wildcards = blocked_attachments_config->GetSnapshot();

      const String file_name = MessageStore::WorkingFile(message);

      std::shared_ptr<MessageData> message_data = std::shared_ptr<MessageData>(new MessageData());

      // The result was discarded. A message that could not be read parses as no
      // attachments, the loop below then does nothing and the file is left
      // alone - so the outcome was accidentally safe rather than deliberately
      // so, and silent either way. An administrator whose blocked-attachment
      // list is not being applied to some messages has a right to know which.
      if (!message_data->LoadFromMessage(file_name, message))
      {
         LOG_APPLICATION(Formatter::Format("VirusScanner - Message {0} could not be read, so it was not checked against the blocked-attachment list. It has been left exactly as it arrived.",
            message->GetID()));
         return;
      }

      // A message that is encrypted end to end is left alone.
      //
      // This is not a refinement. The ciphertext of an S/MIME message is a part
      // named smime.p7m, and the ciphertext of a PGP/MIME one is usually named
      // encrypted.asc or msg.asc - and MimeBody::IsAttachment calls any
      // non-text, non-multipart part an attachment, so to the loop below they
      // are ordinary attachments with ordinary file names. An administrator who
      // blocks *.p7m, or *.asc, or the *.* that some do while chasing one
      // extension, therefore had every encrypted message silently gutted: the
      // ciphertext replaced with a notice, the file rewritten, no log line, no
      // error, and nothing the recipient or the sender could do about it,
      // because the plaintext existed nowhere else.
      //
      // Blocking exists to stop executable content reaching a reader. Encrypted
      // content is not readable by anything on this server, the reader's client
      // will not run it either, and the only thing the rewrite achieves is the
      // destruction of a message. So: unchanged, delivered, and said out loud.
      //
      // IsOpaque and not IsSealed, and the difference is a security boundary.
      // Nothing here verifies a signature, so "multipart/signed" at the top of
      // a message is a claim any sender can make, and GetAttachmentList
      // recurses into the signed part: a multipart/signed wrapping a
      // multipart/mixed with invoice.exe in it must still be blocked, or the
      // list is one header away from being switched off by the people it exists
      // to stop. A signed message is therefore checked exactly as before. What
      // changes is that the log says the rewrite invalidated the signature,
      // which a client that checks will report as tampering - the honest
      // outcome when the administrator's list and a sender's signature disagree.
      //
      // And IsOpaque looks at the body, not at the label, for the same reason:
      // "multipart/encrypted" is as much a claim any sender can make as
      // "multipart/signed", and GetAttachmentList recurses into its parts
      // whatever the label says. Only a message whose parts are exactly a real
      // encrypted message's - the ciphertext and, for PGP/MIME, the version
      // part, under the names clients give them - is left alone; with no other
      // part in it, there is nothing else for the list to find.
      if (SealedMessage::IsOpaque(message_data))
      {
         LOG_APPLICATION(Formatter::Format("VirusScanner - Message {0} from {1} is encrypted ({2}), so it was NOT checked against the blocked-attachment list: its ciphertext is one attachment by MIME type and replacing it would destroy the message beyond recovery. The message has been delivered exactly as it arrived.",
            message->GetID(), message->GetFromAddress(), message_data->GetFieldValue(_T("Content-Type"))));
         return;
      }

      if (SealedMessage::IsOpaqueContentType(message_data->GetFieldValue(_T("Content-Type"))))
      {
         LOG_APPLICATION(Formatter::Format("VirusScanner - Message {0} from {1} is labelled encrypted ({2}) but its body is not an encrypted message, so it has been checked against the blocked-attachment list like any other.",
            message->GetID(), message->GetFromAddress(), message_data->GetFieldValue(_T("Content-Type"))));
      }

      const bool signedMessage = SealedMessage::IsSealed(message_data);

      // The detached signature of a signed message is not a file the sender
      // attached; it is what makes the message verifiable. It is left alone,
      // because replacing it removes nothing and only makes every client report
      // the message as tampered with - and the real attachments of the same
      // message are still checked below.
      //
      // Which part that is, is decided from the structure and not from the
      // part's own Content-Type: any part anywhere in the message can call
      // itself application/pkcs7-signature, and "invoice.exe" under that type
      // was spared the list whatever it held. SealedMessage names the one part
      // that is a signature - the second of the two of a real multipart/signed,
      // named and made as a signature is - and only that part is skipped.
      const std::shared_ptr<MimeBody> signaturePart = SealedMessage::DetachedSignaturePart(message_data->GetMimeMessage());

      std::shared_ptr<Attachments> pAttachments = message_data->GetAttachments();

      bool changes_made = false;
      String blocked_names;

      // owned by this call: the attachments of the message this scan parsed, and Attachments::GetVector hands out a copy
      for (auto attachment : pAttachments->GetVector())
      {
         if (signaturePart && attachment->GetMimeBody() == signaturePart)
            continue;

         // Check if attachment matches blocked file - as it is named, and as
         // Windows will save it. Trailing dots and white space are dropped by
         // Win32 path normalisation and by the save dialogs of browsers and mail
         // clients, so "invoice.exe." and "invoice.exe " are saved as
         // invoice.exe; a list entry *.exe that looked only at the name as sent
         // was one character away from never matching.
         String saved_name = attachment->GetFileName();
         saved_name.TrimRight(_T(". \t"));

         auto matching_wildcard = std::find_if(blocked_attachment_wildcards.begin(), blocked_attachment_wildcards.end(), [&attachment, &saved_name](std::shared_ptr<BlockedAttachment> blocked_attachment)
            {
               String wildcard_text = blocked_attachment->GetWildcard();

               return StringParser::WildcardMatchNoCase(wildcard_text, attachment->GetFileName()) ||
                      StringParser::WildcardMatchNoCase(wildcard_text, saved_name);
            });

         if (matching_wildcard != blocked_attachment_wildcards.end())
         {
            // Attachment should be blocked.
            String updated_message_body = Configuration::Instance()->GetServerMessages()->GetMessage("ATTACHMENT_REMOVED");

            // Replace macros.
            updated_message_body.Replace(_T("%MACRO_FILE%"), attachment->GetFileName());

            if (!blocked_names.IsEmpty())
               blocked_names += _T(", ");
            blocked_names += attachment->GetFileName();

            // Add the new
            attachment->SetFileName(attachment->GetFileName() + ".txt");
            attachment->SetContent(updated_message_body);

            changes_made = true;
         }

      }

      if (changes_made)
      {
         message_data->WriteReported(file_name, "The removal of virus-bearing attachments");

         // Update the size of the message.
         message->SetSize(MessageStore::FileSize(file_name));

         // Said in the ordinary log, once, naming the message and every part
         // whose content was replaced. This function has rewritten people's
         // mail since the feature existed and left no record of having done it,
         // so "where did my attachment go" could only be answered by comparing
         // the delivered message against a copy the sender still had. The
         // recipient is told inside the message; the administrator was told
         // nowhere.
         if (signedMessage)
         {
            LOG_APPLICATION(Formatter::Format("VirusScanner - Message {0} from {1} is signed ({2}); the attachment(s) {3} matched the blocked-attachment list and their content has been replaced with a notice, which invalidates the signature. The recipient's client will report the message as tampered with.",
               message->GetID(), message->GetFromAddress(), message_data->GetFieldValue(_T("Content-Type")), blocked_names));
         }
         else
         {
            LOG_APPLICATION(Formatter::Format("VirusScanner - Message {0} from {1}: the attachment(s) {2} matched the blocked-attachment list. Their content has been replaced with a notice and the message rewritten.",
               message->GetID(), message->GetFromAddress(), blocked_names));
         }
      }


   }

   VirusScanningResult
   VirusScanner::ScanFile_(const String &fileName, bool &scannerError)
   {
      scannerError = false;

      AntiVirusConfiguration &antiVirusConfig = Configuration::Instance()->GetAntiVirusConfiguration();

      if (antiVirusConfig.ClamWinEnabled())
      {
         VirusScanningResult result = ClamWinVirusScanner::Scan(fileName);

         if (result.GetVirusFound())
            return result;
         else if (result.GetErrorOccured())
         {
            scannerError = true;
            ReportScanningError_(result);
         }
      }

      if (antiVirusConfig.GetCustomScannerEnabled())
      {
         VirusScanningResult result = CustomVirusScanner::Scan(fileName);

         if (result.GetVirusFound())
            return result;
         else if (result.GetErrorOccured())
         {
            scannerError = true;
            ReportScanningError_(result);
         }
      }

      if (antiVirusConfig.GetClamAVEnabled())
      {
         VirusScanningResult result = ClamAVVirusScanner::Scan(fileName);

         if (result.GetVirusFound())
            return result;
         else if (result.GetErrorOccured())
         {
            scannerError = true;
            ReportScanningError_(result);
         }
      }

      // No virus found - but if any enabled scanner errored above, that verdict
      // rests on the scanners that did answer, and possibly on none at all. The
      // flag is what lets the delivery path tell the two apart.
      return VirusScanningResult(VirusScanningResult::NoVirusFound, "");
   }

   void 
   VirusScanner::ReportScanningError_(const VirusScanningResult &scanningResult)
   {
      ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5406, scanningResult.GetErrorMessageSource(), scanningResult.GetDetails());
   }
}
