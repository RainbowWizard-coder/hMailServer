// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#include "stdafx.h"
#include "../Common/Util/RestrictedDelivery.h"
#include "SMTPVacationMessageCreator.h"

#include "../Common/BO/Account.h"
#include "../Common/BO/Accounts.h"
#include "../Common/BO/Alias.h"
#include "../Common/BO/Domain.h"
#include "../Common/BO/Message.h"
#include "../Common/BO/MessageData.h"
#include "../Common/BO/RemoteDomainPolicies.h"
#include "../Common/Cache/CacheContainer.h"
#include "../Common/Mime/Mime.h"
#include "../Common/Rules/RuleGuard.h"
#include "../Common/Util/Strings/Formatter.h"
#include "../Common/Util/Time.h"
#include "../Common/Util/Parsing/StringParser.h"
#include "../Common/Persistence/PersistentMessage.h"
#include "../Common/Persistence/MessageStore.h"
#include "../Common/SQL/DALConnection.h"
#include "../Common/SQL/DALRecordset.h"
#include "../Common/SQL/SQLCommand.h"
#include "../Common/SQL/SQLStatement.h"
#include "RecipientParser.h"
#include "SMTPConfiguration.h"
#include "../Common/SQL/DatabaseConnectionManager.h"

#ifndef HM_PLATFORM_POSIX
#include "../COM/COMError.h"
#endif

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include "../Common/BO/MessageRecipients.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{

   SMTPVacationMessageCreator::SMTPVacationMessageCreator()
   {

   }

   SMTPVacationMessageCreator::~SMTPVacationMessageCreator()
   {

   }


   void
   SMTPVacationMessageCreator::CreateVacationMessage(std::shared_ptr<const Account> recipientAccount,
                                        const String &sToAddress,
                                        const String &sVacationSubject,
                                        const String &sVacationMessage,
                                        const std::shared_ptr<Message> pOriginalMessage,
                                        int repeatAfterDays)
   {
      // RFC 3834 2: never answer a message with a null envelope return path
      // (MAIL FROM:<>). That is a bounce or another automatic notice, and answering
      // one is how two servers hand each other replies until one gives up. This was
      // previously enforced one level down, as CanSendVacationMessage_'s "we need a
      // recipient" check; it is a loop guard, so it is stated as one.
      if (sToAddress.IsEmpty())
         return;

      // The remote domain policy's first Exchange switch: whether this server
      // may send an automatic reply to this domain at all. An absence notice is
      // the message a regulated desk and a discussion list both least want from
      // us, and an administrator who has said no to a domain has said no to
      // every account's out-of-office as well as to the domain-wide one - both
      // arrive here.
      if (!RemoteDomainPolicies::AutomaticRepliesPermitted(sToAddress))
      {
         LOG_DEBUG(_T("No out-of-office message was sent: the remote domain policy for ") + StringParser::ExtractDomain(sToAddress) + _T(" does not permit automatic replies."));
         return;
      }

      // Restricted delivery (Common/Util/RestrictedDelivery.h): the reply's
      // recipient is the sender being answered, and a restricted account does
      // not write to a stranger, not even to say it is away. Silent, with a
      // log line; the message itself was delivered or refused on its own
      // terms above this.
      if (RestrictedDelivery::Check(recipientAccount, sToAddress, RestrictedDelivery::Outbound) == RestrictedDelivery::Refused)
      {
         LOG_DEBUG(_T("No out-of-office message was sent: restricted delivery - ") + recipientAccount->GetAddress() +
                   _T(" may send to named domains only, and ") + sToAddress + _T(" is not in one."));
         return;
      }

      // Load the original message's header once. It answers three questions below:
      // whether an automatic reply may answer this message at all, what the original
      // subject was for the %SUBJECT% macro, and what to put after "Re: " when the
      // reply has no subject of its own.
      const String originalFileName = MessageStore::WorkingFile(recipientAccount, pOriginalMessage);

      MimeHeader header;
      AnsiString sHeader = PersistentMessage::LoadHeader(originalFileName);
      header.Load(sHeader, sHeader.GetLength(), true);

      // The RFC 3834 gate, before the once-per-sender slot below is spent: a
      // suppressed message must not burn the one reply this sender will ever get,
      // or a colleague whose first message arrives via a mailing list is never
      // answered directly either.
      String suppressReason;
      if (ShouldSuppressAutoReply_(header, sToAddress, suppressReason))
      {
         LOG_DEBUG(_T("No out-of-office message was sent: ") + suppressReason);
         return;
      }

      // The generated-mail loop count the reply carries is the original's + 1, read by
      // the header's one reader, and there is no reply once it has reached the rule
      // loop limit - as a Sieve vacation reply and a rule's reply are held. This
      // called IncreaseRuleLoopCount() on the new message, which has no count to
      // increase, so every out-of-office reply carried 1, whatever the message it
      // answered had been through: the count restarted at every reply
      // (RuleGuard::LoopCountForGeneratedMessage says why a generator must not). Asked
      // before the once-per-sender slot below is spent, as the gate above is.
      int originalLoopCount = 0;
      const char *originalLoopCountValue = header.GetRawFieldValue(String(_T("X-hMailServer-LoopCount")));
      if (originalLoopCountValue != nullptr)
         originalLoopCount = RuleGuard::ReadLoopCount(String(originalLoopCountValue));

      if (originalLoopCount >= Configuration::Instance()->GetSMTPConfiguration()->GetRuleLoopLimit())
      {
         LOG_APPLICATION(Formatter::Format("No out-of-office message was sent to {0}: the message to {1} has already passed through {2} generated messages (the rule loop limit).",
            sToAddress, recipientAccount->GetAddress(), originalLoopCount));
         return;
      }

      if (!CanSendVacationMessage_(recipientAccount, sToAddress, repeatAfterDays))
         return; // We have already notified this user.

      LOG_DEBUG("Creating out-of-office message.");

      String sModifiedSubject = sVacationSubject;
      String sModifiedBody = sVacationMessage;

      String sOldSubject;
      if (header.GetField("Subject"))
         sOldSubject = header.GetField("Subject")->GetValue();

      if (sModifiedSubject.Find(_T("%")) >= 0 || sModifiedBody.Find(_T("%")) >= 0)
      {
         // Replace macros in the string.
         sModifiedSubject.ReplaceNoCase(_T("%SUBJECT%"), sOldSubject);
         sModifiedBody.ReplaceNoCase(_T("%SUBJECT%"), sOldSubject);
      }

      if (sModifiedSubject.GetLength() == 0)
      {
         // No subject was configured, so Re: the original message's subject.
         sModifiedSubject = "Re: " + sOldSubject;
      }


      // Reply to the email
      std::shared_ptr<Message> pMsg = std::shared_ptr<Message>(new Message());

      pMsg->SetState(Message::Delivering);

      // RFC 3834 4: an automatic response is sent with a null envelope return path,
      // so that a bounce of the response cannot start a second exchange. A fresh
      // Message's from address is already empty; setting it explicitly is what makes
      // that a decision rather than an accident - TraceHeaderWriter turns it into
      // "Return-Path: <>", which is what the far end's own auto-responder looks at
      // before deciding whether to answer this reply.
      pMsg->SetFromAddress(_T(""));

      const String newFileName = MessageStore::WorkingFile(pMsg);

      std::shared_ptr<MessageData> pNewMsgData = std::shared_ptr<MessageData>(new MessageData());
      pNewMsgData->LoadFromMessage(newFileName, pMsg);

      // Required headers
      pNewMsgData->GenerateMessageID();
      pNewMsgData->SetSentTime(Time::GetCurrentMimeDate());
      pNewMsgData->SetFieldValue("Content-Type", "text/plain; charset=\"utf-8\"");

      // Optional headers
      pNewMsgData->SetFrom(recipientAccount->GetAddress());
      pNewMsgData->SetTo(sToAddress);
      pNewMsgData->SetSubject(sModifiedSubject);
      pNewMsgData->SetBody(sModifiedBody);
     pNewMsgData->SetAutoReplied();
      pNewMsgData->SetRuleLoopCount(originalLoopCount + 1);

      // Write message data. First write of a message built from nothing - see the
      // equivalent in SMTPDeliverer. No out-of-office reply is better than one queued
      // with no file behind it.
      if (!pNewMsgData->WriteReported(newFileName, "An out-of-office reply"))
         return;

      // Add recipients.
      bool recipientOK = false;
      RecipientParser recipientParser;
      recipientParser.CreateMessageRecipientList(sToAddress, pMsg->GetRecipients(), recipientOK);

      // Save message. Checked for the same reason the WriteReported above it is: the
      // pair that leaves nothing behind is a file with no row and a row with no file,
      // and this end was unguarded while the other was.
      if (pMsg->GetRecipients()->GetCount() > 0)
      {
         if (!PersistentMessage::SaveObject(pMsg))
         {
            FileUtilities::DeleteFile(newFileName);

            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6089, "SMTPVacationMessageCreator::CreateVacationMessage",
               "The out-of-office reply could not be saved and has not been sent. Its file has been removed rather than left on disk with nothing referring to it.");
         }
      }

      // Tell app to submit mail
      Application::Instance()->SubmitPendingEmail();
   }

   bool
   SMTPVacationMessageCreator::ShouldSuppressAutoReply_(const MimeHeader &originalHeader,
                                                        const String &senderAddress,
                                                        String &reason)
   {
      // RFC 3834 2: an automatic response must not be sent to a message that is
      // itself automatic. This is one half of the contract whose other half is the
      // Auto-Submitted: auto-replied stamped onto every reply this class sends -
      // without this check, two servers each running the other half loop until one
      // of them gives up.
      MimeField *autoSubmitted = originalHeader.GetField("Auto-Submitted");
      if (autoSubmitted)
      {
         String value = autoSubmitted->GetValue();
         int semicolon = value.Find(_T(";"));
         if (semicolon >= 0)
            value = value.Mid(0, semicolon);
         value.Trim();

         if (!value.IsEmpty() && value.CompareNoCase(_T("no")) != 0)
         {
            reason = _T("the message carries an Auto-Submitted header.");
            return true;
         }
      }

      // The pre-RFC-3834 convention for the same statement.
      MimeField *precedence = originalHeader.GetField("Precedence");
      if (precedence)
      {
         String value = precedence->GetValue();
         value.Trim();

         if (value.CompareNoCase(_T("bulk")) == 0 ||
             value.CompareNoCase(_T("list")) == 0 ||
             value.CompareNoCase(_T("junk")) == 0)
         {
            reason = _T("the message has a bulk, list or junk Precedence.");
            return true;
         }
      }

      // No reply to mailing-list traffic: a holiday notice sent back to a list
      // reaches every subscriber or the list robot, and either is how auto-replies
      // get a server blacklisted. Any of the RFC 2369 / RFC 2919 headers is enough
      // to identify it.
      static const char *listHeaders[] =
      {
         "List-Id", "List-Help", "List-Subscribe", "List-Unsubscribe",
         "List-Post", "List-Owner", "List-Archive"
      };

      for (const char *listHeader : listHeaders)
      {
         if (originalHeader.FieldExists(listHeader))
         {
            reason = _T("the message came from a mailing list.");
            return true;
         }
      }

      // The Microsoft convention for "do not auto-reply to this", which Exchange
      // and Outlook emit and honour.
      MimeField *suppress = originalHeader.GetField("X-Auto-Response-Suppress");
      if (suppress)
      {
         String value = suppress->GetValue();
         value.ToLower();

         if (value.Find(_T("all")) >= 0 || value.Find(_T("oof")) >= 0 || value.Find(_T("autoreply")) >= 0)
         {
            reason = _T("the message asks that auto-replies be suppressed.");
            return true;
         }
      }

      // The two robot mailboxes RFC 3834 2 names itself. Real bounces arrive with a
      // null return path and are already refused above this, but a postmaster
      // writing with a real return path is still not a correspondent to send a
      // holiday notice to. The fuller set of robot and list-management local parts
      // lives in SieveEvaluator::IsAutomatedSenderAddress_; the two lists should be
      // consolidated into one shared home rather than grown separately.
      int at = senderAddress.Find(_T("@"));
      String localPart = at >= 0 ? senderAddress.Mid(0, at) : senderAddress;
      if (localPart.CompareNoCase(_T("mailer-daemon")) == 0 ||
          localPart.CompareNoCase(_T("postmaster")) == 0)
      {
         reason = _T("the sender is a robot mailbox.");
         return true;
      }

      return false;
   }

   bool
   SMTPVacationMessageCreator::IsInternalSender(const String &senderAddress)
   {
      if (senderAddress.IsEmpty())
         return false;

      if (!StringParser::IsValidEmailAddress(senderAddress))
         return false;

      std::shared_ptr<const Account> account = CacheContainer::Instance()->GetAccount(senderAddress);
      if (account)
         return true;

      std::shared_ptr<const Alias> alias = CacheContainer::Instance()->GetAlias(senderAddress);
      if (alias && alias->GetIsActive())
         return true;

      return false;
   }

   bool
   SMTPVacationMessageCreator::CanSendVacationMessage_(std::shared_ptr<const Account> recipientAccount, const String &sTo, int repeatAfterDays)
   {
      if (!recipientAccount || sTo.IsEmpty())
         return false;

      try
      {
         // The sender as the column holds it: lower-cased, so that a sender who
         // writes their own address in two spellings is one sender, and at most
         // 255 characters, because a parameter longer than the column is a
         // comparison SQL Server Compact refuses rather than a row it does not
         // find.
         String sender = sTo;
         sender.ToLower();
         if (sender.GetLength() > 255)
            sender = sender.Mid(0, 255);

         const __int64 now = (__int64) time(nullptr);

         SQLCommand find("select arid, arsent from hm_autoreplies where araccountid = @ACCOUNTID and arsender = @SENDER");
         find.AddParameter("@ACCOUNTID", recipientAccount->GetID());
         find.AddParameter("@SENDER", sender);

         std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(find);
         if (!recordset)
            return false;

         if (!recordset->IsEOF())
         {
            const __int64 sent = recordset->GetInt64Value("arsent");
            if (repeatAfterDays <= 0 || sent > now - (__int64) repeatAfterDays * 86400)
               return false; // Answered, and the window - if there is one - has not passed.

            SQLCommand stamp("update hm_autoreplies set arsent = @SENT where arid = @ARID");
            stamp.AddParameter("@SENT", now);
            stamp.AddParameter("@ARID", recordset->GetInt64Value("arid"));
            Application::Instance()->GetDBManager()->Execute(stamp);
            return true;
         }

         SQLStatement statement;
         statement.SetTable("hm_autoreplies");
         statement.AddColumnInt64("araccountid", recipientAccount->GetID());
         statement.AddColumn("arsender", sender);
         statement.AddColumnInt64("arsent", now);
         statement.SetStatementType(SQLStatement::STInsert);
         statement.SetIdentityColumn("arid");

         // The unique index is ignored on purpose, as hm_knownsenders' is: two
         // messages from the same sender delivered at the same moment both find
         // no row and both insert, and the loser's error would be noise about a
         // reply that was sent exactly once more than it should have been.
         Application::Instance()->GetDBManager()->Execute(statement, 0, DALConnection::DALErrorInSQL);
         return true;
      }
      catch (const boost::thread_interrupted&)
      {
         // A stop's interruption, not a fault: see WorkQueue::Stop. No reply is
         // sent, as on any error here.
         boost::this_thread::disable_interruption disabled;
         LOG_DEBUG("SMTPVacationMessageCreator::CanSendVacationMessage_: a stop of the servers interrupted the lookup of who has been answered; no reply is sent.");
         throw;
      }
      catch (...)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::High, 5022, "SMTPVacationMessageCreator::CanSendVacationMessage", "Error when doing lookup of who has been answered.");
      }

      return false;
   }

   void
   SMTPVacationMessageCreator::ForgetAccount_(__int64 accountID)
   {
      SQLCommand command("delete from hm_autoreplies where araccountid = @ACCOUNTID");
      command.AddParameter("@ACCOUNTID", accountID);
      Application::Instance()->GetDBManager()->Execute(command);
   }

   void
   SMTPVacationMessageCreator::VacationMessageTurnedOff(const String &sUserAddress)
   {
      try
      {
         std::shared_ptr<const Account> account = CacheContainer::Instance()->GetAccount(sUserAddress);
         if (account)
            ForgetAccount_(account->GetID());
      }
      catch (const boost::thread_interrupted&)
      {
         // A stop's interruption, not a fault: see WorkQueue::Stop.
         boost::this_thread::disable_interruption disabled;
         LOG_DEBUG("SMTPVacationMessageCreator::VacationMessageTurnedOff: a stop of the servers interrupted the removal of the answered senders of " + sUserAddress + ".");
         throw;
      }
      catch (...)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::High, 5023, "SMTPVacationMessageCreator::VacationMessageTurnedOff", "Error when removing the answered senders of " + sUserAddress);
      }
   }

   void
   SMTPVacationMessageCreator::DomainVacationMessageTurnedOff(const String &sDomainName)
   {
      try
      {
         std::shared_ptr<const Domain> domain = CacheContainer::Instance()->GetDomain(sDomainName);
         if (!domain)
            return;

         // Every account of the domain, as the domain reply answered on behalf of
         // each. This over-clears on purpose, as before: the table does not record
         // WHICH reply answered a sender, so entries made by an account's own
         // reply are dropped too; the cost is one repeated courtesy reply, the
         // alternative a reply that never comes.
         std::shared_ptr<Accounts> accounts = std::shared_ptr<Accounts>(new Accounts(domain->GetID()));
         accounts->Refresh();
         // owned by this call: accounts is a local collection loaded above
         for (std::shared_ptr<Account> account : accounts->GetVector())
         {
            if (account)
               ForgetAccount_(account->GetID());
         }
      }
      catch (const boost::thread_interrupted&)
      {
         // A stop's interruption, not a fault: see WorkQueue::Stop.
         boost::this_thread::disable_interruption disabled;
         LOG_DEBUG("SMTPVacationMessageCreator::DomainVacationMessageTurnedOff: a stop of the servers interrupted the removal of the answered senders of " + sDomainName + ".");
         throw;
      }
      catch (...)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::High, 6320, "SMTPVacationMessageCreator::DomainVacationMessageTurnedOff", "Error when removing the answered senders of " + sDomainName);
      }
   }

   namespace
   {
      [[noreturn]] void StopInterruptionFailed(const std::string &why)
      {
         throw std::logic_error("VacationStopInterruptionTester: " + why);
      }

      // What VacationMessageTurnedOff did with a stop's interruption, on a thread
      // interrupted as WorkQueue::Stop interrupts a worker.
      enum class StopInterruptionOutcome
      {
         // Passed it on, as it is to.
         PassedOn,
         // Returned as from any lookup, with the request gone: the interruption
         // was met and taken in a catch (...) for a fault, as HM5023 took it until
         // 19 October 2026.
         Swallowed,
         // Returned with the request still pending: no interruption point was
         // met, because the lookup did not wait - it was handed a connection
         // another thread gave back, or the pool had none to wait for.
         NotMet,
         // Ended in another exception.
         OtherException,
         // Did not end within a minute.
         DidNotEnd
      };

      // One round: every free connection of the server's pool held, as statements
      // in flight hold them, so that the lookup has to wait for one
      // (DatabaseConnectionManager::GetConnection_) - the interruption point a
      // stop's interruption is met at - and a thread of the test's own, interrupted
      // before its lookup begins, looks up an address nothing has looked up, so
      // that the account cache sends the lookup to the database. The pool is handed
      // back on every path.
      StopInterruptionOutcome InterruptALookup(DatabaseConnectionManager &manager, int round)
      {
         std::vector<std::shared_ptr<DALConnection> > held;
         String error;

         auto handBack = [&manager, &held, &error]()
         {
            for (const std::shared_ptr<DALConnection> &connection : held)
               manager.RollbackTransaction(connection, error);
            held.clear();
         };

         std::atomic<StopInterruptionOutcome> outcome(StopInterruptionOutcome::DidNotEnd);
         bool joined = false;

         try
         {
            while (manager.GetAvailableConnectionCount() > 0)
            {
               std::shared_ptr<DALConnection> connection = manager.BeginTransaction(error);
               if (!connection)
                  StopInterruptionFailed("a free connection of the server's database pool could not be held: " + std::string(AnsiString(error)));

               held.push_back(connection);
            }

            const String address = Formatter::Format("stop-interruption-{0}-{1}@stop-interruption.invalid", (__int64) time(nullptr), round);

            boost::thread worker([&outcome, address]()
            {
               // The lookup begins once the thread has been interrupted, as the
               // work on a worker that a stop interrupted goes on: the request
               // then waits for the lookup's first interruption point, and a
               // lookup that returns with it still pending met none. Neither the
               // question nor the yield is an interruption point.
               while (!boost::this_thread::interruption_requested())
                  std::this_thread::yield();

               try
               {
                  SMTPVacationMessageCreator::Instance()->VacationMessageTurnedOff(address);

                  outcome = boost::this_thread::interruption_requested() ? StopInterruptionOutcome::NotMet : StopInterruptionOutcome::Swallowed;
               }
               catch (const boost::thread_interrupted &)
               {
                  outcome = StopInterruptionOutcome::PassedOn;
               }
               catch (...)
               {
                  outcome = StopInterruptionOutcome::OtherException;
               }
            });

            // As WorkQueue::Stop interrupts a worker.
            worker.interrupt();

            joined = worker.timed_join(boost::posix_time::seconds(60));

            handBack();

            if (!joined)
               worker.join();
         }
         catch (...)
         {
            handBack();
            throw;
         }

         return joined ? outcome.load() : StopInterruptionOutcome::DidNotEnd;
      }
   }

   void
   VacationStopInterruptionTester::Test()
   {
#ifndef HM_PLATFORM_POSIX
      // The COM half, which needs no database: a method's catch (...) that ends
      // in GenerateGenericMessage marks the thread for a stop's interruption, the
      // mark is taken once, and any other exception leaves no mark.
      COMError::TakeStopInterruption();

      try
      {
         throw boost::thread_interrupted();
      }
      catch (...)
      {
         COMError::GenerateGenericMessage();
      }

      if (!COMError::StopInterruptionPending())
         StopInterruptionFailed("a COM method's catch (...) that took a stop's interruption did not mark the thread for the script's host.");
      if (!COMError::TakeStopInterruption() || COMError::TakeStopInterruption())
         StopInterruptionFailed("the mark of a stop's interruption was not taken exactly once.");

      try
      {
         throw std::runtime_error("not a stop");
      }
      catch (...)
      {
         COMError::GenerateGenericMessage();
      }

      if (COMError::TakeStopInterruption())
         StopInterruptionFailed("a COM method's catch (...) that took an exception other than a stop's interruption marked the thread.");

      // Not this test's to leave on the thread.
      SetErrorInfo(0, NULL);
#endif

      std::shared_ptr<DatabaseConnectionManager> manager = Application::Instance()->GetDBManager();
      if (!manager || !manager->GetIsConnected())
         StopInterruptionFailed("the server has no database connection to hold.");

      // The servers run while ClassTester does, so a connection another thread
      // has out when a round holds the pool's free ones is not held, and given
      // back before the lookup asks, it is the lookup's at once: no wait, and no
      // interruption met. Such a round shows nothing - the interruption's request
      // is still pending when the lookup returns, where one met and taken is
      // gone - and is made again, for thirty seconds at most.
      const std::chrono::steady_clock::time_point giveUp = std::chrono::steady_clock::now() + std::chrono::seconds(30);

      for (int round = 1; ; round++)
      {
         switch (InterruptALookup(*manager, round))
         {
         case StopInterruptionOutcome::PassedOn:
            return;
         case StopInterruptionOutcome::Swallowed:
            StopInterruptionFailed("VacationMessageTurnedOff met a stop's interruption at the wait for a pooled connection and took it for a fault (HM5023), where it is to pass it on.");
         case StopInterruptionOutcome::OtherException:
            StopInterruptionFailed("VacationMessageTurnedOff, interrupted at the wait for a pooled connection, ended in an exception other than the interruption.");
         case StopInterruptionOutcome::DidNotEnd:
            StopInterruptionFailed("the interrupted thread did not end within a minute while the free connections of the server's database pool were held.");
         case StopInterruptionOutcome::NotMet:
            break;
         }

         if (std::chrono::steady_clock::now() >= giveUp)
            StopInterruptionFailed(Formatter::FormatAsAnsi("in {0} rounds over thirty seconds the lookup never waited for a pooled connection, and so never met the interruption: each time it was given a connection that another thread had out and gave back before it asked, or the pool had none to wait for.", round));

         Sleep(50);
      }
   }
}
