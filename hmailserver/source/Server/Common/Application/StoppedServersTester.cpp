// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"

#include "StoppedServersTester.h"

#include <atomic>
#include <stdexcept>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

#include "../AntiSpam/SpamTestData.h"
#include "../AntiSpam/SpamTestResult.h"
#include "../AntiSpam/SpamTestSpamAssassin.h"
#include "../AntiSpam/SpamAssassin/SpamAssassinLearner.h"
#include "../AntiSpam/SpamAssassin/SpamAssassinTestConnect.h"
#include "../BO/FetchAccount.h"
#include "../BO/IMAPFolder.h"
#include "../BO/Message.h"
#include "../BO/MessageData.h"
#include "../BO/MessageRecipient.h"
#include "../BO/Messages.h"
#include "../Diagnostics/TestConnect.h"
#include "../Tracking/ChangeNotification.h"
#include "../Tracking/NotificationServer.h"
#include "../Util/FileUtilities.h"
#include "../Util/ServerInfo.h"
#include "../Util/ServerStatus.h"
#include "../../ExternalFetcher/ExternalFetch.h"
#include "../../IMAP/IMAPConnection.h"
#include "../../IMAP/IMAPNotificationClient.h"
#include "../../IMAP/MessagesContainer.h"
#include "../../SMTP/DeliveryQueue.h"
#include "../../SMTP/ExternalDelivery.h"
#include "../../SMTP/RuleResult.h"
#include "../../SMTP/SMTPClientConnection.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // Ids nothing has - the database's count up from 1 - and none of
      // NotificationServerLifetimeTester's, so that nothing else subscribes to
      // the folder or tells of a change to it. Within a 32-bit int: in a
      // running server a session's end removes the folder's \Recent in the
      // database, whose account and folder columns are int, and SQL Server
      // Compact refuses a statement whose parameter does not fit one.
      const __int64 NoSuchAccountID = 2147483611LL;
      const __int64 StoppedFolderID = 2147483612LL;
      const __int64 NoDatabaseFolderID = 2147483613LL;
      const __int64 NoSuchMessageID = 2147483614LL;

      std::atomic<const char *> current_case{""};

      // The case begun last, which nothing clears: a fault taken by a catch
      // (...) under /EHa has unwound the case's scope, and with it the name.
      std::atomic<const char *> last_case{""};

      // Names the case for a fault handler while it runs (GetCurrentCase).
      class CaseScope
      {
      public:
         explicit CaseScope(const char *name)
         {
            current_case = name;
            last_case = name;
         }

         ~CaseScope()
         {
            current_case = "";
         }

         CaseScope(const CaseScope &) = delete;
         CaseScope &operator=(const CaseScope &) = delete;
      };

      void Append(String &said, const String &what)
      {
         if (what.IsEmpty())
            return;

         if (!said.IsEmpty())
            said += _T(" And ");

         said += what;
      }

      String Said(const String &where, const std::exception &thrown)
      {
         return Formatter::Format(_T("{0} threw: {1}."), where, String(thrown.what()));
      }
   }

   const char *
   StoppedServersTester::GetCurrentCase()
   {
      return current_case.load();
   }

   void
   StoppedServersTester::Test()
   {
      String said = TestASessionOnAStoppedIOService_();

      Application &application = *Application::Instance();

      if (ServerStatus::Instance()->GetState() == ServerStatus::StateRunning)
      {
         // In a running server: stopped for the cases, and started again
         // whatever they did. Under /EHa a fault in one of them is taken here,
         // named, and the servers still start.
         const String refused = application.StopServers();
         if (!refused.IsEmpty())
         {
            Append(said, String(_T("the stop of the servers was refused: ")) + refused);
         }
         else
         {
            try
            {
               Append(said, TestWithoutTheIOService_());
            }
            catch (const std::exception &thrown)
            {
               Append(said, Said(Formatter::Format(_T("with the servers stopped, {0}"), String(last_case.load())), thrown));
            }
            catch (...)
            {
               Append(said, Formatter::Format(_T("with the servers stopped, {0} faulted: it reached for the TCP/IP service the stop had let go of."), String(last_case.load())));
            }

            current_case = "";
         }

         String startRefused;
         if (!application.StartServers(0, &startRefused))
            Append(said, String(_T("the servers did not start again: ")) + startRefused);
      }
      else
      {
         try
         {
            Append(said, TestWithoutTheIOService_());

            // Only here: in a running server the clear reports an error, which the
            // regression suite's fixtures take for a failure, so the suite's
            // MainOperations clears the queue through COM with the servers
            // stopped instead and reads the error it reports.
            Append(said, TestWithoutTheDeliveryManager_());
         }
         catch (const std::exception &thrown)
         {
            Append(said, Said(Formatter::Format(_T("with the servers not running, {0}"), String(last_case.load())), thrown));
         }

         current_case = "";
      }

      // Only where there is no database manager: stopped-servers-tests. In a
      // running server there is one, and the others' statements need it.
      if (!application.GetDBManager())
         Append(said, TestWithoutTheDatabase_());

      if (!said.IsEmpty())
         throw std::logic_error(AnsiString(String(_T("StoppedServersTester: ")) + said).c_str());
   }

   String
   StoppedServersTester::TestASessionOnAStoppedIOService_()
   {
      String said;

      std::shared_ptr<NotificationServer> server = Application::Instance()->GetNotificationServer();

      bool subscribed = false;
      size_t kept = 0;
      size_t claimed = 0;
      bool stillRecent = false;

      try
      {
         CaseScope scope("a session on an I/O service that had stopped told of a change to its folder (IMAPNotificationClient::OnNotification)");

         // The collection of the folder's messages that every session shares,
         // holding one message that is \Recent, as one just delivered is.
         std::shared_ptr<Messages> shared = MessagesContainer::Instance()->GetMessages(NoSuchAccountID, StoppedFolderID);
         std::shared_ptr<Message> arrived = std::make_shared<Message>(false);
         arrived->SetID(NoSuchMessageID);
         arrived->SetFlagRecent(true);
         shared->AddItem(arrived);

         // Declared first: the session's stream refers to it until the end.
         boost::asio::ssl::context ssl(boost::asio::ssl::context::sslv23);
         {
            // Stopped as IOService::Stop stops the servers' own: the state of a
            // session a stop could not end, which its queue keeps.
            boost::asio::io_context io;
            io.stop();

            std::shared_ptr<IMAPConnection> session = std::make_shared<IMAPConnection>(CSNone, io, ssl);
            session->Initialize();

            std::shared_ptr<IMAPFolder> folder = std::make_shared<IMAPFolder>(NoSuchAccountID, -1);
            folder->SetID(StoppedFolderID);
            session->SetCurrentFolder(folder, false, shared);
            subscribed = server->HasMessageSubscribers(NoSuchAccountID);

            // Not in IDLE: a change kept for the session's next command, which
            // will never come.
            server->SendNotification(std::make_shared<ChangeNotification>(NoSuchAccountID, StoppedFolderID, ChangeNotification::NotificationMessageAdded));
            {
               std::shared_ptr<IMAPNotificationClient> client = session->GetNotificationClient();
               boost::lock_guard<boost::recursive_mutex> guard(client->mutex_);
               kept = client->cached_changes_.size();
            }

            // In IDLE: the new message's \Recent taken for the session.
            session->SetIsIdling(true);
            server->SendNotification(std::make_shared<ChangeNotification>(NoSuchAccountID, StoppedFolderID, ChangeNotification::NotificationMessageAdded));
            claimed = session->GetRecentMessageCount();
            stillRecent = arrived->GetFlagRecent();
            session->SetIsIdling(false);

            // The session ends; whatever it had posted to the stopped service
            // goes with the service, below.
            session.reset();
         }
      }
      catch (const std::exception &thrown)
      {
         Append(said, Said(_T("a session on an I/O service that had stopped, told of a change to its folder,"), thrown));
         return said;
      }

      const bool givenBack = !server->HasMessageSubscribers(NoSuchAccountID);

      if (!subscribed)
         Append(said, _T("a session that had selected a folder was not subscribed to it, so the rest of this case tested nothing."));

      if (kept != 0)
      {
         Append(said, Formatter::Format(_T("a session on an I/O service that had stopped, not in IDLE, kept {0} change(s) to its folder that it would never send (IMAPNotificationClient::OnNotification): a session a stop could not end is to be told nothing."),
            (int) kept));
      }

      if (claimed != 0 || !stillRecent)
      {
         Append(said, Formatter::Format(_T("a session in IDLE on an I/O service that had stopped took the \\Recent of {0} new message(s) for itself (IMAPNotificationClient::ClaimRecentMessages_), though it will never answer its client: the sessions that will are then never told the messages are \\Recent."),
            (int) claimed));
      }

      if (!givenBack)
         Append(said, _T("a session on an I/O service that had stopped was still subscribed to its folder after it had ended."));

      return said;
   }

   String
   StoppedServersTester::TestWithoutTheIOService_()
   {
      if (Application::Instance()->GetIOService())
         return _T("the servers' TCP/IP service was there with the servers stopped, so nothing that needs it gone was tested.");

      String said;

      const String messageFile = FileUtilities::GetTempFileName();
      FileUtilities::WriteToFile(messageFile, AnsiString("From: a@example.test\r\nTo: b@example.test\r\nSubject: StoppedServersTester\r\n\r\nA message.\r\n"));

      {
         // A message's finalisation that the stop gave up on, still in a DNS test,
         // goes on to this one when the lookup ends.
         CaseScope scope("a message's SpamAssassin test (SpamTestSpamAssassin::RunTest)");

         std::shared_ptr<Message> message = std::make_shared<Message>(false);
         std::shared_ptr<MessageData> data = std::make_shared<MessageData>();
         data->LoadFromMessage(messageFile, message);

         std::shared_ptr<SpamTestData> testData = std::make_shared<SpamTestData>();
         testData->SetMessageData(data);
         testData->SetEnvelopeFrom(_T("a@example.test"));

         SpamTestSpamAssassin test;
         if (!test.RunTest(testData).empty())
            Append(said, _T("with the servers stopped a message's SpamAssassin test gave a verdict, where it can make no connection to spamd."));
      }

      {
         CaseScope scope("a lesson for SpamAssassin (SpamAssassinLearner::Tell)");

         if (SpamAssassinLearner::Tell(messageFile, true, String()))
            Append(said, _T("with the servers stopped a lesson for SpamAssassin was said to have been delivered, where no connection to spamd can be made."));
      }

      {
         CaseScope scope("the test of the connection to SpamAssassin (SpamAssassinTestConnect::TestConnect)");

         String message;
         SpamAssassinTestConnect test;
         const bool connected = test.TestConnect(_T("127.0.0.1"), 783, message);
         if (connected || message.Find(_T("stopped")) < 0)
         {
            Append(said, Formatter::Format(_T("with the servers stopped the test of the connection to SpamAssassin answered {0} and \"{1}\", not that the servers are stopped."),
               String(connected ? _T("true") : _T("false")), message));
         }
      }

      {
         CaseScope scope("the diagnostics' connection test (TestConnect::PerformTest)");

         String result;
         TestConnect test;
         const bool connected = test.PerformTest(CSNone, String(), _T("127.0.0.1"), 25, result);
         if (connected || result.Find(_T("stopped")) < 0)
         {
            Append(said, Formatter::Format(_T("with the servers stopped the diagnostics' connection test answered {0} and \"{1}\", not that the servers are stopped."),
               String(connected ? _T("true") : _T("false")), result));
         }
      }

      {
         CaseScope scope("a fetch from an external account (ExternalFetch::Start)");

         std::shared_ptr<FetchAccount> account = std::make_shared<FetchAccount>();
         account->SetName(_T("StoppedServersTester"));
         account->SetServerAddress(_T("127.0.0.1"));

         ExternalFetch fetch;
         fetch.Start(account);
      }

      {
         CaseScope scope("a delivery to one server (ExternalDelivery::DeliverToSingleServer_)");

         std::shared_ptr<Message> queued = std::make_shared<Message>(false);
         RuleResult rules;
         ExternalDelivery delivery(_T("127.0.0.1"), queued, rules);

         std::shared_ptr<MessageRecipient> recipient = std::make_shared<MessageRecipient>();
         recipient->SetAddress(_T("someone@example.test"));
         std::vector<std::shared_ptr<MessageRecipient> > recipients;
         recipients.push_back(recipient);

         std::shared_ptr<ServerInfo> serverInfo = std::make_shared<ServerInfo>(false, _T("mx.example.test"), _T("127.0.0.1"), 25, String(), String(), CSNone);

         const SmtpTlsResult tls = delivery.DeliverToSingleServer_(recipients, serverInfo, queued);
         if (recipient->GetDeliveryResult() != MessageRecipient::ResultNonFatalError || tls != SmtpTlsResult::NotReached)
         {
            Append(said, Formatter::Format(_T("with the servers stopped a delivery to one server left its recipient with the result {0}, and TLS {1}, where the recipient is deferred ({2}) and nothing was reached."),
               (int) recipient->GetDeliveryResult(), (int) tls, (int) MessageRecipient::ResultNonFatalError));
         }

         // Deferred by this server, not tried: the pass is rescheduled without
         // counting a try (RescheduleDelivery_'s uncounted path, which
         // EveryFailureIsShaping_ chooses), however long the message has been
         // queued, and the message trace is given no deferral of an attempt
         // (CollectDeliveryResult_ writes none for a recipient in
         // shaping_deferred_). Until this was so the pass was counted: a try
         // spent an hour away at the defaults, and on the last try the message
         // returned to its sender, though nobody had been offered it.
         std::map<String, DeliveryFailure> failed;
         failed.insert(std::make_pair(recipient->GetAddress(), DeliveryFailure(recipient->GetAddress(), recipient->GetEnhancedStatusCode(), recipient->GetErrorMessage())));

         if (!delivery.EveryFailureIsShaping_(failed) || delivery.shaping_deferred_.find(recipient->GetAddress()) == delivery.shaping_deferred_.end())
         {
            Append(said, _T("with the servers stopped a delivery to one server deferred its recipient as a failed try, so that the pass was counted against the message's retries, and a message on its last try was returned to its sender, where nobody was offered it."));
         }
         else if (delivery.CountsOwnDeferral_(300L, 24L * 3600L))
         {
            Append(said, _T("with the servers stopped a delivery to one server was counted against the retries of a message queued for longer than its retry schedule spans, where nobody was offered it."));
         }
         else if (recipient->GetEnhancedStatusCode() != _T("4.3.0"))
         {
            Append(said, Formatter::Format(_T("with the servers stopped a delivery to one server deferred its recipient with the status {0}, where it is 4.3.0."), recipient->GetEnhancedStatusCode()));
         }
      }

      {
         CaseScope scope("a delivery to one domain (ExternalDelivery::DeliverToSingleDomain_)");

         // A batch after the stop, or a delivery that began after it: no
         // lookup is made (none could lead to a connection), the batch is
         // deferred as the server's is, and the pass is not counted.
         std::shared_ptr<Message> queued = std::make_shared<Message>(false);
         RuleResult rules;
         ExternalDelivery delivery(_T("127.0.0.1"), queued, rules);

         std::shared_ptr<MessageRecipient> recipient = std::make_shared<MessageRecipient>();
         recipient->SetAddress(_T("someone@example.test"));
         std::vector<std::shared_ptr<MessageRecipient> > recipients;
         recipients.push_back(recipient);

         std::shared_ptr<ServerInfo> serverInfo = std::make_shared<ServerInfo>(false, _T("example.test"), String(), 25, String(), String(), CSNone);

         delivery.DeliverToSingleDomain_(recipients, serverInfo, std::shared_ptr<RemoteDomainPolicy>());

         if (recipient->GetDeliveryResult() != MessageRecipient::ResultNonFatalError || !delivery.servers_stopped_ ||
             delivery.shaping_deferred_.find(recipient->GetAddress()) == delivery.shaping_deferred_.end())
         {
            Append(said, Formatter::Format(_T("with the servers stopped a delivery to one domain left its recipient with the result {0}, not deferred without counting a try ({1})."),
               (int) recipient->GetDeliveryResult(), (int) MessageRecipient::ResultNonFatalError));
         }
      }

      FileUtilities::DeleteFile(messageFile);

      return said;
   }

   String
   StoppedServersTester::TestWithoutTheDeliveryManager_()
   {
      if (Application::Instance()->GetSMTPDeliveryManager())
         return _T("the delivery manager was there with the servers not running, so a clear of the delivery queue without it tested nothing.");

      CaseScope scope("a clear of the delivery queue with the servers stopped (DeliveryQueueClearer::DoWork)");

      // As DeliveryQueue::Clear leaves it when it has queued the clear: until the
      // clear says it is over, every clear asked for is turned away.
      DeliveryQueue::is_clearing_ = true;

      DeliveryQueueClearer clearer;
      clearer.DoWork();

      const bool stillClearing = DeliveryQueue::is_clearing_;
      DeliveryQueue::is_clearing_ = false;

      if (stillClearing)
         return _T("a clear of the delivery queue with the servers stopped never said it was over (DeliveryQueue::OnDeliveryQueueCleared), so that every clear asked for after it, after the start too, would do nothing.");

      return String();
   }

   String
   StoppedServersTester::TestWithoutTheDatabase_()
   {
      String said;

      {
         CaseScope scope("a refresh of a folder's messages with no database manager (Messages::Refresh)");

         try
         {
            Messages messages(NoSuchAccountID, NoDatabaseFolderID);
            if (messages.Refresh(false))
               Append(said, _T("with no database manager a refresh of a folder's messages said it had read them."));
         }
         catch (const std::exception &thrown)
         {
            Append(said, Said(_T("with no database manager a refresh of a folder's messages (Messages::Refresh), which an IMAP session that ends during a reinitialise asks for,"), thrown));
         }
      }

      {
         CaseScope scope("the removal of a folder's \\Recent with no database manager (Messages::RemoveRecentFlags)");

         try
         {
            Messages messages(NoSuchAccountID, NoDatabaseFolderID);
            std::shared_ptr<Message> recent = std::make_shared<Message>(false);
            recent->SetID(NoSuchMessageID);
            recent->SetFlagRecent(true);
            messages.AddItem(recent);

            messages.RemoveRecentFlags();
            if (recent->GetFlagRecent())
               Append(said, _T("with no database manager the removal of a folder's \\Recent left a message \\Recent in memory."));
         }
         catch (const std::exception &thrown)
         {
            Append(said, Said(_T("with no database manager the removal of a folder's \\Recent (Messages::RemoveRecentFlags), which an IMAP session that ends during a reinitialise makes,"), thrown));
         }
      }

      {
         CaseScope scope("a claim of a folder's \\Recent with no database manager (Messages::ClaimRecentMessageIDs)");

         try
         {
            Messages messages(NoSuchAccountID, NoDatabaseFolderID);
            std::shared_ptr<Message> recent = std::make_shared<Message>(false);
            recent->SetID(NoSuchMessageID);
            recent->SetFlagRecent(true);
            messages.AddItem(recent);

            const std::set<__int64> claimed = messages.ClaimRecentMessageIDs();
            if (claimed.size() != 1 || claimed.count(NoSuchMessageID) != 1)
               Append(said, Formatter::Format(_T("with no database manager a claim of a folder's \\Recent claimed {0} message(s), not the one that was \\Recent."), (int) claimed.size()));
         }
         catch (const std::exception &thrown)
         {
            Append(said, Said(_T("with no database manager a claim of a folder's \\Recent (Messages::ClaimRecentMessageIDs)"), thrown));
         }
      }

      {
         CaseScope scope("an IMAP session closing its folder with no database manager (IMAPConnection::CloseCurrentFolder)");

         std::shared_ptr<NotificationServer> server = Application::Instance()->GetNotificationServer();
         bool subscribed = false;

         try
         {
            boost::asio::ssl::context ssl(boost::asio::ssl::context::sslv23);
            boost::asio::io_context io;

            std::shared_ptr<IMAPConnection> session = std::make_shared<IMAPConnection>(CSNone, io, ssl);
            session->Initialize();

            std::shared_ptr<IMAPFolder> folder = std::make_shared<IMAPFolder>(NoSuchAccountID, -1);
            folder->SetID(NoDatabaseFolderID);
            session->SetCurrentFolder(folder, false, std::make_shared<Messages>(NoSuchAccountID, NoDatabaseFolderID));
            subscribed = server->HasMessageSubscribers(NoSuchAccountID);

            // What the session's end does first, called here so that what it
            // throws is seen: its destructor's own catch takes anything.
            session->CloseCurrentFolder();
            session.reset();
         }
         catch (const std::exception &thrown)
         {
            Append(said, Said(_T("with no database manager an IMAP session closing its folder as it ends (IMAPConnection::CloseCurrentFolder)"), thrown));
         }

         if (!subscribed)
            Append(said, _T("with no database manager a session that had selected a folder was not subscribed to it."));
         else if (server->HasMessageSubscribers(NoSuchAccountID))
            Append(said, _T("with no database manager a session that closed its folder was still subscribed to it."));
      }

      return said;
   }
}
