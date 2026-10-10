// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <atomic>
#include <functional>
#include <thread>

#include "../Util/Event.h"

namespace HM
{
 
   class SMTPDeliveryManager;
   class ExternalFetchManager;
   class BackupManager;
   class DatabaseConnectionManager;
   class Scheduler;
   class ScheduledTask;
   class WorkQueue;
   class IOService;
   class ServerMessages;
   class NotificationServer;
   class FolderManager;
   class MetricsServer;
   class SnmpAgent;
   class RestApiServer;
   class WebServicesServer;
   class ManageSieveServer;
   class MessageIndexer;

   class Application : public Singleton<Application>
   {
   public:

      Application();
      virtual ~Application();

      static String GetExecutableName();

      // --- global
      // The two are replaced by every reinitialise - ExitInstance lets them go,
      // InitInstance makes new ones - while COM, the SQL log device's worker and the
      // other threads a reinitialise does not stop go on asking for them, so the
      // pointer is copied, assigned and reset under managers_mutex_, as
      // Configuration's are under its publication_mutex_: a std::shared_ptr copied
      // on one thread while it is assigned on another is a data race that can leave
      // the copy holding a freed control block. Empty between the two, as it always
      // was. The callers here that may run then check for that - COM's Database
      // and BackupManager objects, the SQL log device's worker - but most of the
      // server's do not (Collection::DBLoad_, which a COM list's Refresh reaches,
      // among them): a COM call that reaches one in that moment meets a null
      // pointer, which the call's catch-all, under /EHa, answers as an unknown
      // error, as it always has.
      std::shared_ptr<DatabaseConnectionManager> GetDBManager() const;

      // --- overridables
      virtual bool InitInstance(String &sErrorMessage);
      virtual bool ExitInstance();

      String GetVersionNumber() const;
      String GetVersionArchitecture() const;
      

      String GetStartTime() const {return start_time_; }

      bool OpenDatabase(String &sErrorMessage);
      void CloseDatabase();

      
      String GetLastErrorMessage() {return last_connect_error_message_; }; 

      // True from the end of a successful InitInstance until ExitInstance: the
      // database open, its schema the one this build requires, Configuration
      // loaded. False while a database this server refused stays open for
      // hMailServer.Database - the state DBUpdater works in - when nothing
      // that reads the configuration exists yet and a COM call into it must be
      // refused rather than made.
      bool IsInitialized() const { return initialized_; }

      // The next InitInstance expects a database older than this build, because
      // DBUpdater is about to move it through this process: the start the
      // installer makes for the database step of an upgrade (ServiceMain,
      // --for-database-upgrade), and the restart that follows COM's
      // SetDefaultDatabase, which names a database that exists and may be older
      // - DBSetup, and its /unattended mode, then upgrade it with DBUpdater. That
      // InitInstance spends it, whatever it finds, and when its
      // OnDatabaseConnected finds the database older it refuses the start as
      // before without reporting HM5011. Once, so a later reinitialise or start
      // reports the refusal again. A database newer than this build is reported
      // whatever this says.
      void SetStartedForDatabaseUpgrade() { started_for_database_upgrade_ = true; }

      // The servers started, stopped, and stopped before the process ends. Each
      // of these and Reinitialize holds servers_mutex_ throughout, so that one
      // runs at a time: until 10 October 2026 nothing held them apart, and a
      // service stop or a SIGTERM during a reinitialise found the servers
      // stopping, went straight on, and closed the database under the
      // reinitialise and the message indexer's thread it was waiting for -
      // which faulted the Linux server on its way out. Made on the indexer's
      // own thread, by an OnError script it runs, each is refused when another
      // holds the mutex (MessageIndexer::LockServersUnlessOnItsThread); made on
      // the thread that holds it, by a script the stop or the start runs, each
      // is refused; and after ShutDown has begun every one is, at once.
      //
      // StartServers: COM's Application.Start, the service's start, the Linux
      // server's, a /Test run. False when the start failed or was refused, the
      // reason in the log, and a refusal's in refused when it is given. One
      // under way is waited for until it has ended, or for maxWaitSeconds when
      // that is above 0, and the start then refused.
      bool StartServers(int maxWaitSeconds = 0, String *refused = nullptr);

