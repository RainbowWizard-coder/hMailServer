// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"
#include "IMAPMoveDispatch.h"

#include "IMAPConnection.h"
#include "IMAPConfiguration.h"
#include "IMAPFolderContainer.h"

#include "../Common/AntiSpam/MoveLearning.h"
#include "../Common/AntiSpam/RspamdLearner.h"
#include "../Common/AntiSpam/SpamAssassin/SpamAssassinLearner.h"
#include "../Common/Application/Application.h"
#include "../Common/Application/Configuration.h"
#include "../Common/BO/Account.h"
#include "../Common/BO/IMAPFolder.h"
#include "../Common/BO/IMAPFolders.h"
#include "../Common/Persistence/PersistentAccount.h"
#include "../Common/Scripting/ScriptObjectContainer.h"
#include "../Common/Scripting/ScriptServer.h"
#include "../Common/Threading/Task.h"
#include "../Common/Threading/WorkQueue.h"
#include "../Common/Util/GUIDCreator.h"
#include "../Common/Util/Parsing/StringParser.h"
#include "../Common/Util/Strings/Formatter.h"

#ifdef HM_PLATFORM_POSIX
#include "../Common/Scripting/ScriptBinding.h"
#endif

#include <exception>
#include <set>
#include <stdexcept>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // Fires the events waiting in the queue, oldest first, until none is left,
      // on a thread of the asynchronous work queue: never the IMAP thread, and
      // inside no folder lock or transaction, since the command that queued them
      // has finished its work. One at a time, so a script sees them in the order
      // the moves were made; each handler under the script timeout, as every
      // event's is. A task the queue never ran (the servers stopped first), or one
      // a stop interrupted, tells the queue so as it goes, and the next MOVE that
      // queues an event starts another.
      class IMAPMoveEventTask : public Task
      {
      public:
         IMAPMoveEventTask() :
            Task("IMAPMoveEventTask"),
            emptied_(false)
         {
         }

         // DrainerEnded takes the queue's lock, which the system may refuse: an
         // exception that left this destructor, which may not throw, ended the
         // process, and so does this, said where it happens. Otherwise the queue
         // would believe a drainer still runs, and no event would be fired again.
         ~IMAPMoveEventTask()
         {
            if (emptied_)
               return;

            try
            {
               IMAPMoveDispatch::Queue().DrainerEnded();
            }
            catch (...)
            {
               std::terminate();
            }
         }

         virtual void DoWork()
         {
            IMAPMoveEvent event;
            while (IMAPMoveDispatch::Queue().Take(event))
            {
               try
               {
                  IMAPMoveDispatch::Fire(event);
               }
               catch (boost::thread_interrupted &)
               {
                  // The stop of the servers: passed on, and what is still waiting
                  // waits for the next drainer (the destructor above).
                  throw;
               }
               catch (std::exception &error)
               {
                  // One event that could not be fired - its account not read, say -
                  // is that event's alone: the move has happened, and the events
                  // after it are fired all the same.
                  LOG_APPLICATION(Formatter::Format("OnIMAPMessageMoved for message {0} of account {1} was not fired: {2}",
                     event.message_id, event.account_id, error.what()));
               }
            }

            emptied_ = true;
         }

      private:
         bool emptied_;
      };
   }

   IMAPMoveRecord::IMAPMoveRecord() :
      operation(OperationCopy),
      events_decided(false),
      events_wanted(false),
      account_id(0),
      source_folder_id(0),
      source_uid_validity(0),
      destination_folder_id(0),
      destination_uid_validity(0)
   {
   }

   void
   IMAPMoveRecord::Clear()
   {
      lessons.clear();
      moved.clear();
   }

   void
   IMAPMoveRecord::MarkSourcesRemoved(const std::vector<__int64> &deleted_message_ids)
   {
      if (moved.empty() || deleted_message_ids.empty())
         return;

      const std::set<__int64> deleted(deleted_message_ids.begin(), deleted_message_ids.end());

      for (Moved &item : moved)
      {
         if (deleted.find(item.source_message_id) != deleted.end())
            item.source_removed = true;
      }
   }

   IMAPMoveEvent::IMAPMoveEvent() :
      account_id(0),
      source_folder_id(0),
      source_uid_validity(0),
      source_uid(0),
      destination_folder_id(0),
      destination_uid_validity(0),
      destination_uid(0),
      source_message_id(0),
      message_id(0)
   {
   }

   IMAPMoveEventQueue::IMAPMoveEventQueue(size_t capacity) :
      capacity_(capacity),
      drainer_running_(false)
   {
   }

   size_t
   IMAPMoveEventQueue::Offer(const std::vector<IMAPMoveEvent> &events, bool &startDrainer)
   {
      startDrainer = false;

      boost::lock_guard<boost::mutex> guard(mutex_);

      size_t admitted = 0;
      for (const IMAPMoveEvent &event : events)
      {
         if (pending_.size() >= capacity_)
            break;

         pending_.push_back(event);
         admitted++;
      }

      if (admitted > 0 && !drainer_running_)
      {
         drainer_running_ = true;
         startDrainer = true;
      }

      return admitted;
   }

   bool
   IMAPMoveEventQueue::Take(IMAPMoveEvent &event)
   {
      boost::lock_guard<boost::mutex> guard(mutex_);

      if (pending_.empty())
      {
         drainer_running_ = false;
         return false;
      }

      event = pending_.front();
      pending_.pop_front();
      return true;
   }

   void
   IMAPMoveEventQueue::DrainerEnded()
   {
      boost::lock_guard<boost::mutex> guard(mutex_);
      drainer_running_ = false;
   }

   size_t
   IMAPMoveEventQueue::GetDepth() const
   {
      boost::lock_guard<boost::mutex> guard(mutex_);
      return pending_.size();
   }

   size_t
   IMAPMoveEventQueue::GetCapacity() const
   {
      return capacity_;
   }

   bool
   IMAPMoveEventQueue::GetDrainerRunning() const
   {
      boost::lock_guard<boost::mutex> guard(mutex_);
      return drainer_running_;
   }

   IMAPMoveDispatch::Facts::Facts() :
      events_on(false),
      scripting_on(false),
      handler_declared(false),
      session_account_id(0),
      source_account_id(0),
      destination_account_id(0),
      source_public(false),
      destination_public(false)
   {
   }

   bool
   IMAPMoveDispatch::RaisesMoveEvents(const Facts &facts)
   {
      if (!facts.events_on || !facts.scripting_on || !facts.handler_declared)
         return false;

      // The account's own folders, and only those: a public folder belongs to
      // nobody, and a folder of another account shared or delegated to this one
      // is that account's - an event naming the session's account for it would
      // name the wrong owner, and one naming the owner would tell its script of
      // what somebody else did.
      if (facts.source_public || facts.destination_public)
         return false;

      if (facts.session_account_id <= 0)
         return false;

      return facts.source_account_id == facts.session_account_id &&
             facts.destination_account_id == facts.session_account_id;
   }

   void
   IMAPMoveDispatch::PrepareMoveEvents(std::shared_ptr<IMAPConnection> connection, std::shared_ptr<IMAPFolder> source,
                                       std::shared_ptr<IMAPFolder> destination, IMAPMoveRecord &record)
   {
      if (record.events_decided)
         return;

      record.events_decided = true;
      record.events_wanted = false;

      // The one read a server that has not switched the event on makes.
      if (!Configuration::Instance()->GetIMAPMessageMovedEvents())
         return;

      if (!connection || !source || !destination)
         return;

      std::shared_ptr<const Account> account = connection->GetAccount();
      if (!account)
         return;

      Facts facts;
      facts.events_on = true;
      facts.scripting_on = Configuration::Instance()->GetUseScriptServer();
      facts.handler_declared = ScriptServer::Instance()->HasHandler(ScriptServer::EventOnIMAPMessageMoved);
      facts.session_account_id = account->GetID();
      facts.source_account_id = source->GetAccountID();
      facts.destination_account_id = destination->GetAccountID();
      facts.source_public = source->IsPublicFolder();
      facts.destination_public = destination->IsPublicFolder();

      if (!RaisesMoveEvents(facts))
         return;

      record.events_wanted = true;
      record.account_id = account->GetID();

      // Who signed in: the account itself, or the IMAP master user that opened
      // its mailbox (IMAPConnection::GetSignedInAs). The owner is the account
      // either way.
      record.user = connection->GetSignedInAs();
      if (record.user.IsEmpty())
         record.user = account->GetAddress();

      // RFC 3501 2.3.1.1: UIDVALIDITY is the folder's creation time, as SELECT,
      // STATUS and COPYUID give it.
      record.source_folder_id = source->GetID();
      record.source_folder = FolderPath_(source);
      record.source_uid_validity = (unsigned int) source->GetCreationTime().ToInt();

      record.destination_folder_id = destination->GetID();
      record.destination_folder = FolderPath_(destination);
      record.destination_uid_validity = (unsigned int) destination->GetCreationTime().ToInt();
   }

   String
   IMAPMoveDispatch::FolderPath_(std::shared_ptr<IMAPFolder> folder)
   {
      // Walked up by parent id in the owner's tree, as RENAME walks it, and
      // joined with the hierarchy delimiter: the name LIST gives the folder,
      // before it is encoded for the wire.
      std::shared_ptr<IMAPFolders> tree = IMAPFolderContainer::Instance()->GetFoldersForAccount(folder->GetAccountID());

      std::vector<String> path;
      std::shared_ptr<IMAPFolder> walk = folder;
      int recursionGuard = 0;

      while (walk && recursionGuard++ <= IMAPFolder::MaxFolderDepth)
      {
         path.insert(path.begin(), walk->GetFolderName());

         const __int64 parentId = walk->GetParentFolderID();
         walk = (parentId >= 0 && tree) ? tree->GetItemByDBIDRecursive(parentId) : nullptr;
      }

      return StringParser::JoinVector(path, Configuration::Instance()->GetIMAPConfiguration()->GetHierarchyDelimiter());
   }

   void
   IMAPMoveDispatch::Committed(IMAPMoveRecord &record)
   {
      // The lessons of learning from the Junk folder, each for the backend chosen
      // when its copy was made: SpamAssassin's queued for spamd one by one as they
      // have been since SpamAssassinLearnOnMove came, Rspamd's queued together, so
      // that a full queue is said once for the command.
      std::vector<RspamdLearner::Lesson> rspamdLessons;
      for (const IMAPMoveRecord::Lesson &lesson : record.lessons)
      {
         if (lesson.backend == MoveLearning::BackendRspamd)
         {
            RspamdLearner::Lesson rspamd;
            rspamd.copy = lesson.copy;
            rspamd.owner = lesson.destination_owner;
            rspamd.spam = lesson.spam;
            if (rspamd.copy && rspamd.owner)
               rspamdLessons.push_back(rspamd);
            continue;
         }

         SpamAssassinLearner::Teach(lesson.copy, lesson.destination_owner, lesson.spam);
      }

      if (!rspamdLessons.empty())
         RspamdLearner::Queue(rspamdLessons);

      const std::vector<IMAPMoveEvent> events = EventsOf(record);
      if (!events.empty())
         QueueEvents_(events);

      record.Clear();
   }

   std::vector<IMAPMoveEvent>
   IMAPMoveDispatch::EventsOf(const IMAPMoveRecord &record)
   {
      std::vector<IMAPMoveEvent> events;

      // A COPY moves nothing, and a COPY followed by a STORE and an EXPUNGE is
      // three commands, which nothing here joins into a move.
      if (record.operation != IMAPMoveRecord::OperationMove && record.operation != IMAPMoveRecord::OperationUidMove)
         return events;

      if (!record.events_wanted)
         return events;

      for (const IMAPMoveRecord::Moved &item : record.moved)
      {
         // A message whose original the expunge did not delete is in both folders:
         // not moved.
         if (!item.source_removed)
            continue;

         IMAPMoveEvent event;
         event.operation = OperationName(record.operation);
         event.user = record.user;
         event.account_id = record.account_id;
         event.source_folder_id = record.source_folder_id;
         event.source_folder = record.source_folder;
         event.source_uid_validity = record.source_uid_validity;
         event.source_uid = item.source_uid;
         event.destination_folder_id = record.destination_folder_id;
         event.destination_folder = record.destination_folder;
         event.destination_uid_validity = record.destination_uid_validity;
         event.destination_uid = item.destination_uid;
         event.source_message_id = item.source_message_id;
         event.message_id = item.message_id;
         events.push_back(event);
      }

      return events;
   }

   String
   IMAPMoveDispatch::OperationName(IMAPMoveRecord::Operation operation)
   {
      switch (operation)
      {
      case IMAPMoveRecord::OperationCopy:
         return _T("COPY");
      case IMAPMoveRecord::OperationUidCopy:
         return _T("UID COPY");
      case IMAPMoveRecord::OperationMove:
         return _T("MOVE");
      case IMAPMoveRecord::OperationUidMove:
         return _T("UID MOVE");
      }

      return String();
   }

   IMAPMoveEventQueue &
   IMAPMoveDispatch::Queue()
   {
      // Never freed: a drainer task the asynchronous work queue frees as the
      // process ends still tells it it has ended.
      static IMAPMoveEventQueue *queue = new IMAPMoveEventQueue(QueueCapacity);
      return *queue;
   }

   void
   IMAPMoveDispatch::QueueEvents_(const std::vector<IMAPMoveEvent> &events)
   {
      bool startDrainer = false;
      const size_t admitted = Queue().Offer(events, startDrainer);

      if (admitted < events.size())
      {
         // The client is not made to wait for a script: the newest are dropped,
         // and said so, once for the command.
         LOG_APPLICATION(Formatter::Format("OnIMAPMessageMoved: {0} of the {1} messages a {2} by {3} moved were not queued for the event script, "
            "because {4} events are waiting already, the most there may be. The handler is slower than the moves it is told of.",
            (unsigned __int64) (events.size() - admitted), (unsigned __int64) events.size(), events.front().operation,
            events.front().user, (unsigned __int64) Queue().GetCapacity()));
      }

      if (!startDrainer)
         return;

      std::shared_ptr<WorkQueue> workQueue = Application::Instance()->GetAsyncWorkQueue();
      if (!workQueue)
      {
         // The servers are stopping or stopped. What was queued waits, and the
         // next MOVE that queues an event starts the drainer.
         Queue().DrainerEnded();
         return;
      }

      // It runs a script, which may block for as long as the script timeout:
      // TaskMayBlock keeps it within the queue's cap for such tasks.
      workQueue->AddTask(std::shared_ptr<Task>(new IMAPMoveEventTask()), WorkQueue::TaskMayBlock);
   }

   String
   IMAPMoveDispatch::BuildCall(const IMAPMoveEvent &event, const String &language)
   {
      const bool vbscript = language == _T("VBScript");
      if (!vbscript && language != _T("JScript"))
         return String();

      // Every text is somebody's choice - a folder's name is the user's - so each
      // is made a literal whole for the language it is written in; the numbers are
      // the server's own.
      auto text = [vbscript](const String &value) -> String
      {
         if (vbscript)
            return _T("\"") + ScriptServer::EscapeForVBScriptLiteral(value) + _T("\"");

         return _T("'") + ScriptServer::EscapeForJScriptLiteral(value) + _T("'");
      };

      auto number = [](unsigned __int64 value) -> String
      {
         return Formatter::Format("{0}", value);
      };

      String call = _T("OnIMAPMessageMoved(HMAILSERVER_ACCOUNT, ");
      call += text(event.event_id) + _T(", ");
      call += text(event.operation) + _T(", ");
      call += text(event.user) + _T(", ");
      call += number((unsigned __int64) event.source_folder_id) + _T(", ");
      call += text(event.source_folder) + _T(", ");
      call += number(event.source_uid_validity) + _T(", ");
      call += number(event.source_uid) + _T(", ");
      call += number((unsigned __int64) event.destination_folder_id) + _T(", ");
      call += text(event.destination_folder) + _T(", ");
      call += number(event.destination_uid_validity) + _T(", ");
      call += number(event.destination_uid) + _T(", ");
      call += number((unsigned __int64) event.source_message_id) + _T(", ");
      call += number((unsigned __int64) event.message_id) + _T(")");
      return call;
   }

   std::shared_ptr<ScriptObjectContainer>
   IMAPMoveDispatch::BuildContainer(const IMAPMoveEvent &event, std::shared_ptr<Account> account)
   {
      std::shared_ptr<ScriptObjectContainer> container = std::shared_ptr<ScriptObjectContainer>(new ScriptObjectContainer);
      container->AddObject("HMAILSERVER_ACCOUNT", account, ScriptObject::OTAccount);

      // In the handler's order, after the account: the values the JavaScript
      // engine calls it with (ScriptArgument). The Windows engines read the call
      // text BuildCall makes instead.
      container->AddTextArgument(event.event_id);
      container->AddTextArgument(event.operation);
      container->AddTextArgument(event.user);
      container->AddNumberArgument((long long) event.source_folder_id);
      container->AddTextArgument(event.source_folder);
      container->AddNumberArgument((long long) event.source_uid_validity);
      container->AddNumberArgument((long long) event.source_uid);
      container->AddNumberArgument((long long) event.destination_folder_id);
      container->AddTextArgument(event.destination_folder);
      container->AddNumberArgument((long long) event.destination_uid_validity);
      container->AddNumberArgument((long long) event.destination_uid);
      container->AddNumberArgument((long long) event.source_message_id);
      container->AddNumberArgument((long long) event.message_id);
      return container;
   }

   void
   IMAPMoveDispatch::Fire(IMAPMoveEvent event)
   {
      // Switched off since the move was queued: what was waiting is let go.
      // FireEvent asks the scripting switch and the handler itself.
      if (!Configuration::Instance()->GetIMAPMessageMovedEvents())
      {
         LOG_DEBUG(Formatter::Format("OnIMAPMessageMoved for message {0} not fired: IMAPMessageMovedEvents was switched off after the move.", event.message_id));
         return;
      }

      // Read now, on this thread, so that the handler is given the account as it
      // is: the one the IMAP session holds may be a cached copy.
      std::shared_ptr<Account> account = std::shared_ptr<Account>(new Account);
      if (!PersistentAccount::ReadObject(account, event.account_id))
      {
         LOG_APPLICATION(Formatter::Format("OnIMAPMessageMoved for message {0} not fired: account {1} could not be read; it may have been deleted since the move.",
            event.message_id, event.account_id));
         return;
      }

      if (event.event_id.IsEmpty())
         event.event_id = GUIDCreator::GetGUID();

      // For the language of the script in force; see Events::FireOnClientValidatePassword.
      const String language = ScriptServer::Instance()->GetLoadedLanguage();
      const String call = BuildCall(event, language);

      LOG_DEBUG(Formatter::Format("OnIMAPMessageMoved {0}: message {1}, UID {2} to {3}.", event.event_id, event.message_id,
         event.source_uid, event.destination_uid));

      ScriptServer::Instance()->FireEvent(ScriptServer::EventOnIMAPMessageMoved, call, BuildContainer(event, account), language);
   }

   namespace
   {
      void Expect(bool condition, const char *what)
      {
         if (!condition)
            throw std::logic_error(std::string("IMAPMoveDispatchTester: ") + what);
      }

      IMAPMoveDispatch::Facts OwnMove()
      {
         IMAPMoveDispatch::Facts facts;
         facts.events_on = true;
         facts.scripting_on = true;
         facts.handler_declared = true;
         facts.session_account_id = 7;
         facts.source_account_id = 7;
         facts.destination_account_id = 7;
         return facts;
      }

      IMAPMoveRecord ThreeMoves(IMAPMoveRecord::Operation operation)
      {
         IMAPMoveRecord record;
         record.operation = operation;
         record.events_decided = true;
         record.events_wanted = true;
         record.account_id = 7;
         record.user = _T("owner@example.test");
         record.source_folder_id = 11;
         record.source_folder = _T("INBOX");
         record.source_uid_validity = 1761800000;
         record.destination_folder_id = 12;
         record.destination_folder = _T("Archive.2026");
         record.destination_uid_validity = 1761800100;

         for (unsigned int i = 0; i < 3; i++)
         {
            IMAPMoveRecord::Moved item;
            item.source_uid = 40 + i;
            item.destination_uid = 1 + i;
            item.source_message_id = 500 + i;
            item.message_id = 900 + i;
            record.moved.push_back(item);
         }

         return record;
      }

      std::vector<IMAPMoveEvent> Events(size_t count)
      {
         std::vector<IMAPMoveEvent> events;
         for (size_t i = 0; i < count; i++)
         {
            IMAPMoveEvent event;
            event.message_id = 1000 + static_cast<__int64>(i);
            events.push_back(event);
         }

         return events;
      }

      IMAPMoveEvent SampleEvent()
      {
         IMAPMoveEvent event;
         event.event_id = _T("{0F0E0D0C-0B0A-4908-8706-050403020100}");
         event.operation = _T("UID MOVE");
         event.user = _T("owner@example.test");
         event.account_id = 7;
         event.source_folder_id = 11;
         event.source_folder = _T("Say \"hi\" it's a \\ folder");
         event.source_uid_validity = 1761800000;
         event.source_uid = 41;
         event.destination_folder_id = 12;
         event.destination_folder = _T("Junk");
         event.destination_uid_validity = 1761800100;
         event.destination_uid = 2;
         event.source_message_id = 501;
         event.message_id = 3000000000LL;
         return event;
      }
   }

   void
   IMAPMoveDispatchTester::Test()
   {
      // The rule: the account's own folders, all three switches on.
      Expect(IMAPMoveDispatch::RaisesMoveEvents(OwnMove()), "a move between two of the session account's own folders raises the event");

      IMAPMoveDispatch::Facts facts = OwnMove();
      facts.events_on = false;
      Expect(!IMAPMoveDispatch::RaisesMoveEvents(facts), "IMAPMessageMovedEvents off raises nothing");

      facts = OwnMove();
      facts.scripting_on = false;
      Expect(!IMAPMoveDispatch::RaisesMoveEvents(facts), "scripting off raises nothing");

      facts = OwnMove();
      facts.handler_declared = false;
      Expect(!IMAPMoveDispatch::RaisesMoveEvents(facts), "a script without OnIMAPMessageMoved raises nothing");

      facts = OwnMove();
      facts.destination_account_id = 8;
      Expect(!IMAPMoveDispatch::RaisesMoveEvents(facts), "a move into another account's shared folder raises nothing");

      facts = OwnMove();
      facts.source_account_id = 8;
      facts.destination_account_id = 8;
      Expect(!IMAPMoveDispatch::RaisesMoveEvents(facts), "a move between two folders another account shares or delegates raises nothing");

      facts = OwnMove();
      facts.destination_public = true;
      Expect(!IMAPMoveDispatch::RaisesMoveEvents(facts), "a move into a public folder raises nothing");

      facts = OwnMove();
      facts.source_public = true;
      Expect(!IMAPMoveDispatch::RaisesMoveEvents(facts), "a move out of a public folder raises nothing");

      facts = OwnMove();
      facts.session_account_id = 0;
      facts.source_account_id = 0;
      facts.destination_account_id = 0;
      Expect(!IMAPMoveDispatch::RaisesMoveEvents(facts), "a session with no account raises nothing");

      // Only committed moves, one event per message whose original was removed.
      IMAPMoveRecord record = ThreeMoves(IMAPMoveRecord::OperationMove);
      Expect(IMAPMoveDispatch::EventsOf(record).empty(), "a MOVE whose expunge has not removed an original raises nothing");

      std::vector<__int64> deleted;
      deleted.push_back(500);
      deleted.push_back(502);
      deleted.push_back(777);
      record.MarkSourcesRemoved(deleted);

      std::vector<IMAPMoveEvent> events = IMAPMoveDispatch::EventsOf(record);
      Expect(events.size() == 2, "a MOVE raises one event for each message whose original its expunge removed, and none for the one it could not");
      Expect(events[0].source_uid == 40 && events[0].destination_uid == 1 && events[0].source_message_id == 500 && events[0].message_id == 900,
         "the first event is the first message's, with its UIDs and ids");
      Expect(events[1].source_uid == 42 && events[1].destination_uid == 3 && events[1].source_message_id == 502 && events[1].message_id == 902,
         "the second event is the third message's, in the order the command moved them");
      Expect(events[0].operation == _T("MOVE") && events[0].user == _T("owner@example.test") && events[0].account_id == 7,
         "an event names the operation, the user and the account");
      Expect(events[1].source_folder_id == 11 && events[1].source_folder == _T("INBOX") && events[1].source_uid_validity == 1761800000 &&
             events[1].destination_folder_id == 12 && events[1].destination_folder == _T("Archive.2026") && events[1].destination_uid_validity == 1761800100,
         "an event names both folders, by id and name, with their UIDVALIDITY");
      Expect(events[0].event_id.IsEmpty(), "an event's id is made when it is fired, not when it is queued");

      IMAPMoveRecord uidMove = ThreeMoves(IMAPMoveRecord::OperationUidMove);
      uidMove.MarkSourcesRemoved(deleted);
      events = IMAPMoveDispatch::EventsOf(uidMove);
      Expect(events.size() == 2 && events[0].operation == _T("UID MOVE"), "a UID MOVE raises the same events, named UID MOVE");

      IMAPMoveRecord copy = ThreeMoves(IMAPMoveRecord::OperationCopy);
      copy.MarkSourcesRemoved(deleted);
      Expect(IMAPMoveDispatch::EventsOf(copy).empty(), "a COPY raises nothing");

      IMAPMoveRecord uidCopy = ThreeMoves(IMAPMoveRecord::OperationUidCopy);
      uidCopy.MarkSourcesRemoved(deleted);
      Expect(IMAPMoveDispatch::EventsOf(uidCopy).empty(), "a UID COPY raises nothing");

      IMAPMoveRecord unwanted = ThreeMoves(IMAPMoveRecord::OperationMove);
      unwanted.events_wanted = false;
      unwanted.MarkSourcesRemoved(deleted);
      Expect(IMAPMoveDispatch::EventsOf(unwanted).empty(), "a MOVE whose event is not wanted raises nothing");

      IMAPMoveRecord failed = ThreeMoves(IMAPMoveRecord::OperationMove);
      failed.MarkSourcesRemoved(deleted);
      failed.Clear();
      Expect(IMAPMoveDispatch::EventsOf(failed).empty() && failed.lessons.empty(), "a MOVE that failed and was rolled back raises nothing and teaches nothing");

      // The queue's bound: the newest dropped, one drainer at a time.
      IMAPMoveEventQueue queue(3);
      bool start = false;
      Expect(queue.Offer(Events(2), start) == 2 && start, "the first events are taken, and a drainer is to be started");
      Expect(queue.Offer(Events(2), start) == 1 && !start, "past the bound the newest are dropped, and no second drainer is started");
      Expect(queue.GetDepth() == 3 && queue.GetDrainerRunning(), "the queue holds its bound, with its drainer counted");
      Expect(queue.Offer(Events(1), start) == 0 && !start, "a full queue takes nothing");

      IMAPMoveEvent taken;
      Expect(queue.Take(taken) && taken.message_id == 1000, "the drainer takes the oldest first");
      Expect(queue.Take(taken) && taken.message_id == 1001, "and then the next");
      Expect(queue.Take(taken) && taken.message_id == 1000, "and then the one taken from the second offer");
      Expect(!queue.Take(taken) && !queue.GetDrainerRunning(), "an empty queue ends its drainer");
      Expect(queue.Offer(Events(1), start) == 1 && start, "after its drainer has ended, the next offer starts another");

      queue.DrainerEnded();
      Expect(queue.GetDepth() == 1 && !queue.GetDrainerRunning(), "a drainer that stopped early leaves what is waiting");
      Expect(queue.Offer(Events(1), start) == 1 && start, "and the next offer starts a drainer for it");

      Expect(IMAPMoveDispatch::Queue().GetCapacity() == IMAPMoveDispatch::QueueCapacity, "the server's queue holds QueueCapacity events");

      // The call text, every text a literal whole.
      IMAPMoveEvent event = SampleEvent();
      Expect(IMAPMoveDispatch::BuildCall(event, _T("VBScript")) ==
         String(_T("OnIMAPMessageMoved(HMAILSERVER_ACCOUNT, \"{0F0E0D0C-0B0A-4908-8706-050403020100}\", \"UID MOVE\", \"owner@example.test\", ")
                _T("11, \"Say \"\"hi\"\" it's a \\ folder\", 1761800000, 41, 12, \"Junk\", 1761800100, 2, 501, 3000000000)")),
         "the VBScript call quotes the folder's name with its quotes doubled");
      Expect(IMAPMoveDispatch::BuildCall(event, _T("JScript")) ==
         String(_T("OnIMAPMessageMoved(HMAILSERVER_ACCOUNT, '{0F0E0D0C-0B0A-4908-8706-050403020100}', 'UID MOVE', 'owner@example.test', ")
                _T("11, 'Say \\\"hi\\\" it\\'s a \\\\ folder', 1761800000, 41, 12, 'Junk', 1761800100, 2, 501, 3000000000)")),
         "the JScript call escapes the folder's quotes and its backslash");
      Expect(IMAPMoveDispatch::BuildCall(event, _T("JavaScript")).IsEmpty(), "no call text is built for JavaScript");

      // The container: the account, then the thirteen values in the handler's order.
      std::shared_ptr<Account> account = std::shared_ptr<Account>(new Account);
      account->SetAddress(_T("owner@example.test"));
      std::shared_ptr<ScriptObjectContainer> container = IMAPMoveDispatch::BuildContainer(event, account);
      Expect(container->FindObject(_T("HMAILSERVER_ACCOUNT")) && container->FindObject(_T("HMAILSERVER_ACCOUNT"))->eType == ScriptObject::OTAccount,
         "the account is HMAILSERVER_ACCOUNT");

      const std::vector<ScriptArgument> &arguments = container->GetArguments();
      Expect(arguments.size() == 13, "thirteen values follow the account");
      Expect(arguments[0].kind == ScriptArgument::KindText && arguments[0].text == event.event_id, "the event's id");
      Expect(arguments[1].text == _T("UID MOVE") && arguments[2].text == _T("owner@example.test"), "the operation and the user");
      Expect(arguments[3].kind == ScriptArgument::KindNumber && arguments[3].number == 11 && arguments[4].text == event.source_folder &&
             arguments[5].number == 1761800000 && arguments[6].number == 41, "the source folder's id, name and UIDVALIDITY, and the source UID");
      Expect(arguments[7].number == 12 && arguments[8].text == _T("Junk") && arguments[9].number == 1761800100 && arguments[10].number == 2,
         "the destination folder's id, name and UIDVALIDITY, and the destination UID");
      Expect(arguments[11].number == 501 && arguments[12].number == 3000000000LL, "the message's id in the source and its id now");

#ifdef HM_PLATFORM_POSIX
      // A JavaScript handler called as FireEvent calls it, reading every argument.
      std::vector<String> objectArguments = ScriptServer::JavaScriptObjectArguments(ScriptServer::EventOnIMAPMessageMoved);
      Expect(objectArguments.size() == 1 && objectArguments[0] == _T("HMAILSERVER_ACCOUNT"), "the JavaScript handler is given the account first");

      const String script = _T("function OnIMAPMessageMoved(account, eventId, operation, user, sourceFolderId, sourceFolder, sourceUidValidity, sourceUid, ")
         _T("destinationFolderId, destinationFolder, destinationUidValidity, destinationUid, sourceMessageId, messageId) {\n")
         _T("   return [account.Address, eventId, operation, user, sourceFolderId, sourceFolder, sourceUidValidity, sourceUid,\n")
         _T("      destinationFolderId, destinationFolder, destinationUidValidity, destinationUid, sourceMessageId, messageId,\n")
         _T("      typeof sourceUid, arguments.length].join('|');\n")
         _T("}\n");

      ScriptRunOutcome outcome = ScriptBinding::CallHandler(script, _T("EventHandlers.js"), _T("OnIMAPMessageMoved"), objectArguments, container, 10);
      Expect(outcome.completed && outcome.function_exists, "the JavaScript handler runs");
      Expect(outcome.returned == String(_T("owner@example.test|{0F0E0D0C-0B0A-4908-8706-050403020100}|UID MOVE|owner@example.test|11|")
                                        _T("Say \"hi\" it's a \\ folder|1761800000|41|12|Junk|1761800100|2|501|3000000000|number|14")),
         "the JavaScript handler is given the account and the thirteen values, unquoted, the numbers as numbers");
#endif
   }
}
