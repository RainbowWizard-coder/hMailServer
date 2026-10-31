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

#include <algorithm>
#include <exception>
#include <memory>
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
                     event.message_id, event.fields ? event.fields->account_id : 0, error.what()));
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

   IMAPMoveEventFields::IMAPMoveEventFields() :
      account_id(0),
      source_folder_id(0),
      source_uid_validity(0),
      destination_folder_id(0),
      destination_uid_validity(0)
   {
   }

   size_t
   IMAPMoveEventFields::Bytes() const
   {
      // Each text with its terminator, in the characters String holds.
      const size_t characters = operation.size() + user.size() + source_folder.size() + destination_folder.size() + 4;
      return sizeof(IMAPMoveEventFields) + characters * sizeof(String::value_type);
   }

   IMAPMoveEvent::IMAPMoveEvent() :
      source_uid(0),
      destination_uid(0),
      source_message_id(0),
      message_id(0)
   {
   }

   IMAPMoveEventQueue::IMAPMoveEventQueue(size_t capacity, size_t byteCapacity) :
      capacity_(capacity),
      byte_capacity_(byteCapacity),
      bytes_(0),
      drainer_running_(false)
   {
   }

   size_t
   IMAPMoveEventQueue::EventBytes()
   {
      return sizeof(IMAPMoveEvent);
   }

   size_t
   IMAPMoveEventQueue::FieldBytes_(const IMAPMoveEvent &event)
   {
      return event.fields ? event.fields->Bytes() : 0;
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

         // The fields are charged with the first of the events that share them
         // and wait one after the other, and given back with the last (Take).
         const bool sharesPrevious = !pending_.empty() && pending_.back().fields == event.fields;
         const size_t cost = EventBytes() + (sharesPrevious ? 0 : FieldBytes_(event));
         if (cost > byte_capacity_ - std::min(bytes_, byte_capacity_))
            break;

         pending_.push_back(event);
         bytes_ += cost;
         admitted++;
      }

      if (admitted > 0 && !drainer_running_)
      {
         drainer_running_ = true;
         startDrainer = true;
      }

      return admitted;
   }

   size_t
   IMAPMoveEventQueue::Room(size_t fieldBytes) const
   {
      boost::lock_guard<boost::mutex> guard(mutex_);

      if (pending_.size() >= capacity_)
         return 0;

      const size_t free = byte_capacity_ - std::min(bytes_, byte_capacity_);
      if (free < fieldBytes + EventBytes())
         return 0;

      return std::min(capacity_ - pending_.size(), (free - fieldBytes) / EventBytes());
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

      size_t given = EventBytes();
      if (pending_.empty() || pending_.front().fields != event.fields)
         given += FieldBytes_(event);
      bytes_ -= std::min(bytes_, given);
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

   size_t
   IMAPMoveEventQueue::GetBytes() const
   {
      boost::lock_guard<boost::mutex> guard(mutex_);
      return bytes_;
   }

   size_t
   IMAPMoveEventQueue::GetByteCapacity() const
   {
      return byte_capacity_;
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

      if (CountEvents(record) > 0)
         QueueEvents_(record);

      record.Clear();
   }

   size_t
   IMAPMoveDispatch::CountEvents(const IMAPMoveRecord &record)
   {
      // A COPY moves nothing, and a COPY followed by a STORE and an EXPUNGE is
      // three commands, which nothing here joins into a move.
      if (record.operation != IMAPMoveRecord::OperationMove && record.operation != IMAPMoveRecord::OperationUidMove)
         return 0;

      if (!record.events_wanted)
         return 0;

      // A message whose original the expunge did not delete is in both folders:
      // not moved.
      size_t count = 0;
      for (const IMAPMoveRecord::Moved &item : record.moved)
      {
         if (item.source_removed)
            count++;
      }

      return count;
   }

   std::shared_ptr<const IMAPMoveEventFields>
   IMAPMoveDispatch::FieldsOf(const IMAPMoveRecord &record)
   {
      std::shared_ptr<IMAPMoveEventFields> fields = std::make_shared<IMAPMoveEventFields>();
      fields->operation = OperationName(record.operation);
      fields->user = record.user;
      fields->account_id = record.account_id;
      fields->source_folder_id = record.source_folder_id;
      fields->source_folder = record.source_folder;
      fields->source_uid_validity = record.source_uid_validity;
      fields->destination_folder_id = record.destination_folder_id;
      fields->destination_folder = record.destination_folder;
      fields->destination_uid_validity = record.destination_uid_validity;
      return fields;
   }

   std::vector<IMAPMoveEvent>
   IMAPMoveDispatch::EventsOf(const IMAPMoveRecord &record)
   {
      if (CountEvents(record) == 0)
         return std::vector<IMAPMoveEvent>();

      return EventsOf(record, FieldsOf(record), QueueCapacity);
   }

   std::vector<IMAPMoveEvent>
   IMAPMoveDispatch::EventsOf(const IMAPMoveRecord &record, std::shared_ptr<const IMAPMoveEventFields> fields, size_t most)
   {
      std::vector<IMAPMoveEvent> events;

      // By value: std::min takes its arguments by reference, which QueueCapacity, a
      // constant with no definition of its own, is not to be bound to.
      const size_t capacity = QueueCapacity;
      const size_t count = std::min(std::min(CountEvents(record), most), capacity);
      if (count == 0 || !fields)
         return events;

      events.reserve(count);

      for (const IMAPMoveRecord::Moved &item : record.moved)
      {
         if (events.size() >= count)
            break;

         if (!item.source_removed)
            continue;

         IMAPMoveEvent event;
         event.fields = fields;
         event.source_uid = item.source_uid;
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
      static IMAPMoveEventQueue *queue = new IMAPMoveEventQueue(QueueCapacity, QueueBytes);
      return *queue;
   }

   void
   IMAPMoveDispatch::QueueEvents_(const IMAPMoveRecord &record)
   {
      const size_t raised = CountEvents(record);

      // The fields once for the command, and no more events built than the queue
      // has room for now: a MOVE of a whole mailbox between two folders with long
      // names holds no more memory on this thread than the queue may.
      std::shared_ptr<const IMAPMoveEventFields> fields = FieldsOf(record);
      const std::vector<IMAPMoveEvent> events = EventsOf(record, fields, Queue().Room(fields->Bytes()));

      bool startDrainer = false;
      const size_t admitted = events.empty() ? 0 : Queue().Offer(events, startDrainer);

      if (admitted < raised)
      {
         // The client is not made to wait for a script: the newest are dropped,
         // and said so, once for the command.
         LOG_APPLICATION(Formatter::Format("OnIMAPMessageMoved: {0} of the {1} messages a {2} by {3} moved were not queued for the event script, "
            "because the events waiting already are the most there may be ({4}). The handler is slower than the moves it is told of.",
            (unsigned __int64) (raised - admitted), (unsigned __int64) raised, fields->operation, fields->user,
            Formatter::Format("{0} events, or {1} bytes of their values", (unsigned __int64) Queue().GetCapacity(),
               (unsigned __int64) Queue().GetByteCapacity())));
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

   const IMAPMoveEventFields &
   IMAPMoveDispatch::FieldsOf_(const IMAPMoveEvent &event)
   {
      // An event made by hand without them, as a test makes one, reads as empty.
      static const IMAPMoveEventFields none;
      return event.fields ? *event.fields : none;
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

      const IMAPMoveEventFields &fields = FieldsOf_(event);

      String call = _T("OnIMAPMessageMoved(HMAILSERVER_ACCOUNT, ");
      call += text(event.event_id) + _T(", ");
      call += text(fields.operation) + _T(", ");
      call += text(fields.user) + _T(", ");
      call += number((unsigned __int64) fields.source_folder_id) + _T(", ");
      call += text(fields.source_folder) + _T(", ");
      call += number(fields.source_uid_validity) + _T(", ");
      call += number(event.source_uid) + _T(", ");
      call += number((unsigned __int64) fields.destination_folder_id) + _T(", ");
      call += text(fields.destination_folder) + _T(", ");
      call += number(fields.destination_uid_validity) + _T(", ");
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
      const IMAPMoveEventFields &fields = FieldsOf_(event);
      container->AddTextArgument(event.event_id);
      container->AddTextArgument(fields.operation);
      container->AddTextArgument(fields.user);
      container->AddNumberArgument((long long) fields.source_folder_id);
      container->AddTextArgument(fields.source_folder);
      container->AddNumberArgument((long long) fields.source_uid_validity);
      container->AddNumberArgument((long long) event.source_uid);
      container->AddNumberArgument((long long) fields.destination_folder_id);
      container->AddTextArgument(fields.destination_folder);
      container->AddNumberArgument((long long) fields.destination_uid_validity);
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

      if (!event.fields)
         return;

      // Read now, on this thread, so that the handler is given the account as it
      // is: the one the IMAP session holds may be a cached copy.
      std::shared_ptr<Account> account = std::shared_ptr<Account>(new Account);
      if (!PersistentAccount::ReadObject(account, event.fields->account_id))
      {
         LOG_APPLICATION(Formatter::Format("OnIMAPMessageMoved for message {0} not fired: account {1} could not be read; it may have been deleted since the move.",
            event.message_id, event.fields->account_id));
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
         std::shared_ptr<IMAPMoveEventFields> fields = std::make_shared<IMAPMoveEventFields>();
         fields->operation = _T("UID MOVE");
         fields->user = _T("owner@example.test");
         fields->account_id = 7;
         fields->source_folder_id = 11;
         fields->source_folder = _T("Say \"hi\" it's a \\ folder");
         fields->source_uid_validity = 1761800000;
         fields->destination_folder_id = 12;
         fields->destination_folder = _T("Junk");
         fields->destination_uid_validity = 1761800100;

         IMAPMoveEvent event;
         event.event_id = _T("{0F0E0D0C-0B0A-4908-8706-050403020100}");
         event.fields = fields;
         event.source_uid = 41;
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
      Expect(events[0].fields && events[0].fields->operation == _T("MOVE") && events[0].fields->user == _T("owner@example.test") &&
             events[0].fields->account_id == 7,
         "an event names the operation, the user and the account");
      Expect(events[1].fields && events[1].fields->source_folder_id == 11 && events[1].fields->source_folder == _T("INBOX") &&
             events[1].fields->source_uid_validity == 1761800000 && events[1].fields->destination_folder_id == 12 &&
             events[1].fields->destination_folder == _T("Archive.2026") && events[1].fields->destination_uid_validity == 1761800100,
         "an event names both folders, by id and name, with their UIDVALIDITY");
      Expect(events[0].fields == events[1].fields, "the events of one command share its fields");
      Expect(IMAPMoveDispatch::CountEvents(record) == 2, "the count of a record's events is the count of its messages whose original was removed");
      Expect(events[0].event_id.IsEmpty(), "an event's id is made when it is fired, not when it is queued");

      IMAPMoveRecord uidMove = ThreeMoves(IMAPMoveRecord::OperationUidMove);
      uidMove.MarkSourcesRemoved(deleted);
      events = IMAPMoveDispatch::EventsOf(uidMove);
      Expect(events.size() == 2 && events[0].fields->operation == _T("UID MOVE"), "a UID MOVE raises the same events, named UID MOVE");

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

      // A MOVE of a whole mailbox between two folders with names of thousands of
      // characters: one copy of each name, shared by its events, and never more
      // events than the queue holds - nor than it has room for.
      {
         IMAPMoveRecord large = ThreeMoves(IMAPMoveRecord::OperationMove);
         large.source_folder = String(6000, _T('s'));
         large.destination_folder = String(6000, _T('d'));
         large.moved.clear();

         std::vector<__int64> removed;
         for (unsigned int i = 0; i < 20000; i++)
         {
            IMAPMoveRecord::Moved item;
            item.source_uid = 1 + i;
            item.destination_uid = 1 + i;
            item.source_message_id = 100000 + i;
            item.message_id = 200000 + i;
            large.moved.push_back(item);
            removed.push_back(100000 + i);
         }
         large.MarkSourcesRemoved(removed);

         Expect(IMAPMoveDispatch::CountEvents(large) == 20000, "a MOVE of 20,000 messages raises 20,000 events");

         std::vector<IMAPMoveEvent> built = IMAPMoveDispatch::EventsOf(large);
         Expect(built.size() == IMAPMoveDispatch::QueueCapacity, "a MOVE of 20,000 messages builds no more events than the queue holds");
         Expect(built.front().source_uid == 1 && built.back().source_uid == IMAPMoveDispatch::QueueCapacity,
            "the events built are the first messages', in the order the command moved them");

         std::set<const IMAPMoveEventFields *> distinct;
         for (const IMAPMoveEvent &item : built)
            distinct.insert(item.fields.get());
         Expect(distinct.size() == 1 && built.front().fields.use_count() == static_cast<long>(built.size()),
            "the events of a MOVE hold one copy of its fields between them");
         Expect(built.front().fields->source_folder.size() == 6000 && built.front().fields->destination_folder.size() == 6000,
            "the shared fields are the folders' whole names");

         std::shared_ptr<const IMAPMoveEventFields> fields = IMAPMoveDispatch::FieldsOf(large);
         Expect(fields->Bytes() >= 12000 * sizeof(String::value_type), "the fields are charged their names' length");
         Expect(IMAPMoveDispatch::EventsOf(large, fields, 5).size() == 5, "a command builds no more events than it is given room for");
         Expect(IMAPMoveDispatch::EventsOf(large, fields, 0).empty(), "and none when there is no room");

         // The queue charges a command's fields once, and holds its bytes: room
         // for two commands of one message each, and two more events.
         const size_t eventBytes = IMAPMoveEventQueue::EventBytes();
         IMAPMoveEventQueue bytes(IMAPMoveDispatch::QueueCapacity, (fields->Bytes() + eventBytes) * 2 + eventBytes * 2);
         Expect(bytes.Room(fields->Bytes()) == 4 + fields->Bytes() / eventBytes, "an empty queue has room for as many events of a command as its bytes hold");

         bool start = false;
         Expect(bytes.Offer(IMAPMoveDispatch::EventsOf(large, fields, 3), start) == 3 && start, "the events of one command are taken");
         Expect(bytes.GetBytes() == fields->Bytes() + 3 * eventBytes, "and charged their fields once");

         size_t commands = 1;
         for (int i = 0; i < 10; i++)
         {
            std::shared_ptr<const IMAPMoveEventFields> own = IMAPMoveDispatch::FieldsOf(large);
            if (bytes.Offer(IMAPMoveDispatch::EventsOf(large, own, 1), start) == 1)
               commands++;
         }
         Expect(commands == 2, "commands each with fields of their own are taken only while their fields fit in the queue's bytes");
         Expect(bytes.GetBytes() <= bytes.GetByteCapacity(), "the queue never holds more than its bytes");
         Expect(bytes.Room(fields->Bytes()) == 0, "a queue whose bytes are taken has no room for another command's fields");

         IMAPMoveEvent drained;
         size_t taken = 0;
         while (bytes.Take(drained))
            taken++;
         Expect(taken == 4 && bytes.GetBytes() == 0, "every byte charged is given back as the events are taken");

         IMAPMoveEventQueue server(IMAPMoveDispatch::QueueCapacity, IMAPMoveDispatch::QueueBytes);
         const size_t room = server.Room(fields->Bytes());
         Expect(room == IMAPMoveDispatch::QueueCapacity, "the server's empty queue has room for a whole command of such names");
         Expect(server.Offer(IMAPMoveDispatch::EventsOf(large, fields, room), start) == room && server.GetBytes() <= IMAPMoveDispatch::QueueBytes,
            "and holds it within its bytes");
         Expect(server.Room(fields->Bytes()) == 0, "a full queue has no room");
      }

      // The queue's bound: the newest dropped, one drainer at a time.
      IMAPMoveEventQueue queue(3, IMAPMoveDispatch::QueueBytes);
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
      Expect(IMAPMoveDispatch::Queue().GetByteCapacity() == IMAPMoveDispatch::QueueBytes, "in QueueBytes");

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
      Expect(arguments[3].kind == ScriptArgument::KindNumber && arguments[3].number == 11 && arguments[4].text == event.fields->source_folder &&
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