      // The service's start, and a /Debug run's (InitializeApplication):
      // InitInstance, then finish when it is given - what the start does with
      // the database open before the servers start, true when it changed the
      // server, which is then reinitialised - and then the servers started, all
      // with servers_mutex_ held. The service reports itself running, and
      // accepts a stop, before this begins, so that a stop arriving meanwhile -
      // the installer's, straight after the start it makes for the database
      // step - waits for it (ShutDown), where until 10 October 2026 it closed
      // the database under InitInstance or the new installation's defaults.
      // False when InitInstance failed, the start was refused or failed, or
      // that stop came first, why in sErrorMessage when there is a reason.
      bool InitInstanceAndStart(String &sErrorMessage, const std::function<bool()> &finish);

      // COM's Application.Stop: the servers stopped, to be started again in
      // this process (StartServers, Reinitialize) on the database it leaves
      // open, the indexer waited for until it has ended. Answers empty, or why
      // the stop was refused. maxWaitSeconds as StartServers'.
      String StopServers(int maxWaitSeconds = 0);

      // How long COM's Application.Stop and Start wait for a stop, a start or
      // a reinitialise under way before they are refused. Until 10 October
      // 2026 a Stop during a reinitialise did nothing and answered that it had
      // stopped the servers, and a Start was refused; holding the mutex, both
      // waited for the reinitialise, whose stop waits for the message indexer
      // without a limit - a Control Panel's Stop for as long as the indexer's
      // statement, and a script's on one of a work queue's threads past the
      // ten seconds that queue's stop gives the thread (WorkQueue::Stop),
      // after which it stopped the servers the reinitialise had started again.
      // Five seconds, below those ten.
      static const int ComServersWaitSeconds = 5;

      // The stop before the process ends - the service's stop and Windows'
      // shutdown (TerminateServer), a signal's on Linux, the end of a /Debug or
      // a /Test run: the servers stopped, the indexer waited for a minute at
      // most, the database closed (ExitInstance). A stop, a start or a
      // reinitialise under way is waited for - the service's own start among
      // them (InitInstanceAndStart) - and one that comes after does nothing. A reinitialise's stop waits for the indexer without a limit,
      // so this waits for that thread itself, the same minute at most, and the
      // reinitialise then stops short, leaving the rest to this.
      //
      // Answers false when a thread was left running - the indexer's, still in
      // a statement; one of a work queue's that the queue's stop gave up on
      // after its ten seconds and five more (WorkQueue::Stop), in this stop or
      // an earlier one; or the stop or start this waited for - in which case
      // the database is left open, since that thread may be using it: the
      // caller then ends the process at once, without the destructors of
      // statics, which the thread may be using too (TerminateProcess, _Exit).
      // Until 10 October 2026 the database was closed under such a thread, and
      // the process ended through the destructors.
      bool ShutDown();

      void SubmitPendingEmail();

      // Maintenance mode - a drain without a stop - and what a stop does first.
      //
      // EnterDrain closes the SMTP, IMAP and POP3 listeners and pauses the
      // delivery queue, moves the state to StateDraining (so /readyz answers 503
      // with "draining") and leaves everything else up: the sessions and
      // deliveries in flight finish at their own pace, the metrics, REST and
      // web-services listeners keep answering, and the process stays up. It is
      // what POST /api/v1/server/drain does, what the live update does before
      // it starts the helper that will stop the service, and what StopServers
      // does first when the state is Running. LeaveDrain reopens the listeners
      // on the same ports (a port that cannot be reopened is named in
      // notReopened, reported as HM7150, and the rest are opened), resumes the
      // queue and moves the state back to Running. Both answer empty on
      // success and otherwise why not, in the words the route returns.
      struct DrainStatus
      {
         DrainStatus() : draining(false), sessions(0), deliveries(0), drained(false) {}

         bool draining;
         String since;
         int sessions;
         int deliveries;
         bool drained;
      };

      String EnterDrain();
      String LeaveDrain(std::vector<String> &notReopened);
      bool IsDraining() const;
      DrainStatus GetDrainStatus();

      // LeaveDrain on the server's own behalf, with the reason logged and the
      // "left" recorded under the actor "server": what the live update's watcher
      // does when the helper it started finished without stopping the service,
      // or never stopped it. Answers what LeaveDrain answers.
      String LeaveDrainAsTheServer(const String &reason);

