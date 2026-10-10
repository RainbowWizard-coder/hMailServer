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
      // StartServers starts the indexer when the setting is on, and
      // Configuration::SetMessageIndexing saves the setting before it calls this.
      void Start();

      // Asks the thread to stop and waits until it has ended, so that nothing
      // it does comes after what it uses is gone - the database manager, which
      // a Reinitialize resets straight after StopServers - and answers whether
      // it has. The thread ends at its next interruption point: before each
      // message of a batch, between two slices of the sweeps of the message
      // metadata's and the full-text terms' orphans, in the pause between two
      // backfill passes and in the minute's wait. That can be several statements
      // away - a pass reads what it is to index with several before its first
      // message - and a statement takes as long as the database takes to answer
      // it, or DatabaseStatementTimeout where the database applies that: not to
      // a write on MySQL, not at all when it is 0. The request is asked again at
      // every second of the wait, since one that an interruption point threw
      // inside a catch (...) is forgotten there.
      //
      // maxWaitSeconds 0 waits until the thread has ended, as a Reinitialize
      // must, the database closed straight after. Above 0 it gives up after so
      // long, says so in the log and answers false, the thread left to end by
      // itself: the stop before the process ends (Application::ShutDown),
      // so that a database that has stopped answering does not hold a service
      // stop for ever while the service controller is told it is progressing.
      // progress is called about once a second while it waits (the service
      // controller's checkpoint, as Sandbox::Stop calls it).
      //
      // Made on the indexer's own thread - by an OnError script it runs - it asks
      // the thread to stop at its next interruption point and answers false: the
      // thread cannot wait for itself. The lock is let go of while a stop waits,
      // so that such a script's Start or Stop, which return at once on that
      // thread, are never made to wait for the stop that waits for them.
      bool Stop(const std::function<void()> &progress = std::function<void()>(), int maxWaitSeconds = 0);

      // Takes lock's mutex - Application's servers_mutex_, held across every
      // stop, start and reinitialise of the servers - and answers whether it
      // has. On the indexer's own thread - an OnError script it runs calling
      // COM's Application.Reinitialize, Stop or Start - only when the mutex is
      // free at once: the stop that holds it may be waiting for this thread to
      // end, a reinitialise's without a limit, and this thread would wait for
      // it in turn, for ever, as until 10 October 2026 such a Reinitialize did.
      // Elsewhere it waits until the mutex is free, or for maxWaitSeconds when
      // that is above 0 (COM's Application.Stop and Start).
      bool LockServersUnlessOnItsThread(boost::unique_lock<boost::timed_mutex> &lock, int maxWaitSeconds = 0);

      void IndexNow();

   private:

      friend class MessageIndexerTester;
      friend class ServersLockTester;

      // An indexer whose thread runs work rather than the indexer's loop - a
      // stand-in with the loop's interruption points and none of its database,
      // for MessageIndexerTester.
      explicit MessageIndexer(const boost::function<void()> &work);

      void WorkerFunc();
      void WorkerFuncInternal();

      // A Start on the indexer's own thread after a Stop on it - an OnError
      // script turning MessageIndexing off and on again - takes the Stop back,
      // the thread going on as the indexer. Under starterMutex_, with the
      // caller's interruptions turned off by noInterruption.
      void TakeBackStopOnItsThread_(boost::this_thread::disable_interruption &noInterruption);

      // Waits, starterMutex_ held by lock on the way in and out but let go of
      // while it waits, until the thread a Stop has interrupted has ended (true),
      // or for maxWaitSeconds when that is above 0 (false), calling progress
      // about once a second. One caller at a time joins the thread, in turns of a
      // second with joining_ set; another waits for the turn to be over.
      bool WaitForEnd_(boost::unique_lock<boost::mutex> &lock, const std::function<void()> &progress, int maxWaitSeconds);

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

      // The server's indexer, whose thread uses the database, which is not
      // started while the servers are stopped (Start); false for a tester's
      // stand-in, which uses none.
      const bool uses_the_database_;

      // starterMutex_ guards what follows to turn_over_. It is never held while
      // the thread is waited for: from 6ebf39d39 Stop joined under it, and an
      // OnError script the indexer ran then, turning MessageIndexing on or off,
      // waited for the lock on the indexer's thread while the stop waited for
      // that thread - for ever.
      boost::mutex starterMutex_;

      boost::thread workerThread_;

      // workerThread_'s id from its start until it is joined; none otherwise.
      // What the checks for the indexer's own thread ask, since a caller may be
      // joining workerThread_ outside the lock (joining_), and nothing else
      // touches workerThread_ then.
      boost::thread::id worker_id_;

      // Counts the threads Start has begun, so that a waiter for one knows it
      // has ended once a Start has begun the next.
      unsigned long generation_;

      // The thread has been interrupted by a Stop and not yet joined: by a Stop
      // made on the thread itself, which cannot wait for itself; by one that
      // gave up waiting; or by one waiting now. A Start waits for it to end - or,
      // made on that thread while no one else waits for it, takes the stop back
      // - rather than taking it for the running indexer. Set whenever joining_
      // is.
      bool stop_asked_;

      // The callers in WaitForEnd_ for the thread of this generation.
      int waiters_;

      // A caller is joining workerThread_ with starterMutex_ let go of
      // (WaitForEnd_), and turn_over_ is notified when its turn is over.
      bool joining_;
      boost::condition_variable turn_over_;

      // What workerThread_ runs: WorkerFuncInternal, or a tester's stand-in.
      boost::function<void()> work_;

      int iIndexRunCount;

      // Counts loop iterations so the orphaned-terms sweep - a scan of the
      // whole terms table - runs on the full-index cadence rather than every
      // minute.
      int full_text_iteration_count_;

      // True while a reconciliation that found a full batch is still working
      // through its backlog, so it continues at backfill speed instead of one
      // batch per hourly gate.
      bool full_text_reconcile_active_;

      Event index_now_;
   };

   // A stand-in pass in the indexer's thread, with no database: Stop waits for
   // the pass in flight before it returns, and a Start after a Stop starts a new
   // thread - where until 10 October 2026 Stop interrupted and returned, the
   // pass went on to the database after a Reinitialize had closed it, which
   // ended the Linux server in the Alpine suite, and the Start after it took the
   // stopping thread for the running indexer and started none. And a Start made
   // on the indexer's own thread, as an OnError script would make it, neither
   // asks the thread to join itself - boost throws, as it did until 10 October
   // 2026 - nor ends the indexer when it follows a Stop there. A Stop with a
   // limit gives up on a pass that outlasts it and answers false, and the Start
   // after it waits for that pass to end and begins a new indexer. And a Stop
   // and a Start the indexer's own thread makes while a Stop on another waits
   // for it return at once, the Start taking back no stop, where the wait held
   // the lock they need and the stop and the indexer waited for each other for
   // ever. And a Stop ends a pass that swallowed its request to stop in a
   // catch (...), where until 10 October 2026 the request was asked once and a
   // Reinitialize's stop waited for ever; and the servers' mutex, held by a
   // Stop that waits for the indexer, is not waited for on the indexer's own
   // thread, where an OnError script's Reinitialize waited for it for ever
   // while a reinitialise held it. ClassTester runs
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

      // A stand-in indexer starts while the servers are stopped, as Test's
      // asks of it wherever ClassTester runs. It sets the process's server
      // state, and so runs in business-object-tests alone, which has no
      // servers: inside a server it would stop that server's indexer starting.
      void TestAStandInStartsWhileTheServersAreStopped();

   private:
      void TestStopWaitsForThePassInFlight_();
      void TestAStartAfterAStopIsANewIndexer_();
      void TestAStartOnItsOwnThread_();
      void TestAStopBeforeExitGivesUp_();
      void TestItsOwnStartAndStopWhileAStopWaits_();
      void TestASwallowedStopIsAskedAgain_();
      void TestItsOwnReinitialiseWhileAStopWaits_();
   };
}
