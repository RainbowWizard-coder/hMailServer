// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "SieveNotifyResponder.h"
#include "SieveVacationResponder.h"
#include "../Util/RestrictedDelivery.h"

#include "../BO/Account.h"
#include "../BO/Message.h"
#include "../BO/MessageData.h"
#include "../BO/MessageRecipients.h"
#include "../Persistence/PersistentMessage.h"
#include "../Persistence/MessageStore.h"
#include "../../SMTP/RecipientParser.h"
#include "../Util/FileUtilities.h"
#include "../Util/Time.h"
#include "../Application/Application.h"
#include "../../SMTP/SMTPConfiguration.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   bool
   SieveNotifyResponder::Send(std::shared_ptr<const Account> account, const SieveNotifyDecision &decision)
   {
      if (!account || decision.to.IsEmpty())
         return false;

      // Restricted delivery (Util/RestrictedDelivery.h): a notification is
      // the account writing - its body carries the original's From and
      // Subject and the script's own text - and a three-line script must not
      // walk around what the account's forward box cannot, as the redirect
      // action is held to in SMTPForwarding.
      if (RestrictedDelivery::Check(account, decision.to, RestrictedDelivery::Outbound) == RestrictedDelivery::Refused)
      {
         LOG_DEBUG("Sieve notify: no notification was sent, restricted delivery - " + account->GetAddress() +
            " may send to named domains only, and " + decision.to + " is not in one.");
         return false;
      }

      int loopLimit = Configuration::Instance()->GetSMTPConfiguration()->GetRuleLoopLimit();
      if (decision.loopCount >= loopLimit)
      {
         // The limit SieveVacationResponder::Respond holds a vacation reply to, and
         // RuleApplier::IsGeneratedResponseAllowed rule-generated mail. A notification
         // is generated mail like them, and until 6 October 2026 it alone went out
         // whatever the count, carrying count + 1: a signed overflow for a sender who
         // wrote 2147483647, which came out as the most negative int, so that every
         // forward of the notification had that far to climb to the limit. Past this
         // check the count is below the limit, and the + 1 below is an int.
         String message;
         message.Format(_T("Sieve notify: no notification for %s, the message has already passed through %d ")
                        _T("generated messages (the rule loop limit)."),
                        String(account->GetAddress()).c_str(), decision.loopCount);
         LOG_APPLICATION(message);
         return false;
      }

      try
      {
         std::shared_ptr<Message> notification = std::shared_ptr<Message>(new Message());
         notification->SetState(Message::Delivering);

         // Null envelope return path (RFC 5436 2.7.1, same reasoning as RFC 3834
         // for vacation): a bounce of a notification must die, not converse.
         notification->SetFromAddress(_T(""));

         const String fileName = MessageStore::WorkingFile(notification);

         MessageData data;
         if (!data.LoadFromMessage(fileName, notification))
         {
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5902, "SieveNotifyResponder::Send",
               "Could not initialise the message data for a Sieve notification.");
            return false;
         }

         data.GenerateMessageID();
         data.SetSentTime(Time::GetCurrentMimeDate());
         // :from is held to the rule a vacation reply's is (SieveFromAddress): an
         // address that reaches this account, or else the account's own address, with
         // a line in the application log. The tag is the notify command's, not the
         // method's: a method added beside mailto is to take its sender from here. Until
         // 6 October 2026 a notification said it was from whatever :from said, so that
         // any account with a script could send mail through this server as anyone -
         // and, for a domain this server signs for, with that domain's DKIM signature.
         data.SetFrom(SieveFromAddress::Resolve(account, decision.from, SieveFromAddress::Action::Notify));
         data.SetTo(decision.to);
         data.SetSubject(decision.subject);

         // RFC 5436 2.7.1: the marker that stops the next auto-responder from
         // answering a notification. "auto-notified" rather than "auto-replied",
         // so the two kinds of generated mail stay distinguishable in a trace.
         data.SetFieldValue(_T("Auto-Submitted"), _T("auto-notified"));
         data.SetFieldValue(_T("X-Auto-Response-Suppress"), _T("All"));

         // RFC 5435 3.2's three-level importance, in the header vocabulary mail
         // clients actually read.
         if (decision.importance == 1)
            data.SetFieldValue(_T("Importance"), _T("High"));
         else if (decision.importance == 3)
            data.SetFieldValue(_T("Importance"), _T("Low"));

         // Below the limit, held to it above, so + 1 is an int.
         data.SetRuleLoopCount(decision.loopCount + 1);

         String body;
         body += _T("You have new mail.\r\n\r\n");
         if (!decision.originalFrom.IsEmpty())
            body += _T("From: ") + decision.originalFrom + _T("\r\n");
         if (!decision.originalSubject.IsEmpty())
            body += _T("Subject: ") + decision.originalSubject + _T("\r\n");

         data.SetFieldValue(_T("Content-Type"), _T("text/plain; charset=\"utf-8\""));
         data.SetBody(body);

         if (!data.Write(fileName))
         {
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5902, "SieveNotifyResponder::Send",
               "Could not write the message file for a Sieve notification.");
            return false;
         }

         bool recipientOK = false;
         RecipientParser recipientParser;
         recipientParser.CreateMessageRecipientList(decision.to, notification->GetRecipients(), recipientOK);

         if (notification->GetRecipients()->GetCount() == 0)
         {
            // No deliverable recipient: leaving the file would leak one message
            // file per attempt with no database row ever collecting it.
            FileUtilities::DeleteFile(fileName);

            String message;
            message.Format(_T("Sieve notify: no notification was sent, no deliverable recipient could be built from '%s'."),
               decision.to.c_str());
            LOG_APPLICATION(message);
            return false;
         }

         if (!PersistentMessage::SaveObject(notification))
         {
            FileUtilities::DeleteFile(fileName);
            return false;
         }

         Application::Instance()->SubmitPendingEmail();

         return true;
      }
      catch (...)
      {
         // A notification is a courtesy; the delivery it describes must never
         // depend on it.
         return false;
      }
   }
}
