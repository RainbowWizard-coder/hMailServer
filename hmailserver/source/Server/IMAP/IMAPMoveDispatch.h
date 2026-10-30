// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <boost/thread/mutex.hpp>

#include <deque>
#include <vector>

namespace HM
{
   class Account;
   class IMAPConnection;
   class IMAPFolder;
   class Message;
   class ScriptObjectContainer;

   // What one committed IMAP COPY, UID COPY, MOVE or UID MOVE did, as the command
   // collects it while it copies each message and hands it, once the command's
   // work is done, to the one place that acts on it: IMAPMoveDispatch::Committed.
   // A command that fails takes its copies out again and forgets all of it
   // (Clear), so nothing is ever told of a copy or a move that did not happen.
   class IMAPMoveRecord
   {
   public:
      enum Operation
      {
         OperationCopy = 0,
         OperationUidCopy = 1,
         OperationMove = 2,
         OperationUidMove = 3
      };

      // SpamAssassinLearnOnMove's lesson of one copy into or out of the Junk
      // folder (SpamAssassinLearner::IsLesson), decided as the copy is made.
      class Lesson
      {
      public:
         std::shared_ptr<Message> copy;
         std::shared_ptr<const Account> destination_owner;
         bool spam = false;
      };

      // One message a MOVE moved: its UID in the folder it left and in the one it
      // went to, its id there before and its id now (a copy is a new row), and
      // whether its original has been taken out of the source - which the MOVE's
      // expunge says, after the copy (MarkSourcesRemoved).
      class Moved
      {
      public:
         unsigned int source_uid = 0;
         unsigned int destination_uid = 0;
         __int64 source_message_id = 0;
         __int64 message_id = 0;
         bool source_removed = false;
      };

      IMAPMoveRecord();

      // Forgets every lesson and every move, for a command that failed.
      void Clear();

      // The originals the MOVE's expunge deleted, by their ids: each move whose
      // original is among them is committed, its source removal included. One
      // the expunge could not delete is not.
      void MarkSourcesRemoved(const std::vector<__int64> &deleted_message_ids);

      Operation operation;
      std::vector<Lesson> lessons;

      // The move event's half, filled in by IMAPMoveDispatch::PrepareMoveEvents
      // on the command's first message, and only when the event is wanted: the
      // account the folders belong to, the user signed in, and the two folders -
      // their ids, their names as LIST gives them, decoded, and their
      // UIDVALIDITY. A MOVE has one source and one destination.
      bool events_decided;
      bool events_wanted;
      __int64 account_id;
      String user;
      __int64 source_folder_id;
      String source_folder;
      unsigned int source_uid_validity;
      __int64 destination_folder_id;
      String destination_folder;
      unsigned int destination_uid_validity;
      std::vector<Moved> moved;
   };

   // One message's OnIMAPMessageMoved, as its handler is given it.
   class IMAPMoveEvent
   {
   public:
      IMAPMoveEvent();

      // Unique to this event: made when it is fired (a braced UUID).
      String event_id;
      // "MOVE" or "UID MOVE".
      String operation;
      // The user signed in to the session that moved it.
      String user;
      // The account that owns both folders.
      __int64 account_id;
      __int64 source_folder_id;
      String source_folder;
      unsigned int source_uid_validity;
      unsigned int source_uid;
      __int64 destination_folder_id;
      String destination_folder;
      unsigned int destination_uid_validity;
      unsigned int destination_uid;
      // The message's id in the source, which is gone now, and its id now, in the
      // destination.
      __int64 source_message_id;
      __int64 message_id;
   };

   // The events waiting to be fired, oldest first, and at most a capacity of them:
   // an IMAP command never waits for a script, so a queue that is full takes no
   // more and the newest are dropped, which the caller logs. One drainer at a time
   // fires them, in order, on the asynchronous work queue; Offer says when one has
   // to be started, Take ends it when nothing is left, and DrainerEnded lets a
   // drainer that stopped before then - the servers stopped, its task never run -
   // be replaced by the next Offer. Held in memory only: a restart of the service
   // loses what was waiting.
   class IMAPMoveEventQueue
   {
   public:
      explicit IMAPMoveEventQueue(size_t capacity);

      // As many of events as there is room for, from the first; returns how many.
      // startDrainer is set when no drainer is running and one has to be started
      // for them, and the queue then counts one as running.
      size_t Offer(const std::vector<IMAPMoveEvent> &events, bool &startDrainer);

      // The oldest event, for the drainer. False when there is none: the drainer
      // stops, and the queue no longer counts one as running.
      bool Take(IMAPMoveEvent &event);

      // The drainer stopped, or was never run, with events perhaps still waiting.
      void DrainerEnded();

      size_t GetDepth() const;
      size_t GetCapacity() const;
      bool GetDrainerRunning() const;

