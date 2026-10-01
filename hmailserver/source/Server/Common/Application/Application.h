// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <functional>

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

      // The start the installer makes for the database step of an upgrade
      // (ServiceMain, --for-database-upgrade): the database is expected to be
      // older than this build, because DBUpdater is about to move it through
      // this process. The next InitInstance spends it, whatever it finds, and
      // when its OnDatabaseConnected finds the database older it refuses the
      // start as before without reporting HM5011. Once, so a reinitialise or
      // any later start reports the refusal again.
      void SetStartedForDatabaseUpgrade() { started_for_database_upgrade_ = true; }

      bool StartServers();
      void StopServers();
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
      void SetStopProgressCallback(std::function<void()> callback) { stop_progress_ = callback; }

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

      // SetStartedForDatabaseUpgrade. Set before the first InitInstance, on the
      // thread that makes it, and spent by that InitInstance whether or not it
      // reaches the database; no other thread reaches it before then.
      bool started_for_database_upgrade_ = false;

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
      // violation. They run one after the other now.
      boost::mutex reinitialize_mutex_;
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
}