      // Called from the stop's drain wait about once a second while it waits,
      // so that the service host can tell the service controller the stop is
      // still making progress (a checkpoint) rather than letting its wait hint
      // run out. Empty by default; hMailServer.cpp sets it before a stop.
      void SetStopProgressCallback(std::function<void()> callback);

      std::shared_ptr<SMTPDeliveryManager> GetSMTPDeliveryManager() {return smtp_delivery_manager_;} 
      std::shared_ptr<ExternalFetchManager> GetExternalFetchManager() {return external_fetch_manager_;} 

      // Under managers_mutex_ (GetDBManager says why).
      std::shared_ptr<BackupManager> GetBackupManager() const;

      std::shared_ptr<WorkQueue> GetMaintenanceWorkQueue();
      std::shared_ptr<WorkQueue> GetAsyncWorkQueue();

      // Queue for blocking name lookups performed on behalf of a live session
      // (currently the SMTP reverse-DNS prefetch). Kept separate from the
      // asynchronous task queue so a burst of slow or unanswered DNS queries
      // can never delay message finalization - the work that sends the final
      // "250 OK" - or POP3 external fetching.
      std::shared_ptr<WorkQueue> GetNameLookupWorkQueue();
      std::shared_ptr<IOService> GetIOService() {return io_service_; }
      // The random work queue can run any task.

      std::shared_ptr<NotificationServer> GetNotificationServer();
      std::shared_ptr<FolderManager> GetFolderManager();

      String Reinitialize();

      int GetUniqueID();

      void OnPropertyChanged(std::shared_ptr<Property> pProperty);

      // databaseUpgradeExpected: the start is the one SetStartedForDatabaseUpgrade
      // marked, and a database older than this build is refused, not reported.
      bool OnDatabaseConnected(String &sErrorMessage, bool databaseUpgradeExpected = false);

      boost::signals2::signal<void()> OnServerStopped;

   private:

      friend class ServersLockTester;

      // What a stop of the servers is for, which decides how long it waits for
      // the message indexer's thread (MessageIndexer::Stop). StopToStartAgain:
      // the servers may be started again in this process - after a Reinitialize,
      // which closes the database as soon as the stop returns and opens it
      // again, or after COM's Application.Stop, which leaves it open - so the
      // indexer is waited for until it has ended, however long its statement
      // takes. StopBeforeExit: the process ends after this stop (ShutDown), and
      // the indexer is waited for a minute at most, so that a database that has
      // stopped answering does not hold the stop for ever while the service
      // controller is told that it is progressing.
      enum StopPurpose
      {
         StopToStartAgain,
         StopBeforeExit
      };

      // The stop and the start themselves, servers_mutex_ held. StopServers_
      // answers whether the indexer's thread has ended: false when a stop
      // before exit gave up on it, or the stop was made on that thread, which
      // cannot wait for itself.
      bool StopServers_(StopPurpose purpose);
      bool StartServers_();

      // Reinitialize itself, servers_mutex_ held.
      String Reinitialize_();

      // servers_mutex_ taken by lock for a stop, a start or a reinitialise:
      // answers empty, or why it was not taken - on the thread that holds it
      // already, by a script the stop or the start it is making runs; on the
      // indexer's own thread while another holds it; or after ShutDown has
      // begun. The caller declares a ServersLockHeld_ straight after lock.
      // indexer is the server's (MessageIndexer::Instance), or a tester's
      // stand-in (ServersLockTester). A stop, a start or a reinitialise under
      // way is waited for until it has ended, or for maxWaitSeconds when that
      // is above 0 (StartServers); after ShutDown has begun nothing is.
      String TakeServersLock_(boost::unique_lock<boost::timed_mutex> &lock, MessageIndexer &indexer, int maxWaitSeconds = 0);

      // Says which thread holds servers_mutex_ from when it is taken
      // (TakeServersLock_, ShutDown) until this goes, before the lock does.
      class ServersLockHeld_
      {
      public:
         ServersLockHeld_(Application &application, boost::unique_lock<boost::timed_mutex> &lock);
         ~ServersLockHeld_();

         // Taken now, by ShutDown's own try.
         void Taken();

         ServersLockHeld_(const ServersLockHeld_ &) = delete;
         ServersLockHeld_ &operator=(const ServersLockHeld_ &) = delete;

      private:
         Application &application_;
         boost::unique_lock<boost::timed_mutex> &lock_;
      };