   private:
      mutable boost::mutex mutex_;
      std::deque<IMAPMoveEvent> pending_;
      size_t capacity_;
      bool drainer_running_;
   };

   // THE POST-COMMIT DISPATCH POINT. A COPY or a MOVE does its work on the IMAP
   // thread - each copy written, and for a MOVE the originals expunged - and then,
   // once and only when it has succeeded, hands what it did to Committed. Nothing
   // is done there but queueing: the lessons of SpamAssassinLearnOnMove go to the
   // asynchronous work queue as they always have (SpamAssassinLearner::Teach), and
   // the move events to the bounded queue above, whose drainer fires them on the
   // asynchronous work queue too, outside every folder lock and transaction. A
   // learning backend added later is added here, beside the lessons, so that the
   // IMAP commands keep calling one thing.
   //
   // OnIMAPMessageMoved (GitLab work item #109) is raised for a committed MOVE or
   // UID MOVE - never for a COPY or a UID COPY, and never inferred from a COPY, a
   // STORE and an EXPUNGE - whose source and destination are folders of the
   // account signed in to the session, neither of them public: a move in another
   // account's shared or delegated folders, or into or out of a public folder,
   // raises nothing. Once for each message moved, in the order the command moved
   // them, and only for those whose original the MOVE's expunge took out of the
   // source. It is raised only while the IMAPMessageMovedEvents setting and
   // scripting are both on and the script in force declares the handler; with any
   // of them off a MOVE records nothing for it.
   class IMAPMoveDispatch
   {
   public:
      // The most events that wait at once. Each is a few hundred bytes, so a full
      // queue is a few megabytes; a handler slower than the moves it is told of
      // loses the newest rather than holding up a client or the memory.
      static const size_t QueueCapacity = 10000;

      // What decides whether a MOVE's messages raise the event, as plain values.
      class Facts
      {
      public:
         Facts();

         bool events_on;
         bool scripting_on;
         bool handler_declared;
         __int64 session_account_id;
         __int64 source_account_id;
         __int64 destination_account_id;
         bool source_public;
         bool destination_public;
      };

      // The rule: all three switches on, and both folders the session account's
      // own, neither public.
      static bool RaisesMoveEvents(const Facts &facts);

      // Asked by a MOVE as it moves its first message: decides, once for the
      // command, whether its moves raise the event, and when they do, fills the
      // record's event half. With the setting off this reads one setting and
      // nothing else.
      static void PrepareMoveEvents(std::shared_ptr<IMAPConnection> connection, std::shared_ptr<IMAPFolder> source,
                                    std::shared_ptr<IMAPFolder> destination, IMAPMoveRecord &record);

      // The dispatch point: what a COPY or a MOVE that succeeded hands on, after
      // all of its work. Queues the lessons and the events, and empties the record.
      static void Committed(IMAPMoveRecord &record);

      // The events a record raises: one for each message of a MOVE or UID MOVE
      // whose original was removed, when the record wants them; none for a COPY.
      static std::vector<IMAPMoveEvent> EventsOf(const IMAPMoveRecord &record);

      // The call the Windows engines are handed for the event, in the language
      // given (VBScript or JScript; empty for any other), every text quoted for it
      // (ScriptServer::EscapeForVBScriptLiteral, EscapeForJScriptLiteral).
      static String BuildCall(const IMAPMoveEvent &event, const String &language);

      // The event's objects and values: the account as HMAILSERVER_ACCOUNT, and the
      // thirteen values in the handler's order, which the JavaScript engine passes
      // after it.
      static std::shared_ptr<ScriptObjectContainer> BuildContainer(const IMAPMoveEvent &event, std::shared_ptr<Account> account);

      // Fires one event, on the calling thread: the drainer's.
      static void Fire(IMAPMoveEvent event);

      // The queue the events wait in.
      static IMAPMoveEventQueue &Queue();

      // The name of the operation, as the handler is given it.
      static String OperationName(IMAPMoveRecord::Operation operation);

   private:

      static void QueueEvents_(const std::vector<IMAPMoveEvent> &events);
      static String FolderPath_(std::shared_ptr<IMAPFolder> folder);
   };

   // The rules above held to examples: which MOVEs raise the event and which do
   // not (a COPY, a failed MOVE, a move of another account's folders, a public
   // folder, any switch off), one event per message whose source was removed and
   // none for one whose was not, the queue's bound - the newest dropped, the
   // drainer started once and again after it ends - the call text in VBScript and
   // JScript with a folder name that tries to end its literal, and the container's
   // values in the handler's order; on the POSIX build a JavaScript handler called
   // as the event calls it, reading every argument. No database, no network.
   // ClassTester::DoTests runs it on Windows and imap-move-event-tests on Linux.
   class IMAPMoveDispatchTester
   {
   public:
      void Test();
   };
}
