// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <functional>

#include <boost/thread.hpp>

#include "../Persistence/PersistentMessageIndex.h"

namespace HM
{

   class MessageIndexer : public Singleton<MessageIndexer>
   {
   public:
      MessageIndexer();
      ~MessageIndexer(void);

      // The indexer's thread, unless one runs already. Not while the servers
      // are stopping or stopped: from StopServers to the next StartServers -
      // across a Reinitialize, the database manager closed and made again -
      // the setting is kept and StartServers starts the indexer.
      void Start();

      // Asks the thread to stop and waits until it has ended, so that nothing
      // it does comes after what it uses is gone - the database manager, which
      // a Reinitialize resets straight after StopServers. It ends at its next
      // interruption point: a message of a batch, a slice of a sweep, a pause
      // or the minute's wait - one statement or one message at most. progress
      // is called about once a second while it waits (the service controller's
      // checkpoint, as Sandbox::Stop calls it).
      void Stop(const std::function<void()> &progress = std::function<void()>());

      void IndexNow();

   private:

      friend class MessageIndexerTester;

      // An indexer whose thread runs work rather than the indexer's loop - a
      // stand-in with the loop's interruption points and none of its database,
      // for MessageIndexerTester.
      explicit MessageIndexer(const boost::function<void()> &work);

      void WorkerFunc();
      void WorkerFuncInternal();

      void IndexMessages_();

      // One bounded pass of the full-text term index (hm_messageindexterms):
      // advances the backfill cursor by at most one batch, or heals a handful
      // of messages the cursor walked past. Answers true when there is nothing
      // left to do, so the caller knows whether to wait the full minute or
      // only pause briefly between passes.
      bool IndexFullTextBatch_();
      // False when a save failed. skipped says a message was left unindexed
      // because whether its mailbox is encrypted at rest was not known: the
      // backfill goes past it - the reconciliation finds it later, as it finds
      // any message without the complete-marker - and the reconciliation waits
      // its minute rather than asking again at once.
      bool IndexFullTextMessages_(const std::vector<PersistentMessageIndex::IndexableMessage> &messages, bool &skipped);

      boost::thread workerThread_;

      // What workerThread_ runs: WorkerFuncInternal, or a tester's stand-in.
      boost::function<void()> work_;

      // The thread was asked to stop from the thread itself - Stop called on it
      // by something the indexer ran, an OnError script - which cannot wait for
      // itself; it ends at its next interruption point, and a Start meanwhile
      // waits for it rather than taking it for the running indexer.
      bool stop_asked_on_its_thread_;

      int iIndexRunCount;

      // Counts loop iterations so the orphaned-terms sweep - a scan of the
      // whole terms table - runs on the full-index cadence rather than every
      // minute.
      int full_text_iteration_count_;

      // True while a reconciliation that found a full batch is still working
      // through its backlog, so it continues at backfill speed instead of one
      // batch per hourly gate.
      bool full_text_reconcile_active_;

      boost::recursive_mutex starterMutex_;

      Event index_now_;
   };

   // A stand-in pass in the indexer's thread, with no database: Stop waits for
   // the pass in flight before it returns, and a Start after a Stop starts a new
   // thread - where until 10 October 2026 Stop interrupted and returned, the
   // pass went on to the database after a Reinitialize had closed it, which
   // ended the Linux server in the Alpine suite, and the Start after it took the
   // stopping thread for the running indexer and started none. ClassTester runs
   // Test on Windows, business-object-tests on Linux; TestAnExceptionOnItsThread
   // only business-object-tests, since on Windows the exception handler writes a
   // minidump of the server it runs in.
   class MessageIndexerTester
   {
   public:
      void Test();

      // An exception on the indexer's thread ends that thread and not the
      // process, and the next Start starts the indexer again. The handler
      // reports it (HM4208): the caller has stopped the log's writes
      // (Logger::ForbidWritesForThisProcess), or the report is written.
      void TestAnExceptionOnItsThread();

   private:
      void TestStopWaitsForThePassInFlight_();
      void TestAStartAfterAStopIsANewIndexer_();
   };
}