      // ExitInstance. Before the process ends (ShutDown) the database is closed
      // only when no thread the stop gave up on may be using it - the message
      // indexer's (indexerEnded false), or a work queue's
      // (WorkQueue::GetQueuesWithThreadsLeft), the queues this removes among
      // them - and this answers whether it was. A reinitialise's closes it.
      bool ExitInstance_(StopPurpose purpose, bool indexerEnded);

      // The stop's progress callback, copied under its lock: TerminateServer
      // sets it on the service controller's thread while a stop on another -
      // a reinitialise's, which ShutDown waits for - may be calling it.
      std::function<void()> StopProgress_() const;

      void RegisterSessionTypes_();
      void CreateScheduledTasks_();

      // The backup schedule's task, registered by a start when a schedule is
      // configured and by a save that configures one on a running server.
      void ScheduleBackups_(std::shared_ptr<Scheduler> scheduler);

      // The subsystems that re-latch a [Settings] value when a save changes it
      // (IniFileSettings::Subscribe). Once per process; see the definition.
      void SubscribeToSettings_();
      bool settings_subscribed_;

      // Running or draining: the states in which the servers StartServers built
      // exist and a subscriber may reach them.
      static bool IsServing_();

      // HM6119: the database fault injector is on. Said by every start and
      // reinitialise that finds it set, and by the save that sets it.
      static void AnnounceDatabaseFaultInjector_(const String &pattern);

      // The two queues whose tasks hold a live TCPConnection, and therefore a
      // socket that belongs to io_service_'s io_context. Created and removed with
      // the servers rather than with the process, so that their lifetime nests
      // inside the io_context's instead of straddling it - see the definitions.
      void CreateSessionWorkQueues_();
      void RemoveSessionWorkQueues_();

      // The first half of a drain and of a stop: the SMTP, IMAP, POP3 and
      // ManageSieve listeners closed and the delivery queue paused. Idempotent;
      // both halves are. False when a listener is not known to be closed (the
      // io threads did not answer), which the caller says rather than logging
      // "listeners closed" over a door that may be open.
      bool CloseTheDoors_();

      // The "left" event under an actor named server, of kind system: what a
      // stop from Draining and LeaveDrainAsTheServer both record, so that the
      // operator's "entered" is never left without its other half by something
      // that had no request thread to carry an actor.
      void RecordDrainLeftByTheServer_(const String &detail);

      std::function<void()> stop_progress_;
      mutable boost::mutex stop_progress_mutex_;

      // Set when ShutDown begins, and never cleared: the process is ending.
      std::atomic<bool> shutting_down_{false};

      // Serialises the three state transitions that close or reopen the doors -
      // EnterDrain, LeaveDrain and the head of StopServers - so that a stop and
      // a drain arriving together cannot both read Running and both act on it.
      // Held only across the transition, never across the drain's wait.
      // Recursive, because GetDrainStatus reads drain_since_ under it and is
      // called from inside EnterDrain, which holds it.
      boost::recursive_mutex drain_mutex_;
      String drain_since_;

      String version_;
      String start_time_;
      
      String last_connect_error_message_;
      bool initialized_;

      // SetStartedForDatabaseUpgrade. Set before the InitInstance that spends it,
      // whether or not that one reaches the database: by ServiceMain, on the
      // thread that makes the first, or by SetDefaultDatabase on the thread that
      // serves that COM call, before the Reinitialize DBSetup asks for next, which
      // another thread may serve - so it is atomic.
      std::atomic<bool> started_for_database_upgrade_{false};

      std::shared_ptr<DatabaseConnectionManager> db_manager_;

      // Guards db_manager_ and backup_manager_, the pointers only: held to copy,
      // assign or reset one, never across a call into either - a leaf.
      mutable boost::mutex managers_mutex_;

      // the servers
      std::shared_ptr<SMTPDeliveryManager> smtp_delivery_manager_;

      std::shared_ptr<ExternalFetchManager> external_fetch_manager_;
      std::shared_ptr<BackupManager> backup_manager_;
      std::shared_ptr<Scheduler> scheduler_;

      // The two scheduled tasks a saved setting reaches in place: the greylist
      // cleaner, whose period is GreylistingRecordExpirationInterval, and the
      // backup schedule, registered only once a schedule is configured. Reset with
      // the scheduler.
      std::shared_ptr<ScheduledTask> greylist_cleaner_task_;
      std::shared_ptr<ScheduledTask> backup_schedule_task_;

