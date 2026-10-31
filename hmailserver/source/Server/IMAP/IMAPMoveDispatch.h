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

      // The lesson of one copy into or out of the Junk folder
      // (MoveLearning::IsLesson), decided as the copy is made, for the backend
      // chosen then (MoveLearning::Backend).
      class Lesson
      {
      public:
         std::shared_ptr<Message> copy;
         std::shared_ptr<const Account> destination_owner;
         bool spam = false;
         int backend = 0;
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

   // What every event of one MOVE shares: the operation, the user, the account
   // and both folders. Made once for the command (IMAPMoveDispatch::FieldsOf) and
   // held by its events through one shared pointer, never copied into each: a
   // folder's full name is the user's choice and may be thousands of characters
   // (25 levels of 255), and a MOVE may move every message of a mailbox.
   class IMAPMoveEventFields
   {
   public:
      IMAPMoveEventFields();

      // The memory the fields take, their texts counted by their length: what
      // the queue charges once for a command's events (IMAPMoveEventQueue).
      size_t Bytes() const;

      // "MOVE" or "UID MOVE".
      String operation;
      // The user signed in to the session that moved it.
      String user;
      // The account that owns both folders.
      __int64 account_id;
      __int64 source_folder_id;
      String source_folder;
      unsigned int source_uid_validity;
      __int64 destination_folder_id;
      String destination_folder;
      unsigned int destination_uid_validity;
   };

   // One message's OnIMAPMessageMoved, as its handler is given it: what is the
   // message's own, and the command's fields, shared with the command's other
   // events.
   class IMAPMoveEvent
   {
   public:
      IMAPMoveEvent();

      // Unique to this event: made when it is fired (a braced UUID).
      String event_id;
      // The operation, the user, the account and both folders; never null in an
      // event EventsOf made.
      std::shared_ptr<const IMAPMoveEventFields> fields;
      unsigned int source_uid;
      unsigned int destination_uid;
      // The message's id in the source, which is gone now, and its id now, in the
      // destination.
      __int64 source_message_id;
      __int64 message_id;
   };

   // The events waiting to be fired, oldest first, and at most a capacity of them
   // in at most a capacity of bytes: an IMAP command never waits for a script, so
   // a queue that is full takes no more and the newest are dropped, which the
   // caller logs. An event is charged its own size, and the fields it shares with
   // the events of its command once for those of them that wait one after the
   // other, as a command's do (Offer takes them together). One drainer at a time
   // fires them, in order, on the asynchronous work queue; Offer says when one has
   // to be started, Take ends it when nothing is left, and DrainerEnded lets a
   // drainer that stopped before then - the servers stopped, its task never run -
   // be replaced by the next Offer. Held in memory only: a restart of the service
   // loses what was waiting.
   class IMAPMoveEventQueue
   {
   public:
      IMAPMoveEventQueue(size_t capacity, size_t byteCapacity);

      // As many of events as there is room for, from the first; returns how many.
      // startDrainer is set when no drainer is running and one has to be started
      // for them, and the queue then counts one as running.
      size_t Offer(const std::vector<IMAPMoveEvent> &events, bool &startDrainer);

      // How many events of one command, whose shared fields take fieldBytes, there
      // is room for now: what the command builds at most (IMAPMoveDispatch::Committed).
      // Offer still holds the bound, should another command's events come first.
      size_t Room(size_t fieldBytes) const;

      // The oldest event, for the drainer. False when there is none: the drainer
      // stops, and the queue no longer counts one as running.
      bool Take(IMAPMoveEvent &event);

      // The drainer stopped, or was never run, with events perhaps still waiting.
      void DrainerEnded();

      size_t GetDepth() const;
      size_t GetCapacity() const;
      size_t GetBytes() const;
      size_t GetByteCapacity() const;
      bool GetDrainerRunning() const;

      // What one event is charged beside its command's fields.
      static size_t EventBytes();

   private:
      static size_t FieldBytes_(const IMAPMoveEvent &event);

      mutable boost::mutex mutex_;
      std::deque<IMAPMoveEvent> pending_;
      size_t capacity_;
      size_t byte_capacity_;
      size_t bytes_;
      bool drainer_running_;
   };

   // THE POST-COMMIT DISPATCH POINT. A COPY or a MOVE does its work on the IMAP
   // thread - each copy written, and for a MOVE the originals expunged - and then,
   // once and only when it has succeeded, hands what it did to Committed. Nothing
   // is done there but queueing: the lessons of learning from the Junk folder
   // (Common/AntiSpam/MoveLearning.h) go, for SpamAssassin, to the asynchronous
   // work queue as they always have (SpamAssassinLearner::Teach), whose task also
   // makes the plain copy spamd is sent of a message stored compressed, encrypted
   // or in an object store, and for Rspamd
   // to its bounded queue (RspamdLearner::Queue), whose drainer sends them on the
   // asynchronous work queue; the move events go to the bounded queue above,
   // whose drainer fires them on the asynchronous work queue too - all of it
   // outside every folder lock and transaction, so that the IMAP commands keep
   // calling one thing.
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
      // The most events that wait at once, and the most memory they take. An
      // event's own values are a few dozen bytes; the fields a command's events
      // share - above all the two folders' full names, which may be thousands of
      // characters each - are held once for the command and charged once, so a
      // full queue is at most QueueCapacity events in QueueBytes, whether one
      // command filled it or many. A command builds no more events than there is
      // room for (IMAPMoveEventQueue::Room), so a MOVE of a whole mailbox costs
      // no more than that either; a handler slower than the moves it is told of
      // loses the newest rather than holding up a client or the memory.
      static const size_t QueueCapacity = 10000;
      static const size_t QueueBytes = static_cast<size_t>(16) * 1024 * 1024;

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

      // How many events a record raises: one for each message of a MOVE or UID
      // MOVE whose original was removed, when the record wants them; none for a
      // COPY.
      static size_t CountEvents(const IMAPMoveRecord &record);

      // The fields the events of a record share, made once: its operation's name,
      // the user, the account and both folders.
      static std::shared_ptr<const IMAPMoveEventFields> FieldsOf(const IMAPMoveRecord &record);

      // The events a record raises (CountEvents), in the order the command moved
      // the messages, each holding fields - no copy of them - and no more than
      // most of them, and never more than QueueCapacity: the first ones.
      static std::vector<IMAPMoveEvent> EventsOf(const IMAPMoveRecord &record, std::shared_ptr<const IMAPMoveEventFields> fields,
                                                 size_t most);
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

      static void QueueEvents_(const IMAPMoveRecord &record);
      static const IMAPMoveEventFields &FieldsOf_(const IMAPMoveEvent &event);
      static String FolderPath_(std::shared_ptr<IMAPFolder> folder);
   };

   // The rules above held to examples: which MOVEs raise the event and which do
   // not (a COPY, a failed MOVE, a move of another account's folders, a public
   // folder, any switch off), one event per message whose source was removed and
   // none for one whose was not, a MOVE of 20,000 messages between two folders
   // with names of 6,000 characters building no more events than the queue holds
   // and one copy of the names between them, the queue's bounds - the newest
   // dropped, past its count and past its bytes, each command's fields charged
   // once, the drainer started once and again after it ends - the call text in VBScript and
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