      // Guards scheduler_ and the two handles above wherever a thread other than
      // the one starting or stopping the servers reaches them - the subscribers a
      // save calls (SubscribeToSettings_). A service stop resets them without the
      // reinitialise mutex those subscribers try, so without this a subscriber could
      // copy a handle the stop was resetting. Held only to copy or assign a pointer.
      boost::mutex scheduler_handles_mutex_;

      // Reinitialize() is reachable from a COM client and from the
      // Reinitializator thread an ACME deployment or a backup restore starts,
      // and two of them at once tear down and rebuild the same queues under
      // each other - a RunOnce task queued by one StartServers onto a
      // maintenance queue the other ExitInstance had just removed was an access
      // violation. They run one after the other, and since 10 October 2026 so
      // do COM's Start and Stop and the stop before exit (StartServers, above).
      // Timed, so that ShutDown can wait for it in turns of a second, telling
      // the service controller meanwhile.
      boost::timed_mutex servers_mutex_;

      // The thread holding servers_mutex_, none while it is free.
      std::atomic<std::thread::id> servers_mutex_holder_{std::thread::id()};
      std::shared_ptr<NotificationServer> notification_server_;
      std::shared_ptr<IOService> io_service_;
      std::shared_ptr<FolderManager> folder_manager_;
      std::shared_ptr<MetricsServer> metrics_server_;

      // The SNMP agent (Util/SnmpAgent.h): made and started with the servers
      // when SnmpEnabled is on, stopped with them, as the metrics listener is.
      std::shared_ptr<SnmpAgent> snmp_agent_;

      std::shared_ptr<RestApiServer> rest_api_server_;
      std::shared_ptr<WebServicesServer> web_services_server_;
      std::shared_ptr<ManageSieveServer> manage_sieve_server_;

      const String maintenance_queue_;
      // The random work queue can run any type of task.

      const String server_work_queue_;
      // The main server queue, that contains one task per server.

      const String asynchronous_tasks_queue_;

      const String name_lookup_queue_;

      // LONG, not long, because GetUniqueID hands its address to
      // InterlockedIncrement, whose parameter is LONG volatile *. On Windows
      // LONG is a typedef for long and this is the same declaration it has
      // always been; elsewhere long is 64 bits and the counter would be the one
      // width the interlocked call cannot take.
      LONG unique_id_;
   };

   // Application's servers_mutex_, which every stop, start and reinitialise of
   // the servers holds (StartServers, above), driven through Application itself.
   // A reinitialise, a stop or a start asked on the thread that holds the mutex
   // - by a script that the stop or the start it is making runs - is refused at
   // once, where it would wait for the mutex its own thread holds; one asked on
   // the message indexer's own thread while another holds the mutex - by an
   // OnError script the indexer runs - is refused at once, where the holder may
   // be waiting for that thread to end; once the stop before the process ends
   // (ShutDown) has begun every one is refused; that stop waits for one under
   // way; and it leaves the database open, and answers so, while a thread a
   // work queue's stop gave up on is still running. Until 10 October 2026 no
   // test drove the mutex: the indexer's test (MessageIndexerTester) took one
   // of its own.
   //
   // Test lets go of whatever it takes at once and starts and stops nothing,
   // the indexer it asks on being a stand-in with no database, so that it runs
   // wherever ClassTester runs - in the server, on Windows - and in
   // business-object-tests. TestTheStopBeforeExit makes the stop before the
   // process ends, sets the process's server state and holds the mutex for
   // seconds, and so runs in business-object-tests alone, which has no
   // servers, after every other test. It also holds COM's Application.Stop and
   // Start to the few seconds they wait for one under way, and the service's
   // start to taking the mutex before it opens the database.
   class ServersLockTester
   {
   public:
      void Test();
      void TestTheStopBeforeExit();

   private:
      void TestAskedOnTheThreadThatHoldsIt_();
      void TestAskedOnTheIndexersThreadWhileAStopWaits_();
      void TestRefusedOnceTheStopBeforeExitHasBegun_();
      void TestTheStopBeforeExitWaitsForOneUnderWay_();
      void TestComsStopAndStartWaitAMomentAtMost_();
      void TestTheServicesStartTakesTheLockFirst_();
      void TestAWorkQueuesThreadLeftKeepsTheDatabaseOpen_();
   };
}
