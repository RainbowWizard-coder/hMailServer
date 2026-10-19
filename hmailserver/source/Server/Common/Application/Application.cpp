// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"

#include "Application.h"

#include "BackupManager.h"
#include "Scheduler.h"
#include "OutOfMemoryHandler.h"
#include "ACLManager.h"
// Nothing below names a Win32 status code, so the POSIX build does without the
// Windows error header.
#ifdef _MSC_VER
#include <winerror.h>
#endif

#include "MessageIndexer.h"

#include "FolderManager.h"
#include "../Cache/CacheContainer.h"

#include "../AntiSpam/SpamProtection.h"
#include "../AntiVirus/VirusScanner.h"

#include "../SQL/DALConnection.h"
#include "../SQL/DALConnectionFactory.h"
#include "../SQL/DatabaseConnectionManager.h"
#include "../SQL/DatabaseTls.h"
#include "../Scripting/ScriptServer.h"

#include "../Util/Time.h"
#include "../Util/ServerStatus.h"
#include "../Util/Languages.h"
#include "../Util/Utilities.h"
#include "../Util/MetricsServer.h"
#include "../Util/MailboxKeys.h"
#include "../Util/SnmpAgent.h"
#include "../Util/RateLimiter.h"
#include "../Util/JwksKeySet.h"
#include "../Util/Totp.h"
#include "../Util/DiscoverySearch.h"
#include "../Util/OtelTracer.h"
#include "../Util/OtelMetricsExporter.h"
#include "../Util/OtelLogExporter.h"
#include "../Util/SyslogSink.h"
#include "../Util/NeverBanNameList.h"
#include "../Mime/MimeCode.h"

#include "Property.h"
#include "Version.h"

#include "../../SMTP/SMTPDeliveryManager.h"
#include "../../SMTP/IpWarmup.h"
#include "../../IMAP/IMAPConfiguration.h"
#include "../../IMAP/IMAPCommandAppend.h"
#include "../../ExternalFetcher/ExternalFetchManager.h"

#include "../Threading/WorkQueueManager.h"
#include "../Threading/WorkQueue.h"
#include "../Threading/Task.h"
#include "../Tracking/NotificationServer.h"
#include "SessionManager.h"
#include "EnvironmentSettings.h"

#include "../../SMTP/GreyListCleanerTask.h"
#include "../../SMTP/TlsRptReporterTask.h"
#include "../../SMTP/DmarcRptReporterTask.h"
#include "../Util/AcmeClient.h"
#include "../Util/FileUtilities.h"
#include "../Util/RestApiServer.h"
#include "../Sieve/ManageSieveServer.h"
#include "../Util/WebServicesServer.h"
#include "../TCPIP/IOService.h"
#include "../TCPIP/TCPServer.h"

#include "../BO/Message.h"
#include "../BO/Domain.h"
#include "../Persistence/MailboxMove.h"
#include "MigrationManager.h"
#include "../Persistence/PersistentMessage.h"
#include "../Persistence/PersistentDomain.h"
#include "../Persistence/PersistentAccount.h"
#include "../Persistence/PreSaveLimitationsCheck.h"
#include "../Persistence/SchemaUpgrader.h"
#include "../Util/AuditTrail.h"
#include "RemoveExpiredRecords.h"
#include "ScheduledMailTask.h"
#include "../Util/AutoBanFirewall.h"
#include "../Util/StoredSecrets.h"
#include "IniSettingStore.h"
#include "../Util/CompromiseDetector.h"
#include "../AntiVirus/Sandbox.h"
#include "LogRetentionTask.h"
#include "ArchiveRetentionTask.h"
#include "MailboxRetentionTask.h"
#include "MetricsHistoryTask.h"
#include "QuarantineDigestTask.h"
#include "ListDigestTask.h"
#include "UpdateCheckTask.h"
#include "../Util/UpdateApplyToken.h"
#include "../Util/UpdateChecker.h"
#include "IMAPExpungeRetentionTask.h"
#include "MessageStoreConsistencyTask.h"
#include "../Persistence/MessageStoreObjects.h"
#include "WorkQueueHealthTask.h"
#include "BounceMaintenanceTask.h"
#include "ComplaintMaintenanceTask.h"
#include "../Util/DiskSpace.h"
#include "../Util/AlertManager.h"
#include "../Util/PushNotifier.h"
#include "../Util/InboundWebhooks.h"
#include "../Util/ResourceBooking.h"
#include "../Util/WebhookManager.h"
#include "DkimRotationTask.h"
#include "ReputationWatchTask.h"
#include "BackupScheduleTask.h"
#include "../LDAP/DirectorySyncTask.h"

#include <chrono>
#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif



namespace HM
{
   namespace
   {
      // How long a stop before the process ends waits for the message indexer's
      // thread (StopServers_, ShutDown).
      const int IndexerWaitBeforeExitSeconds = 60;
   }

   Application::Application() :
      server_work_queue_("Server queue"),
      maintenance_queue_("Maintenance queue"),
      asynchronous_tasks_queue_("Asynchronous task queue"),
      name_lookup_queue_("Name lookup queue"),
      unique_id_(0),
      initialized_(false),
      settings_subscribed_(false)
   {
      version_ = Formatter::Format("{0}-B{1}", HMAILSERVER_VERSION, HMAILSERVER_BUILD);
      start_time_ = Time::GetCurrentDateTime();
   }

   Application::~Application()
   {

   }

   String
   Application::GetVersionNumber() const
   {
      return version_;
   }

   String
   Application::GetVersionArchitecture() const
   {
#if defined(_M_ARM64)
      return "ARM64";
#elif _WIN64
      return "x64";
#else
      return "x86";
#endif 
   }

   bool
   Application::InitInstance(String &sErrorMessage)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Application initializing stuff.
   //---------------------------------------------------------------------------()
   {
      initialized_ = false;

      // The installer's start for the database step of an upgrade, or the
      // restart after SetDefaultDatabase, is this start only
      // (SetStartedForDatabaseUpgrade): spent here, whether or not the database
      // is reached, so that no later one inherits it.
      const bool databaseUpgradeExpected = started_for_database_upgrade_.exchange(false);

      VirusScanner::ResetCounter();

      OutOfMemoryHandler::Initialize();

      MimeEnvironment::SetAutoFolding(true);

      ServerStatus::Instance()->SetState(ServerStatus::StateStopped);

      // Initialize objects that should always exist, regardless of anything.
      //
      // ScopeReinitialize, because this is the one place the servers are built from
      // the settings: every key marked now or reinitialize is taken from the store,
      // and a restart key keeps what this process started with (docs/SettingsApply.md).
      IniFileSettings::Instance()->LoadSettings(IniFileSettings::ScopeReinitialize);

      if (!IniFileSettings::Instance()->GetDatabaseSettingsExists())
      {
         return false;
      }

      // Start a random thread queue that can run different 
      // types of background tasks, such as backups etc.
      WorkQueueManager::Instance()->CreateWorkQueue(5, maintenance_queue_);

      // The rooms' and equipment's decisions (Util/ResourceBooking.h): a queue
      // of their own, so that a backlog for a room never holds a thread the
      // SMTP finalisation or a backup waits for.
      WorkQueueManager::Instance()->CreateWorkQueue(ResourceBooking::DrainThreads, ResourceBooking::WorkQueueName());

      // Language needed for COM API
      Languages::Instance()->Load();

      // Create the backup manager that manages backup tasks...
      {
         std::shared_ptr<BackupManager> backupManager(new BackupManager);
         boost::lock_guard<boost::mutex> guard(managers_mutex_);
         backup_manager_.swap(backupManager);
      }

      LOG_DEBUG("Application::InitInstance - Connecting to database...");
      if (!OpenDatabase(sErrorMessage))
      {
         // We could not open the database. We can just
         // as well stop now since there's nothing to do.
         return false;
      }
      
      if (!OnDatabaseConnected(sErrorMessage, databaseUpgradeExpected))
      {
         return false;
      }

      /*
         The second settings load, and it has to be exactly here.

         AFTER OnDatabaseConnected, because that is what proves the schema is the
         version this build requires. hm_inisettings only exists from 6011, so
         reading it on an older database would raise a SQL error in place of the
         "run DBUpdater" message that tells the operator what to actually do.

         BEFORE Configuration::Load below, and therefore before every server object
         built afterwards, so that nothing latches a value from the file that the
         database was going to change.

         The first load at the top of this function stays file-only and is what the
         bootstrap runs on: it is how the server finds the database at all.
      */
      // The installation's stored-secrets key (Util/StoredSecrets.h), before the
      // settings store is reconciled, which seals and opens the [Settings] secrets
      // with it: judged against the key the database records. Never made here - the
      // service may have no write on hMailServer.ini, and a key made by whichever
      // node starts first is how a database came to be sealed under two - and
      // nothing is written into the file. The switch is read as the store holds it,
      // not as this machine's file does.
      StoredSecrets::EnsureAtStart(IniSettingStore::IsProtectionOnAsStored());

      IniFileSettings::Instance()->LoadDatabaseSettings();
      IniFileSettings::Instance()->LoadSettings(IniFileSettings::ScopeReinitialize);

      // What the servers are about to be built from is now what they run on: from
      // here a save applies only the keys marked now, and the rest are pending until
      // a reinitialise or a restart takes them.
      IniFileSettings::Instance()->LatchRunningValues();
      SubscribeToSettings_();

      LOG_DEBUG("Application::InitInstance - Loading configuration...");
      if (!Configuration::Instance()->Load())
         return false;

      LOG_DEBUG("Application::InitInstance - Configuration loaded.");

      // What the key's judgement and the settings reconcile had to say, now that
      // the logger is configured: before Configuration::Load its mask is 0 and an
      // application-log line is dropped, whatever logging is set to.
      StoredSecrets::LogNotes();

      // And what a schema upgrade had to say before the logger was configured:
      // UpgradeOnStart's steps above, or - at the reinitialise DBUpdater asks
      // for once its upgrade is committed - the counts of a prerequisite it ran
      // over COM (SchemaUpgrader::Note).
      SchemaUpgrader::LogNotes();

      // Which settings the environment sets, said where an operator reads the
      // start: no interface will change them (docs/EnvironmentConfiguration.md).
      EnvironmentSettings::AnnounceManaged();

      // How the database connection is protected, said now that the log's settings
      // are read - the connections were made before they were (docs/DatabaseTls.md).
      DatabaseTls::AnnounceConnection();

      // The firewall is not made to match the auto-ban ranges here: StartServers
      // does that, before its first listener. Every command that starts the
      // server short of its listeners runs this function - --sample-domain,
      // --apply-environment (which the container runs at every start),
      // --apply-profile and the rest - often beside a running server, and the
      // start's reconciliation resets the packaged hook and tells AutoBanCommand
      // "unban" from its ledger (AutoBanFirewall::SynchroniseAtStart). Run from
      // such a command it would lift every ban below the running server until it
      // had told them all again - and, if it stopped in between, for the rest of
      // each of those bans or until that server's next start, since the running
      // server goes on believing the hook holds them and never tells them again.
      // Until 6.3.6 a pass ran here that only told every ban in force again.

      // The accounts the compromised-account detection locked before this
      // start are held again before a listener is open: the lock is a row in
      // hm_accountreview, and the hold on the name is in memory.
      CompromiseDetector::LoadLocks();

      // The accounts' second factors still stored as generated - every one enrolled
      // before 25 September 2026 - are sealed under the stored-secrets key, or with
      // ProtectStoredSecretsWithDPAPI off the sealed ones are opened back, before a
      // listener is open. Once done, its select finds no row (docs/SecretsAtRest.md).
      PersistentAccount::ProtectStoredTotpSecrets();

      // The asynchronous task and name lookup queues are deliberately NOT created
      // here. Their tasks hold live client connections, so they belong to the
      // servers, not to the process - see CreateSessionWorkQueues_.

      initialized_ = true;
      return true;
   }

   bool
   Application::OpenDatabase(String &sErrorMessage)
   {
      String sIniFileCheckError;
      if (!IniFileSettings::Instance()->CheckSettings(sIniFileCheckError))
      {
         String sError;
         sError.Format(_T("Loading of ini file settings failed. Error: %s"), sIniFileCheckError.c_str());

         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5005, "Application::OpenDatabase", sError);
         return false;
      }

      // The database fault injector is a test facility, and the way a facility like
      // this hurts somebody is not by being switched on - it is by being left on. So
      // it announces itself, every time the server starts or reinitialises, for as
      // long as it is set.
      //
      // High severity and the error log rather than the application log, deliberately:
      // this makes writes fail on purpose, and an operator who finds mail behaving
      // strangely should meet this line before they start looking anywhere else.
      // SimulateSpoolWriteFailure and CrashSimulationMode do not do this and arguably
      // should; the difference is that they affect one path each, while this can fail
      // any statement in the server.
      const String simulateDatabaseFailureFor = IniFileSettings::Instance()->GetSimulateDatabaseFailureFor();

      if (!simulateDatabaseFailureFor.IsEmpty())
         AnnounceDatabaseFaultInjector_(simulateDatabaseFailureFor);

      // Published before its connections are made, as it always was: a caller finds
      // it not yet connected (GetIsConnected) rather than missing. A manager this
      // replaces - none, after CloseDatabase - is let go of after the lock, since
      // its destructor may report, and a report may ask for the manager.
      std::shared_ptr<DatabaseConnectionManager> manager(new DatabaseConnectionManager);
      std::shared_ptr<DatabaseConnectionManager> replaced = manager;
      {
         boost::lock_guard<boost::mutex> guard(managers_mutex_);
         db_manager_.swap(replaced);
      }
      replaced.reset();
      bool bConnectedSuccessfully = manager->CreateConnections(sErrorMessage);

      last_connect_error_message_ = sErrorMessage;

      if (!bConnectedSuccessfully)
      {
         // The stop before the process ends gave up waiting for this start or
         // reinitialise and closed the database while its connections were
         // made (ShutDown, CloseDatabaseBeforeExit_): not reported as a
         // database that could not be reached, in the moment before the
         // process ends. The line is written only once logging is configured:
         // in a reinitialise, when application logging is on. At the
         // service's start (InitInstanceAndStart) the logger's mask is 0 until
         // Configuration::Load, so nothing is written, where an HM4354 was
         // reported before, and the process ends at once (TerminateServer).
         if (manager->GetIsClosingBeforeExit())
         {
            LOG_APPLICATION("The database was not opened, the process ending: " + sErrorMessage);
            return false;
         }

         ErrorManager::Instance()->ReportError(ErrorManager::Critical, 4354, "Application::OpenDatabase", "hMailServer failed to connect to the database server. Error message: " + sErrorMessage);
         return false;
      }

      return true;
   }


   bool
   Application::OnDatabaseConnected(String &sErrorMessage, bool databaseUpgradeExpected)
   {
      LOG_DEBUG("Application::OnDatabaseConnected()");

      std::shared_ptr<DatabaseConnectionManager> manager = GetDBManager();
      int iDBVersion = manager->GetCurrentDatabaseVersion();

      if (iDBVersion == 0)
      {
         sErrorMessage = "Database version could not be detected.";
         last_connect_error_message_ = sErrorMessage;

         // Closed before the process ends since OpenDatabase connected
         // (ShutDown): the statement found no connection, which is not a
         // database whose version is not there. Written as OpenDatabase's line
         // is: in a reinitialise, when application logging is on, and at the
         // service's start, before Configuration::Load, not at all, where an
         // HM5010 was reported before.
         if (manager->GetIsClosingBeforeExit())
         {
            LOG_APPLICATION("The database's version was not read, the process ending: the database was closed for the end of the process.");
            return false;
         }

         ErrorManager::Instance()->ReportError(ErrorManager::Critical, 5010, "Application::OnDatabaseConnected", sErrorMessage);
         return false;
      }

      // The schema step in the service's own start, when told to. [Database]
      // UpgradeOnStart=1 is a file key by necessity - it has to be read before
      // the database is at a version this build can open, which is before the
      // settings store exists - and the one such key this row adds. With it
      // set, an older database is brought forward here through the same code
      // `hmailserver --upgrade-database` runs, one step at a time with one log
      // line per step, and the start continues; without it, or when a step
      // fails, the refusal below stands, naming the step. Nothing is backed up
      // first: Upgrading.md says to, and this is where a restore would start.
      const int requiredVersion = Configuration::Instance()->GetRequiredDBVersion();
      if (iDBVersion < requiredVersion && IniFileSettings::Instance()->GetDatabaseUpgradeOnStart())
      {
         // These lines are written before Configuration::Load has set the
         // logger's mask, which drops every application-log line until then:
         // SchemaUpgrader::Note keeps them, and InitInstance writes them once
         // the settings are loaded (the upgrade's own counts among them -
         // PreReqGreyListKeys says how many greylisting rows 6107 keyed).
         SchemaUpgrader::Note(Formatter::Format("Schema: the database is at version {0} and this build needs {1}; [Database] UpgradeOnStart is set, so the upgrade chain runs now, one step at a time.", iDBVersion, requiredVersion));

         SchemaUpgrader::Outcome outcome = GetDBManager()->UpgradeSchema(requiredVersion, IniFileSettings::Instance()->GetDBScriptDirectory(),
            [](const String &line) { SchemaUpgrader::Note("Schema: " + line); });

         if (!outcome.ok)
         {
            sErrorMessage = Formatter::Format("The database is too old for this version of hMailServer, and the upgrade [Database] UpgradeOnStart asked for stopped at step {0}: {1} The database is at version {2}; the steps before that one have been applied and it will resume there. Fix the cause and start the service again, or run hMailServer Database updater (DBUpdater.exe) / hmailserver --upgrade-database. Required database version: {3}",
               outcome.failed_step, outcome.error, outcome.version, requiredVersion);
            last_connect_error_message_ = sErrorMessage;
            ErrorManager::Instance()->ReportError(ErrorManager::Critical, 5011, "Application::OnDatabaseConnected", sErrorMessage);
            return false;
         }

         iDBVersion = outcome.version;
         SchemaUpgrader::Note(Formatter::Format("Schema: upgraded in {0} step(s); the database is at version {1}.", outcome.steps, iDBVersion));
      }

      if (iDBVersion < Configuration::Instance()->GetRequiredDBVersion()
          || iDBVersion > Configuration::Instance()->GetRequiredDBVersion())
      {
         // Into the caller's message, which a local of the same name hid until 6.3.6:
         // on Linux the start printed "hMailServer could not start: " and nothing after
         // it, and a server that refused its database said why only in the error log -
         // where an update's outcome sends an administrator to the journal first.
         sErrorMessage.Empty();

         if (iDBVersion > Configuration::Instance()->GetRequiredDBVersion())
            sErrorMessage = "The database is too new for this version of hMailServer. Please upgrade hMailServer.";
         else
            sErrorMessage = "The database is too old for this version of hMailServer. Please run hMailServer Database updater (DBUpdater.exe) to upgrade it.";

         String sVersionInfo;
         sVersionInfo.Format(_T(" Database version: %d, Required database version: %d"), iDBVersion, Configuration::Instance()->GetRequiredDBVersion());

         sErrorMessage += sVersionInfo;
         
         last_connect_error_message_ = sErrorMessage;

         // Expected, not an error, on the start the installer makes for the
         // database step of an upgrade (SetStartedForDatabaseUpgrade): the new
         // binary is in place and DBUpdater is about to move the schema through
         // this process's hMailServer.Database. Until 25 September 2026 that
         // start was DBSetupQuick's COM activation, and every silent upgrade
         // across a schema step wrote this refusal to the error log and to the
         // Application event log, as a Critical event 2010 that also registered
         // the event source. The refusal stands: nothing is loaded, and the
         // message above is what Connect() answers, which DBUpdater reads and
         // expects. It is not reported, and not logged either - the log mask is
         // not loaded until Configuration::Load, which a refused start never
         // reaches - so the record of this start is the installer's log, which
         // says it made it. So is the restart DBSetup asks for after COM's
         // SetDefaultDatabase has named a database that exists, which DBSetup
         // reads the versions of next and upgrades with DBUpdater when it is
         // older: until 6.3.6 that restart wrote this refusal as a Critical
         // HM5011 to the error log and the Application event log on every new
         // installation given /createdatabase=0 and an older database, which then
         // succeeded (the installer smoke VM, 2 October 2026). A start made any
         // other way - the service controller at boot, net start, services.msc,
         // a COM activation - still reports it, and so does a database newer than
         // the binary, which no upgrade puts right.
         if (databaseUpgradeExpected && iDBVersion < Configuration::Instance()->GetRequiredDBVersion())
            return false;

         ErrorManager::Instance()->ReportError(ErrorManager::Critical, 5011, "Application::OnDatabaseConnected", sErrorMessage);

         return false;
      }

      LOG_DEBUG("Application::~OnDatabaseConnected() - E2");
      return true;
   }

   bool
   Application::ExitInstance()
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Do uninitialization: a reinitialise's, and a command-line verb's before its
   // process ends. False, as it always was; no caller reads it.
   //---------------------------------------------------------------------------()
   {
      ExitInstance_(StopToStartAgain, true);
      return false;
   }

   bool
   Application::ExitInstance_(StopPurpose purpose, bool indexerEnded)
   {
      initialized_ = false;

      // Close work queue
      WorkQueueManager::Instance()->RemoveQueue(maintenance_queue_);

      // What a room had not decided stays in hm_bookingqueue for the next start.
      WorkQueueManager::Instance()->RemoveQueue(ResourceBooking::WorkQueueName());

      // Normally already gone: StopServers removes these while io_service_ is still
      // alive, which is the only ordering that is safe. Kept here as a backstop for
      // the one path that skips it - a StartServers that failed before the state
      // reached Running, after which StopServers returns at its own first check.
      RemoveSessionWorkQueues_();

      // The same backstop for the mailbox keys' idle sweep, which such a start
      // may have started: stopped while what it reads is still here.
      MailboxKeys::StopIdleSweep();

      // Backup manager is created by initinstance so should be destroyed here -
      // let go of after the lock, by the last holder.
      std::shared_ptr<BackupManager> backupManager;
      {
         boost::lock_guard<boost::mutex> guard(managers_mutex_);
         backupManager.swap(backup_manager_);
      }
      backupManager.reset();

      // A reinitialise closes the database whatever is left, as it always has:
      // it opens it again at once, and the process goes on.
      bool ended = true;
      if (purpose == StopBeforeExit)
      {
         ended = CloseDatabaseBeforeExit_(indexerEnded, false);
      }
      else
      {
         LOG_DEBUG("Application::ExitInstance - Closing database connection...");
         CloseDatabase();
      }

      MimeEnvironment::SetAutoFolding(false);

      OutOfMemoryHandler::Terminate();

      return ended;
   }

   bool
   Application::CloseDatabaseBeforeExit_(bool indexerEnded, bool holderLeft)
   {
      // Before the process ends, a thread may still be using the database: the
      // message indexer's after its minute, one of a work queue's that its
      // queue's stop gave up on after its ten seconds - a delivery, a script, a
      // lookup in a database or a host that has stopped answering - a COM
      // call's, or that of a start, a stop or a reinitialise ShutDown gave up
      // waiting for (holderLeft). Every connection not in use is closed now,
      // and each one in use as its caller hands it back - never under the
      // statement on it, which until 10 October 2026 the close freed (PQfinish,
      // mysql_close) while the call was still in it - and none is handed out
      // after this: a statement asked for meanwhile, by such a thread or a COM
      // call, finds no connection and fails as one the database could not take,
      // where it went through the manager let go of here.
      //
      // A connection still in use is waited for as long as what was committed
      // needs to reach the database's own storage before the process ends
      // (DatabaseConnectionManager::SecondsToWaitForConnectionsInUse): not at
      // all on a database server, which has it; on SQL Server Compact, which
      // runs in this process and writes what was committed to its file every
      // ten seconds, or when any of its connections to the file closes - and
      // whose one connection is then the one in use - that interval and two
      // seconds more. From 10 October 2026 until the re-check of that change
      // the database was left open, nothing closed, whenever the indexer had
      // not ended or a work queue's stop had once given up on a thread - in
      // this stop or any before it in the process's life, the thread ended or
      // not - and the process was then ended at once: on SQL Server Compact,
      // what had been committed since its last write was lost with it, as nine
      // acknowledged saves of the last second were in the re-check's run, whose
      // process ended without a close another way.
      std::shared_ptr<DatabaseConnectionManager> manager = GetDBManager();
      int inUse = 0;
      if (manager)
      {
         const int waitSeconds = DatabaseConnectionManager::SecondsToWaitForConnectionsInUse(IniFileSettings::Instance()->GetDatabaseType());
         inUse = manager->CloseWhenIdle(waitSeconds, StopProgress_());
      }

      // Read after the wait, which a thread may have ended in.
      const int threadsLeft = WorkQueue::GetThreadsLeft();

      if (inUse == 0 && indexerEnded && !holderLeft && threadsLeft == 0)
      {
         // Nothing left that may use it: let go of, as a reinitialise's close
         // does, and the process ends the ordinary way.
         LOG_DEBUG("Application::ExitInstance - Closing database connection...");
         CloseDatabase();
         return true;
      }

      // Left published, closed: a thread left running that asks for a
      // connection is answered that there is none. The process then ends at
      // once (ShutDown), without the destructors of statics, which such a
      // thread may be using too.
      String said;
      if (inUse > 0)
         said = Formatter::Format(_T("The database connections not in use are closed, and {0} still in use are left open"), inUse);
      else
         said = _T("The database is closed");

      if (!indexerEnded)
         said += _T("; the message indexer's thread has not ended");

      if (holderLeft)
         said += _T("; the start, the stop or the reinitialise of the servers this stop waited for has not ended");

      if (threadsLeft > 0)
         said += Formatter::Format(_T("; {0} thread(s) of the work queues, which their stop gave up on, still run"), threadsLeft);

      LOG_APPLICATION(said + _T(". The process ends now."));

      return false;
   }

   void
   Application::CloseDatabase()
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Close the database connection.
   //---------------------------------------------------------------------------()
   {
      // Disconnected while still published, as it always was, so that a caller
      // meanwhile finds it not connected rather than missing; then taken away
      // under the lock and let go of after it.
      std::shared_ptr<DatabaseConnectionManager> manager = GetDBManager();
      if (manager)
      {
         try
         {
            manager->Disconnect();
         }
         catch (...)
         {

         }

         {
            boost::lock_guard<boost::mutex> guard(managers_mutex_);
            db_manager_.reset();
         }
      }
   }

   std::shared_ptr<DatabaseConnectionManager>
   Application::GetDBManager() const
   {
      boost::lock_guard<boost::mutex> guard(managers_mutex_);
      return db_manager_;
   }

   std::shared_ptr<BackupManager>
   Application::GetBackupManager() const
   {
      boost::lock_guard<boost::mutex> guard(managers_mutex_);
      return backup_manager_;
   }

   String
   Application::GetExecutableName()
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Returns the name of the executable (h*.exe)
   //---------------------------------------------------------------------------()
   {
#ifdef HM_PLATFORM_POSIX
      // There are no module handles here, and no need for one: /proc/self/exe is
      // a symbolic link the kernel maintains to the running program's own image,
      // so reading it gives the same answer GetModuleFileName gives on Windows
      // and needs no cooperation from the program. readlink does not terminate
      // what it writes and does not report a truncation, so the buffer is one
      // character longer than the length asked for and the terminator is written
      // by hand.
      char szPath[1024];
      const ssize_t pathLength = ::readlink("/proc/self/exe", szPath, sizeof(szPath) - 1);

      if (pathLength <= 0)
         return String();

      szPath[pathLength] = '\0';

      String sPath(szPath);
      return sPath;
#else
      // A TCHAR array, not alloca(2048): the count passed to GetModuleFileName is
      // in characters, so asking for 2048 characters of a 2048 BYTE allocation
      // let it write twice the space that existed in a Unicode build.
      TCHAR szPath[1024];
      const DWORD characterCount = sizeof(szPath) / sizeof(TCHAR);

      HMODULE handle = GetModuleHandle(_T("hmailserver.exe"));
      DWORD dwPathLength = GetModuleFileName(handle, szPath, characterCount);

      if (dwPathLength == 0)
         return String();

      // On truncation the return value is the buffer size, and older Windows
      // versions did not terminate in that case.
      if (dwPathLength >= characterCount)
         dwPathLength = characterCount - 1;

      szPath[dwPathLength] = 0;

      String sPath(szPath);
      return sPath;
#endif
   }

   void
   Application::CreateSessionWorkQueues_()
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Creates the two work queues whose tasks hold a live TCPConnection.
   //
   // WHY THEY LIVE HERE AND NOT IN InitInstance. A task on either of these queues
   // keeps its session alive through a shared_ptr, and that session owns a socket
   // built on io_service_'s io_context. These queues were created in InitInstance
   // and removed in ExitInstance, while io_service_ is created in StartServers and
   // destroyed in StopServers - so the queues STRADDLED the io_context rather than
   // nesting inside it, and the last reference to a connection could be dropped
   // after the io_context it belonged to had been destroyed. The socket destructor
   // then writes through the destroyed context.
   //
   // That is not theoretical. It was observed on a Reinitialize: a reverse-DNS
   // prefetch sat in a two second DnsQuery while StopServers ran to completion,
   // finished 1.9 seconds after "Destructing IOCP", released the last reference to
   // its SMTPConnection, and faulted writing 0x...DC8 inside ~basic_stream_socket.
   // The shutdown even logged that it was waiting for it - "Still 1 remaining
   // threads in queue Name lookup queue. First task: AsynchronousTask" - it just
   // waited in the wrong place, two teardown steps too late.
   //
   // Creating them here, before io_service_ runs, and removing them in
   // StopServers before io_service_ is reset, makes the ordering an invariant of
   // the two functions rather than something each new queue has to remember.
   //
   // Each queue also keeps io_service_ alive for as long as the queue exists
   // (WorkQueue's outlives), and lets go of it after its io_context and the
   // handlers that never ran. The ordering above holds for a task that ends
   // within its queue's stop; one that does not - a wait the stop cannot
   // interrupt, past its ten seconds - leaves its queue to WorkQueueManager,
   // which keeps it until the thread has left. Until 6.3.7 the queue was freed
   // with the thread inside, and io_service_ with it when StopServers let go of
   // it. Now the I/O service lives as long as the queue does, so the task ends on
   // a connection whose context still exists, and the stop goes on meanwhile.
   //---------------------------------------------------------------------------()
   {
      // Kept alive by each queue below. Made by StartServers_ before this.
      std::shared_ptr<void> outlives = io_service_;

      // Processes asynchronous tasks from clients - principally message
      // finalization, which is what sends the final "250 OK".
      WorkQueueManager::Instance()->CreateWorkQueue(Configuration::Instance()->GetAsynchronousThreads(),
                                                    asynchronous_tasks_queue_, outlives);

      // Separate queue for blocking name lookups made on behalf of a live
      // session. These can stall for seconds on an address whose reverse zone
      // is unreachable, so they are kept off the asynchronous task queue that
      // finalizes (and acknowledges) received messages. Saturating this queue
      // only delays the lookups themselves, which degrade to "Unknown".
      WorkQueueManager::Instance()->CreateWorkQueue(Configuration::Instance()->GetAsynchronousThreads(),
                                                    name_lookup_queue_, outlives);

      // The never-ban list's verifications, on threads of their own: a ban of a
      // listed provider is not to wait behind the sessions' reverse lookups,
      // which an outsider whose reverse zone does not answer can keep backed up
      // (NeverBanNameList.h). Not monitored for stalls: a verification waits on
      // DNS for up to its deadline by design.
      WorkQueueManager::Instance()->CreateWorkQueue(NeverBanNameList::VerifyThreads, NeverBanNameList::QueueName(), outlives);

      // Both of these run tasks that are expected to finish, so "every thread busy
      // for longer than the threshold" means something is wedged and is worth an
      // error. The maintenance, main server and IOCP queues are deliberately not
      // opted in - see WorkQueue::SetMonitorForStalls for why that would report the
      // healthy state of a stock installation once a minute.
      if (std::shared_ptr<WorkQueue> queue = WorkQueueManager::Instance()->GetQueue(asynchronous_tasks_queue_))
         queue->SetMonitorForStalls(true);

      if (std::shared_ptr<WorkQueue> queue = WorkQueueManager::Instance()->GetQueue(name_lookup_queue_))
         queue->SetMonitorForStalls(true);
   }

   void
   Application::RemoveSessionWorkQueues_()
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Drains and removes the queues created by CreateSessionWorkQueues_. Must be
   // called while io_service_ is still alive; see there for what happens if it is
   // not. RemoveQueue joins the queue's worker threads, and on return a task that
   // ended within the join holds no connection. One that did not - a wait the
   // stop cannot interrupt, past its ten seconds - may still hold one: its queue
   // is then kept by WorkQueueManager until the thread has left, and keeps
   // io_service_, which the connection's socket is on, alive with it
   // (CreateSessionWorkQueues_), so that StopServers' reset of io_service_ does
   // not free the context under that task.
   //
   // Safe to call twice - RemoveQueue on a name that is not registered is a
   // no-op - which is what lets ExitInstance keep it as a backstop for a
   // StartServers that failed before the state reached Running.
   //---------------------------------------------------------------------------()
   {
      WorkQueueManager::Instance()->RemoveQueue(asynchronous_tasks_queue_);

      WorkQueueManager::Instance()->RemoveQueue(name_lookup_queue_);

      WorkQueueManager::Instance()->RemoveQueue(NeverBanNameList::QueueName());
   }

   bool
   Application::StartServers(int maxWaitSeconds, String *refusal)
   {
      boost::unique_lock<boost::timed_mutex> lock(servers_mutex_, boost::defer_lock);
      const String refused = TakeServersLock_(lock, *MessageIndexer::Instance(), maxWaitSeconds);
      ServersLockHeld_ held(*this, lock);
      if (!refused.IsEmpty())
      {
         LOG_APPLICATION("The servers are not started: " + refused);
         if (refusal)
            *refusal = refused;
         return false;
      }

      return StartServers_();
   }

   bool
   Application::InitInstanceAndStart(String &sErrorMessage, const std::function<bool()> &finish)
   {
      boost::unique_lock<boost::timed_mutex> lock(servers_mutex_, boost::defer_lock);
      const String refused = TakeServersLock_(lock, *MessageIndexer::Instance());
      ServersLockHeld_ held(*this, lock);
      if (!refused.IsEmpty())
      {
         sErrorMessage = "The server was not started: " + refused;
         return false;
      }

      if (!InitInstance(sErrorMessage))
         return false;

      if (finish && finish())
      {
         sErrorMessage = Reinitialize_(*MessageIndexer::Instance());
         return sErrorMessage.IsEmpty();
      }

      // A stop before the process ends that came during InitInstance waits for
      // this (ShutDown): nothing is started, and that stop closes the database.
      if (shutting_down_)
      {
         sErrorMessage = "The server was not started: it is stopping.";
         return false;
      }

      return StartServers_();
   }

   bool
   Application::StartServers_()
   {
      LOG_APPLICATION("Starting servers...")

      if (ServerStatus::Instance()->GetState() != ServerStatus::StateStopped)
      {
         LOG_DEBUG("Application::~StartServers() - E1");
         return false;
      }

      ServerStatus::Instance()->SetState(ServerStatus::StateStarting);

      // The firewall is made to match the auto-ban ranges before a listener is
      // open: a rule for a ban that expired while the service was down is removed
      // here, and one for a ban still in force is put back if it is missing. What
      // the hooks hold of an earlier run is learned first - the packaged hook is
      // reset, AutoBanCommand is told "unban" from its ledger - since nothing in
      // this process remembers it (AutoBanFirewall::SynchroniseAtStart). Here and
      // not in InitInstance, so that only a process about to listen does it - the
      // service's start, a reinitialise, the COM API's Start - and never a command
      // run beside a running server, whose firewall that server keeps.
      AutoBanFirewall::SynchroniseAtStart();

      SpamProtection::Instance()->Load();

      // Which accounts are keyed, read before any listener or delivery asks,
      // and what an encryption a crash cut short left beside a keyed mailbox's
      // files put right (Util/MailboxKeys.h). A server with no keyed account
      // reads the empty table and nothing more.
      MailboxKeys::Start();

      // The sweep that closes the keys gone idle, every ten seconds on a thread
      // of its own: not one of the scheduled tasks, whose turns come a minute
      // plus whatever all of them took apart (MailboxKeys.h).
      MailboxKeys::StartIdleSweep();

      // What an IMAP APPEND the last process stopped in the middle of left in
      // the message store - a literal's file with no row - removed before any
      // listener is up (IMAP/IMAPCommandAppend.h).
      IMAPCommandAppend::SweepAbandonedAppends();

      // Made before the session queues, which keep it alive (CreateSessionWorkQueues_),
      // and run only after them: nothing listens until its task runs on the main
      // server queue below, so no listener can ever accept a connection on a
      // context whose session queues do not exist yet.
      io_service_ = std::shared_ptr<IOService>(new IOService);
      io_service_->Initialize();

      CreateSessionWorkQueues_();

      RegisterSessionTypes_();

      // Create the main work queue.
      size_t iMainServerQueue = WorkQueueManager::Instance()->CreateWorkQueue(4, server_work_queue_);

      notification_server_ = std::shared_ptr<NotificationServer>(new NotificationServer());
      folder_manager_ = std::shared_ptr<FolderManager>(new FolderManager());
      
      // Create the scheduler. This is always in use.
      {
         boost::lock_guard<boost::mutex> handles(scheduler_handles_mutex_);
         scheduler_ = std::shared_ptr<Scheduler>(new Scheduler());
      }
      WorkQueueManager::Instance()->AddTask(iMainServerQueue, scheduler_);

      WorkQueueManager::Instance()->AddTask(iMainServerQueue, io_service_);

      smtp_delivery_manager_ = std::shared_ptr<SMTPDeliveryManager>(new SMTPDeliveryManager);
      WorkQueueManager::Instance()->AddTask(iMainServerQueue, smtp_delivery_manager_);

      external_fetch_manager_ = std::shared_ptr<ExternalFetchManager> (new ExternalFetchManager);
      WorkQueueManager::Instance()->AddTask(iMainServerQueue, external_fetch_manager_);

      CreateScheduledTasks_();

      if (Configuration::Instance()->GetMessageIndexing())
      {
         MessageIndexer::Instance()->Start();
      }

      // The sandbox's poller (AntiVirus/Sandbox.h). Started whether or not the
      // sandbox is on: a message held while it was on is put back in the queue
      // by the poller when it is turned off, and nothing else does that.
      Sandbox::Start();

      start_time_ = Time::GetCurrentDateTime();

      scheduler_->GetIsStartedEvent().Wait();
      smtp_delivery_manager_->GetIsStartedEvent().Wait();
      external_fetch_manager_->GetIsStartedEvent().Wait();
      io_service_->GetIsStartedEvent().Wait();

      ServerStatus::Instance()->SetState(ServerStatus::StateRunning);

      // Start the Prometheus metrics endpoint if enabled in hMailServer.ini.
      int metricsPort = IniFileSettings::Instance()->GetMetricsServerPort();
      if (metricsPort > 0)
      {
         metrics_server_ = std::shared_ptr<MetricsServer>(new MetricsServer());
         metrics_server_->Start(IniFileSettings::Instance()->GetMetricsServerBindAddress(), metricsPort);
      }

      // The read-only SNMP agent, beside the metrics endpoint and on its
      // lifecycle, answering from the same counters (Util/SnmpAgent.h). Off by
      // default; a start that fails has said why, and the server runs without it.
      if (Configuration::Instance()->GetSnmpEnabled())
      {
         snmp_agent_ = std::shared_ptr<SnmpAgent>(new SnmpAgent());
         if (!snmp_agent_->Start())
            snmp_agent_.reset();
      }

      // Start the OpenTelemetry exporters - one per OTLP signal, each a no-op
      // unless its own endpoint is configured.
      OtelTracer::Instance()->Start();
      OtelMetricsExporter::Instance()->Start();
      OtelLogExporter::Instance()->Start();

      // The syslog sink: RFC 5424 to a collector, or the journal on a systemd
      // host with no collector configured. Reads its settings from hm_settings,
      // which Configuration::Load has already refreshed, so this is also where a
      // Reinitialize picks a changed setting up.
      SyslogSink::Instance()->Start();

      // Start the REST administration API if enabled in hMailServer.ini.
      int restApiPort = IniFileSettings::Instance()->GetRestApiPort();
      if (restApiPort > 0)
      {
         String restCertificateFile = IniFileSettings::Instance()->GetRestApiCertificateFile();
         String restPrivateKeyFile = IniFileSettings::Instance()->GetRestApiPrivateKeyFile();

         // No certificate configured? Fall back to the ACME certificate if one exists.
         if (restCertificateFile.IsEmpty())
         {
            String acmeCertificate = AcmeClient::GetCertificateDirectory() + FileUtilities::PathSeparator + _T("fullchain.pem");
            String acmePrivateKey = AcmeClient::GetCertificateDirectory() + FileUtilities::PathSeparator + _T("privkey.pem");

            if (FileUtilities::Exists(acmeCertificate) && FileUtilities::Exists(acmePrivateKey))
            {
               restCertificateFile = acmeCertificate;
               restPrivateKeyFile = acmePrivateKey;
               LOG_APPLICATION("RestApi: Using the ACME certificate for HTTPS.");
            }
         }

         rest_api_server_ = std::shared_ptr<RestApiServer>(new RestApiServer());
         rest_api_server_->Start(IniFileSettings::Instance()->GetRestApiBindAddress(), restApiPort,
            restCertificateFile, restPrivateKeyFile);
      }

      // Start the public web services (MTA-STS policy hosting, client
      // autoconfiguration, ACME challenges) if enabled in hMailServer.ini.
      int webServicesHttpPort = IniFileSettings::Instance()->GetWebServicesHttpPort();
      int webServicesHttpsPort = IniFileSettings::Instance()->GetWebServicesHttpsPort();

      // Unconditionally, and deliberately *outside* the port check below.
      //
      // This report exists to say "these features are enabled but nothing is
      // listening, so nothing answers those URLs" - which is only ever true when
      // both ports are 0. Its only previous caller was WebServicesServer::Start(),
      // reached solely from inside the same `if` below, so at the moment it ran
      // both ports were guaranteed non-zero and its main branch could never
      // execute. The report has therefore never been emitted, in this release or
      // any earlier one, for exactly the configuration that needs it: the shipped
      // default, where MtaStsHostingEnabled is 1 and both ports are 0.
      //
      // It only reads settings and writes to the application log - no sockets, no
      // member state - so calling it before any listener exists is safe. Its
      // once-per-process guard makes the call still inside Start() a no-op.
      WebServicesServer::ReportUnreachableFeatures();

      if (webServicesHttpPort > 0 || webServicesHttpsPort > 0)
      {
         web_services_server_ = std::shared_ptr<WebServicesServer>(new WebServicesServer());
         web_services_server_->Start(IniFileSettings::Instance()->GetWebServicesBindAddress(),
            webServicesHttpPort, webServicesHttpsPort,
            IniFileSettings::Instance()->GetWebServicesCertificateFile(),
            IniFileSettings::Instance()->GetWebServicesPrivateKeyFile());
      }

      // Start the ManageSieve (RFC 5804) script-management listener if enabled.
      int manageSievePort = IniFileSettings::Instance()->GetManageSieveServerPort();
      if (manageSievePort > 0)
      {
         manage_sieve_server_ = std::shared_ptr<ManageSieveServer>(new ManageSieveServer());
         manage_sieve_server_->Start(IniFileSettings::Instance()->GetManageSieveServerBindAddress(), manageSievePort);
      }

      // A mailbox rename or move that was interrupted - the process killed
      // between the store being moved and the account row being written - is
      // visible in hm_mailboxmoves and nowhere else, so it is said here, once
      // per process, rather than waiting for somebody to go looking.
      MailboxMove::ReportInterrupted();

      // A domain whose name is a directory the server keeps its own files in
      // under the data directory - made before such names were refused, or put
      // back by a restore - shares that directory with the store, and nothing
      // else says so: said here, each time the servers start.
      PreSaveLimitationsCheck::ReportReservedDomainNames();

      // Migration jobs (MigrationManager.h), last: a job reads its source over
      // the network - this server's own IMAP listener, when the source is a
      // domain of this server - and files into the message store, so both have
      // to be up. A job a stop left running is queued again here.
      MigrationManager::Instance()->Start();

      LOG_APPLICATION("Servers started.")

      return true;
   }

   void 
   Application::RegisterSessionTypes_()
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Registers the different session types...
   //---------------------------------------------------------------------------()
   {
      // Start SMTP server and delivery threads.
      if (Configuration::Instance()->GetUseSMTP())
      {
         io_service_->RegisterSessionType(STSMTP);
      }

      if (Configuration::Instance()->GetUsePOP3())
      {
         io_service_->RegisterSessionType(STPOP3);
      }

      if (Configuration::Instance()->GetUseIMAP())
      {
         io_service_->RegisterSessionType(STIMAP);
      }
   }

   void
   Application::ScheduleBackups_(std::shared_ptr<Scheduler> scheduler)
   {
      if (!scheduler)
         return;

      std::shared_ptr<BackupScheduleTask> backupScheduleTask = std::shared_ptr<BackupScheduleTask>(new BackupScheduleTask);
      backupScheduleTask->SetReoccurance(ScheduledTask::RunInfinitely);
      backupScheduleTask->SetMinutesBetweenRun(1);
      scheduler->ScheduleTask(backupScheduleTask);

      // Kept, so that a schedule saved while one is already registered is not
      // registered twice (SubscribeToSettings_) - and only while the scheduler it
      // went into is still the running one: a stop that reset the handles while a
      // subscriber registered on the old scheduler must not leave that handle
      // behind for the next start to take for its own.
      boost::lock_guard<boost::mutex> handles(scheduler_handles_mutex_);
      if (scheduler_ == scheduler)
         backup_schedule_task_ = backupScheduleTask;
   }

   bool
   Application::IsServing_()
   {
      const int state = ServerStatus::Instance()->GetState();
      return state == ServerStatus::StateRunning || state == ServerStatus::StateDraining;
   }

   void
   Application::AnnounceDatabaseFaultInjector_(const String &pattern)
   {
      // High severity and the error log rather than the application log, deliberately:
      // this makes writes fail on purpose, and an operator who finds mail behaving
      // strangely should meet this line before they start looking anywhere else.
      ErrorManager::Instance()->ReportError(ErrorManager::High, 6119, "Application::OpenDatabase",
         "[Settings] SimulateDatabaseFailureFor is set to '" + pattern +
         "', so every database statement containing that text is being FAILED ON PURPOSE. This is a test facility. "
         "Clear the setting - it applies when it is saved - unless you are deliberately testing failure handling.");
   }

   void
   Application::SubscribeToSettings_()
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // The subsystems that copy a [Settings] value into something of their own when
   // the servers start, and can take a new one in place, take it here when a save
   // changes it (IniFileSettings::Subscribe) - which is what lets the baseline mark
   // their keys now. Once per process: the subscriptions outlive a reinitialise,
   // which rebuilds what they reach and finds them still in place.
   //
   // Each reaches only a server that is running or draining; a stopped or starting
   // one reads the new value when StartServers runs, since the save published it
   // first. The two that reach the scheduler do nothing while a stop, a start or a
   // reinitialise holds servers_mutex_: it is rebuilding the scheduler from the
   // published values, or tearing it down, and a handle to the one it is tearing
   // down is not worth racing for. They hold it while they act, and say so as a
   // stop does (ServersLockHeld_), so that an OnError script an error of theirs
   // runs, asking for a reinitialise, a stop or a start, is refused at once
   // rather than left waiting for the mutex its own thread holds, as until the
   // re-check of 10 October 2026. A service stop did not take that mutex until 10
   // October 2026, so the scheduler and the two task handles are copied under
   // scheduler_handles_mutex_, which the stop's reset takes too. Each reads the
   // value it takes when it runs, as Subscribe asks, and runs after the save's
   // reload lock is released (IniFileSettings::Tell_).
   //
   // docs/SettingsApply.md lists them; a latch found later that cannot be redone
   // safely is marked reinitialize in build/ini-settings-baseline.txt instead.
   //---------------------------------------------------------------------------()
   {
      if (settings_subscribed_)
         return;

      settings_subscribed_ = true;

      IniFileSettings *ini = IniFileSettings::Instance();

      // The greylist cleaner's period, set from the setting when the task is built.
      ini->Subscribe(_T("GreylistingRecordExpirationInterval"), [this]()
      {
         boost::unique_lock<boost::timed_mutex> rebuilding(servers_mutex_, boost::try_to_lock);
         ServersLockHeld_ held(*this, rebuilding);
         if (!rebuilding.owns_lock() || !IsServing_())
            return;

         std::shared_ptr<Scheduler> scheduler;
         std::shared_ptr<ScheduledTask> task;
         {
            boost::lock_guard<boost::mutex> handles(scheduler_handles_mutex_);
            scheduler = scheduler_;
            task = greylist_cleaner_task_;
         }

         if (scheduler && task)
            scheduler->SetTaskInterval(task, IniFileSettings::Instance()->GetGreylistingExpirationInterval());
      });

      // The backup schedule: registered at a start only when one is configured. A
      // schedule set on a server that started without one is registered now, as a
      // start would register it. One already registered reads the time and the
      // interval on every tick and needs nothing here; one switched off no-ops on
      // its next tick, as it always has.
      std::vector<String> backupSchedule;
      backupSchedule.push_back(_T("ScheduledBackupTime"));
      backupSchedule.push_back(_T("ScheduledBackupIntervalMinutes"));
      ini->Subscribe(backupSchedule, [this]()
      {
         if (!BackupScheduleTask::IsEnabled())
            return;

         boost::unique_lock<boost::timed_mutex> rebuilding(servers_mutex_, boost::try_to_lock);
         ServersLockHeld_ held(*this, rebuilding);
         if (!rebuilding.owns_lock() || !IsServing_())
            return;

         std::shared_ptr<Scheduler> scheduler;
         {
            boost::lock_guard<boost::mutex> handles(scheduler_handles_mutex_);
            if (!scheduler_ || backup_schedule_task_)
               return;
            scheduler = scheduler_;
         }

         ScheduleBackups_(scheduler);
      });

      // The three OpenTelemetry exporters, each of which reads its endpoint, the
      // service name and (metrics) the interval when it starts. Start() stops the
      // running exporter and starts it on the published values, under the
      // exporter's own lock, so a restart from here and one from a reinitialise
      // cannot overlap.
      std::vector<String> tracer;
      tracer.push_back(_T("OtelEndpoint"));
      tracer.push_back(_T("OtelServiceName"));
      ini->Subscribe(tracer, []()
      {
         if (IsServing_())
            OtelTracer::Instance()->Start();
      });

      std::vector<String> metrics;
      metrics.push_back(_T("OtelMetricsEndpoint"));
      metrics.push_back(_T("OtelServiceName"));
      metrics.push_back(_T("OtelMetricsInterval"));
      ini->Subscribe(metrics, []()
      {
         if (IsServing_())
            OtelMetricsExporter::Instance()->Start();
      });

      std::vector<String> logs;
      logs.push_back(_T("OtelLogsEndpoint"));
      logs.push_back(_T("OtelServiceName"));
      ini->Subscribe(logs, []()
      {
         if (IsServing_())
            OtelLogExporter::Instance()->Start();
      });

      // The RFC 6764 redirect targets, cached for a minute by the web services
      // listener: dropped, so the next request reads the saved value.
      std::vector<String> davRedirects;
      davRedirects.push_back(_T("CalDavRedirectUrl"));
      davRedirects.push_back(_T("CardDavRedirectUrl"));
      ini->Subscribe(davRedirects, []()
      {
         WebServicesServer::ForgetDavRedirectTargets();
      });

      // The two per-minute rate limits start their window again when they are
      // saved: the events counted under the old limit are dropped, as a restart
      // always dropped them, so a limit counts from when it was set. Without this
      // a limit saved a few seconds after traffic counted under an earlier one
      // refused the first transaction it saw - found by SMTP.RateShaping, run
      // within a minute of a test that had used the limit.
      ini->Subscribe(_T("MaxSubmissionsPerIPPerMinute"), []()
      {
         RateLimiter::Instance()->ClearBuckets(_T("smtp-submit:"));
      });

      ini->Subscribe(_T("MaxOutboundPerDestinationPerMinute"), []()
      {
         RateLimiter::Instance()->ClearBuckets(_T("smtp-out:"));
      });

      // The database fault injector applies when it is saved, and says so the
      // moment it is set - the start and a reinitialise say it as well.
      ini->Subscribe(_T("SimulateDatabaseFailureFor"), []()
      {
         const String pattern = IniFileSettings::Instance()->GetSimulateDatabaseFailureFor();
         if (!pattern.IsEmpty())
            AnnounceDatabaseFaultInjector_(pattern);
      });
   }

   void 
   Application::CreateScheduledTasks_()
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Create scheduled tasks.
   //---------------------------------------------------------------------------()
   {
      if (Configuration::Instance()->GetUseSMTP())
      {
         std::shared_ptr<GreyListCleanerTask> pCleanerTask = std::shared_ptr<GreyListCleanerTask>(new GreyListCleanerTask);
         pCleanerTask->SetReoccurance(ScheduledTask::RunInfinitely);
         pCleanerTask->SetMinutesBetweenRun(IniFileSettings::Instance()->GetGreylistingExpirationInterval());
         scheduler_->ScheduleTask(pCleanerTask);

         // Kept, so that a saved GreylistingRecordExpirationInterval can move its
         // period in place (SubscribeToSettings_).
         boost::lock_guard<boost::mutex> handles(scheduler_handles_mutex_);
         greylist_cleaner_task_ = pCleanerTask;
      }

      // cleaning of expired IP ranges.
      std::shared_ptr<RemoveExpiredRecords> removeExpiredRecordsTask = std::shared_ptr<RemoveExpiredRecords>(new RemoveExpiredRecords);
      removeExpiredRecordsTask->SetReoccurance(ScheduledTask::RunInfinitely);
      removeExpiredRecordsTask->SetMinutesBetweenRun(1);
      scheduler_->ScheduleTask(removeExpiredRecordsTask);

      // What the webmail put off until later: drafts to send at a time,
      // snoozed messages to come back. Every minute, since a time is chosen
      // to the minute.
      std::shared_ptr<ScheduledMailTask> scheduledMailTask = std::shared_ptr<ScheduledMailTask>(new ScheduledMailTask);
      scheduledMailTask->SetReoccurance(ScheduledTask::RunInfinitely);
      scheduledMailTask->SetMinutesBetweenRun(1);
      scheduler_->ScheduleTask(scheduledMailTask);

      // SMTP TLS reporting (RFC 8460). The task always runs so collected
      // statistics are flushed; reports are only sent when TlsRptFromAddress
      // is configured.
      std::shared_ptr<TlsRptReporterTask> tlsRptTask = std::shared_ptr<TlsRptReporterTask>(new TlsRptReporterTask);
      tlsRptTask->SetReoccurance(ScheduledTask::RunInfinitely);
      tlsRptTask->SetMinutesBetweenRun(60);
      scheduler_->ScheduleTask(tlsRptTask);

      // DMARC aggregate reporting (RFC 7489). Same shape as the TLS-RPT task
      // above: the task always runs so collected statistics are flushed;
      // reports are only sent when DmarcRptFromAddress is configured.
      std::shared_ptr<DmarcRptReporterTask> dmarcRptTask = std::shared_ptr<DmarcRptReporterTask>(new DmarcRptReporterTask);
      dmarcRptTask->SetReoccurance(ScheduledTask::RunInfinitely);
      dmarcRptTask->SetMinutesBetweenRun(60);
      scheduler_->ScheduleTask(dmarcRptTask);

      // The suppression list's sweeps: expired entries, old counts, old report
      // keys. Once at start and then hourly, whether or not bounce processing is on,
      // so switching it off does not strand what it wrote; on a server that never
      // switched it on each pass is three counts of nothing.
      std::shared_ptr<BounceMaintenanceTask> bounceMaintenanceStartupTask = std::shared_ptr<BounceMaintenanceTask>(new BounceMaintenanceTask);
      bounceMaintenanceStartupTask->SetReoccurance(ScheduledTask::RunOnce);
      scheduler_->ScheduleTask(bounceMaintenanceStartupTask);

      std::shared_ptr<BounceMaintenanceTask> bounceMaintenanceTask = std::shared_ptr<BounceMaintenanceTask>(new BounceMaintenanceTask);
      bounceMaintenanceTask->SetReoccurance(ScheduledTask::RunInfinitely);
      bounceMaintenanceTask->SetMinutesBetweenRun(60);
      scheduler_->ScheduleTask(bounceMaintenanceTask);

      // The feedback loops' sweep: complaints older than ComplaintRetentionDays and
      // the daily tally rows behind them. Once at start and then hourly, whether or
      // not complaint processing is on, so switching it off does not leave a record
      // of who complained about what standing for ever.
      std::shared_ptr<ComplaintMaintenanceTask> complaintMaintenanceStartupTask = std::shared_ptr<ComplaintMaintenanceTask>(new ComplaintMaintenanceTask);
      complaintMaintenanceStartupTask->SetReoccurance(ScheduledTask::RunOnce);
      scheduler_->ScheduleTask(complaintMaintenanceStartupTask);

      std::shared_ptr<ComplaintMaintenanceTask> complaintMaintenanceTask = std::shared_ptr<ComplaintMaintenanceTask>(new ComplaintMaintenanceTask);
      complaintMaintenanceTask->SetReoccurance(ScheduledTask::RunInfinitely);
      complaintMaintenanceTask->SetMinutesBetweenRun(60);
      scheduler_->ScheduleTask(complaintMaintenanceTask);

      // Log retention: prune date-stamped log files older than LogDeleteDays.
      // The task itself no-ops when LogDeleteDays <= 0 (retention disabled).
      // Run once promptly at startup, then periodically thereafter.
      std::shared_ptr<LogRetentionTask> logRetentionStartupTask = std::shared_ptr<LogRetentionTask>(new LogRetentionTask);
      logRetentionStartupTask->SetReoccurance(ScheduledTask::RunOnce);
      scheduler_->ScheduleTask(logRetentionStartupTask);

      std::shared_ptr<LogRetentionTask> logRetentionTask = std::shared_ptr<LogRetentionTask>(new LogRetentionTask);
      logRetentionTask->SetReoccurance(ScheduledTask::RunInfinitely);
      logRetentionTask->SetMinutesBetweenRun(6 * 60);
      scheduler_->ScheduleTask(logRetentionTask);

      // Archive retention: prune ArchiveDir to ArchiveRetentionDays. Same shape as
      // the log sweep above and no-ops the same way when the setting is 0, which is
      // its default. Twelve hours rather than six because the archive is a directory
      // tree rather than one folder, and nothing about it is urgent.
      std::shared_ptr<ArchiveRetentionTask> archiveRetentionStartupTask = std::shared_ptr<ArchiveRetentionTask>(new ArchiveRetentionTask);
      archiveRetentionStartupTask->SetReoccurance(ScheduledTask::RunOnce);
      scheduler_->ScheduleTask(archiveRetentionStartupTask);

      std::shared_ptr<ArchiveRetentionTask> archiveRetentionTask = std::shared_ptr<ArchiveRetentionTask>(new ArchiveRetentionTask);
      archiveRetentionTask->SetReoccurance(ScheduledTask::RunInfinitely);
      archiveRetentionTask->SetMinutesBetweenRun(12 * 60);
      scheduler_->ScheduleTask(archiveRetentionTask);

      // Mailbox retention: delivered mail older than a domain's or account's
      // MessageRetentionDays. The task no-ops for every mailbox without a policy,
      // which is all of them until an administrator sets one; every six hours, so a
      // policy takes effect the day it is set rather than the day after.
      std::shared_ptr<MailboxRetentionTask> mailboxRetentionStartupTask = std::shared_ptr<MailboxRetentionTask>(new MailboxRetentionTask);
      mailboxRetentionStartupTask->SetReoccurance(ScheduledTask::RunOnce);
      scheduler_->ScheduleTask(mailboxRetentionStartupTask);

      std::shared_ptr<MailboxRetentionTask> mailboxRetentionTask = std::shared_ptr<MailboxRetentionTask>(new MailboxRetentionTask);
      mailboxRetentionTask->SetReoccurance(ScheduledTask::RunInfinitely);
      mailboxRetentionTask->SetMinutesBetweenRun(6 * 60);
      scheduler_->ScheduleTask(mailboxRetentionTask);

      // Metric history: one row per metric per minute, kept MetricsHistoryDays.
      // The task no-ops when that is 0. A sample at startup as well, so the
      // history shows the restart as the drop in every counter that it is.
      std::shared_ptr<MetricsHistoryTask> metricsHistoryStartupTask = std::shared_ptr<MetricsHistoryTask>(new MetricsHistoryTask);
      metricsHistoryStartupTask->SetReoccurance(ScheduledTask::RunOnce);
      scheduler_->ScheduleTask(metricsHistoryStartupTask);

      std::shared_ptr<MetricsHistoryTask> metricsHistoryTask = std::shared_ptr<MetricsHistoryTask>(new MetricsHistoryTask);
      metricsHistoryTask->SetReoccurance(ScheduledTask::RunInfinitely);
      metricsHistoryTask->SetMinutesBetweenRun(1);
      scheduler_->ScheduleTask(metricsHistoryTask);

      // The quarantine digest. Every five minutes rather than once a day, so
      // that a domain's sends are spread across its digest hour instead of
      // arriving as one storm; the task no-ops when the quarantine is off, when
      // no domain has the digest switched on, or when no base URL has been set
      // for its links. No startup run: a restart must not send anybody a second
      // digest.
      std::shared_ptr<QuarantineDigestTask> quarantineDigestTask = std::shared_ptr<QuarantineDigestTask>(new QuarantineDigestTask);
      quarantineDigestTask->SetReoccurance(ScheduledTask::RunInfinitely);
      quarantineDigestTask->SetMinutesBetweenRun(5);
      scheduler_->ScheduleTask(quarantineDigestTask);

      // The list manager's digests and its sweep (Application/ListDigestTask.h).
      // Every five minutes, so a list's digest goes within five minutes of its
      // hour. On a server with no managed list both ask one short question -
      // whether any list is managed, remembered for a minute - and do nothing
      // more.
      // No start-up run, for the quarantine digest's reason: a restart must not
      // send anybody a second digest in a day.
      std::shared_ptr<ListDigestTask> listDigestTask = std::shared_ptr<ListDigestTask>(new ListDigestTask);
      listDigestTask->SetReoccurance(ScheduledTask::RunInfinitely);
      listDigestTask->SetMinutesBetweenRun(5);
      scheduler_->ScheduleTask(listDigestTask);

      // The update check: does the project have a newer release than this one.
      // The task no-ops until UpdateCheckEnabled=1 (Status.CheckForUpdate is the
      // on-demand form and needs no opt-in). Once at startup, so a server that is
      // only ever restarted still hears, then every quarter hour - the feed is
      // read every UpdateCheckHours, but an UpdateWindow an hour long must not be
      // missed, and it is the task that notices the window is open.
      UpdateChecker::ResetSchedule();

      // A token that is still here at start belongs to no apply. The helper stops
      // the service, runs the installer - which is where DBUpdater redeems the
      // token - and starts the service again, so a redeemed token is gone before
      // this line runs and an unredeemed one is from an apply that never happened.
      // It is the administrator password for an hour, so it is not left lying.
      UpdateApplyToken::Revoke();

      std::shared_ptr<UpdateCheckTask> updateCheckStartupTask = std::shared_ptr<UpdateCheckTask>(new UpdateCheckTask);
      updateCheckStartupTask->SetReoccurance(ScheduledTask::RunOnce);
      scheduler_->ScheduleTask(updateCheckStartupTask);

      std::shared_ptr<UpdateCheckTask> updateCheckTask = std::shared_ptr<UpdateCheckTask>(new UpdateCheckTask);
      updateCheckTask->SetReoccurance(ScheduledTask::RunInfinitely);
      updateCheckTask->SetMinutesBetweenRun(15);
      scheduler_->ScheduleTask(updateCheckTask);

      // QRESYNC expunge records: cap hm_imapexpunged at IMAPExpungeRetentionRecords
      // per mailbox and drop rows whose folder no longer exists. Same startup-plus-
      // periodic shape as the two sweeps above, and twelve hours for the same
      // reason - a table that has been growing since the installation was built is
      // not going to be urgent on the day this ships.
      //
      // Registered unconditionally, unlike the greylist cleaner above, because the
      // records are written whenever a message is expunged from a folder and not
      // only when some feature is switched on.
      std::shared_ptr<IMAPExpungeRetentionTask> expungeRetentionStartupTask = std::shared_ptr<IMAPExpungeRetentionTask>(new IMAPExpungeRetentionTask);
      expungeRetentionStartupTask->SetReoccurance(ScheduledTask::RunOnce);
      scheduler_->ScheduleTask(expungeRetentionStartupTask);

      std::shared_ptr<IMAPExpungeRetentionTask> expungeRetentionTask = std::shared_ptr<IMAPExpungeRetentionTask>(new IMAPExpungeRetentionTask);
      expungeRetentionTask->SetReoccurance(ScheduledTask::RunInfinitely);
      expungeRetentionTask->SetMinutesBetweenRun(12 * 60);
      scheduler_->ScheduleTask(expungeRetentionTask);

      // Message-store consistency: cross-check the message database against the
      // files on disk and publish the number of missing files. The task no-ops
      // when MessageStoreConsistencyCheck is disabled (the default). Run once
      // promptly at startup, then periodically thereafter.
      std::shared_ptr<MessageStoreConsistencyTask> consistencyStartupTask = std::shared_ptr<MessageStoreConsistencyTask>(new MessageStoreConsistencyTask);
      consistencyStartupTask->SetReoccurance(ScheduledTask::RunOnce);
      scheduler_->ScheduleTask(consistencyStartupTask);

      std::shared_ptr<MessageStoreConsistencyTask> consistencyTask = std::shared_ptr<MessageStoreConsistencyTask>(new MessageStoreConsistencyTask);
      consistencyTask->SetReoccurance(ScheduledTask::RunInfinitely);
      consistencyTask->SetMinutesBetweenRun(60);
      scheduler_->ScheduleTask(consistencyTask);

      // The object store's committer (docs/ObjectStorage.md): every minute, the
      // messages placed on the local disk and handed over are put into the store
      // on the maintenance queue. Returns at once while no store is written to.
      scheduler_->ScheduleTask(MessageStoreObjects::CommitTask());

      // Names the tasks holding the message-acknowledgement threads if they are
      // ever all busy at once with work queued behind them. Runs on the
      // scheduler's own thread, which belongs to a different queue than the one
      // it measures.
      // Free space on the volume holding the message store. Registered
      // unconditionally, because the half of that feature worth the most is the
      // one that speaks BEFORE anything breaks: a line saying "1% free" at 03:00
      // is worth more than a server that starts refusing mail at 09:00.
      //
      // Two registrations, like log retention and the consistency check: one at
      // start-up, so an administrator who has just restarted a struggling server
      // is told immediately, and one hourly thereafter. Hourly rather than by the
      // minute because a disk fills over days and the task announces only a CHANGE
      // of band - a server sitting in the warning band writes one line, not one an
      // hour. The accept path notices the floor being crossed within seconds
      // regardless; this task exists for the band nothing else looks at.
      //
      // Nothing here fires on a healthy default install: the warning threshold is
      // 1024 MB and the floor 100 MB.
      std::shared_ptr<DiskSpaceMonitorTask> diskSpaceStartupTask = std::shared_ptr<DiskSpaceMonitorTask>(new DiskSpaceMonitorTask);
      diskSpaceStartupTask->SetReoccurance(ScheduledTask::RunOnce);
      scheduler_->ScheduleTask(diskSpaceStartupTask);

      std::shared_ptr<DiskSpaceMonitorTask> diskSpaceTask = std::shared_ptr<DiskSpaceMonitorTask>(new DiskSpaceMonitorTask);
      diskSpaceTask->SetReoccurance(ScheduledTask::RunInfinitely);
      diskSpaceTask->SetMinutesBetweenRun(60);
      scheduler_->ScheduleTask(diskSpaceTask);

      std::shared_ptr<WorkQueueHealthTask> workQueueHealthTask = std::shared_ptr<WorkQueueHealthTask>(new WorkQueueHealthTask);
      workQueueHealthTask->SetReoccurance(ScheduledTask::RunInfinitely);
      workQueueHealthTask->SetMinutesBetweenRun(1);
      scheduler_->ScheduleTask(workQueueHealthTask);

      // Alerts. Registered unconditionally and every minute, because it is the
      // ONLY thing that sends: everything that notices a condition writes a row
      // and returns, so that a notification about the delivery queue is never
      // submitted into the delivery queue by the thread that is stuck on it.
      // A minute is the resolution a cool-down and a digest hour need; with
      // AlertsEnabled off, or with every rule off, each pass is three selects
      // that find nothing.
      std::shared_ptr<AlertTask> alertTask = std::shared_ptr<AlertTask>(new AlertTask);
      alertTask->SetReoccurance(ScheduledTask::RunInfinitely);
      alertTask->SetMinutesBetweenRun(1);
      scheduler_->ScheduleTask(alertTask);

      // Outbound webhooks, and registered for the same reason: the paths that
      // notice an event write a row and return, and this is the only thing that
      // POSTs - so a webhook about a delivery is never sent by the thread doing
      // the delivering. A minute is the scheduler's finest tick; an administrator
      // who has just configured an endpoint does not wait for it, because
      // POST /api/v1/webhooks/run makes the same pass at once. With no
      // subscription configured - which is every installation until somebody
      // makes one - each minute is two selects that find nothing: the pass's
      // due rows, and the retention sweep's count.
      std::shared_ptr<WebhookTask> webhookTask = std::shared_ptr<WebhookTask>(new WebhookTask);
      webhookTask->SetReoccurance(ScheduledTask::RunInfinitely);
      webhookTask->SetMinutesBetweenRun(1);
      scheduler_->ScheduleTask(webhookTask);

      // Inbound webhooks (Util/InboundWebhooks.h), the other direction: mail for
      // a routed address is queued by the delivery path and POSTed from here -
      // and from a drain posted when it is queued, so that an endpoint hears of
      // it in seconds - and the minute is what catches the retries. With no
      // route configured each minute is two selects that find nothing, the
      // pass's due rows and the retention sweep's count, as for the outbound
      // webhooks; and the routes themselves are read at most twice a minute
      // while mail for a hosted domain is arriving.
      std::shared_ptr<InboundWebhookTask> inboundWebhookTask = std::shared_ptr<InboundWebhookTask>(new InboundWebhookTask);
      inboundWebhookTask->SetReoccurance(ScheduledTask::RunInfinitely);
      inboundWebhookTask->SetMinutesBetweenRun(1);
      scheduler_->ScheduleTask(inboundWebhookTask);

      // DKIM keys that rotate. Registered unconditionally and twice, like log
      // retention: once at start-up, so a record published while the server was
      // down is looked at straight away, and every fifteen minutes after. A pass
      // with no domain's schedule on and nothing in flight is two selects - see
      // DkimRotationTask.
      // Web Push. Registered unconditionally and every minute, like the alerts, and
      // for the same reason: it is the only thing that sends. Delivery notes that
      // mail arrived and returns, and a drain is posted to the asynchronous queue
      // at once, so this minute is the floor that catches retries and anything the
      // drain could not do - with the feature off, each pass is one property read
      // and nothing else. See Common/Util/PushNotifier.h.
      std::shared_ptr<PushTask> pushTask = std::shared_ptr<PushTask>(new PushTask);
      pushTask->SetReoccurance(ScheduledTask::RunInfinitely);
      pushTask->SetMinutesBetweenRun(1);
      scheduler_->ScheduleTask(pushTask);

      std::shared_ptr<DkimRotationTask> dkimRotationStartupTask = std::shared_ptr<DkimRotationTask>(new DkimRotationTask);
      dkimRotationStartupTask->SetReoccurance(ScheduledTask::RunOnce);
      scheduler_->ScheduleTask(dkimRotationStartupTask);

      std::shared_ptr<DkimRotationTask> dkimRotationTask = std::shared_ptr<DkimRotationTask>(new DkimRotationTask);
      dkimRotationTask->SetReoccurance(ScheduledTask::RunInfinitely);
      dkimRotationTask->SetMinutesBetweenRun(15);
      scheduler_->ScheduleTask(dkimRotationTask);

      // The reputation watch. Registered unconditionally and hourly; the period it
      // keeps is a day, measured from the rows the last check wrote, so a server
      // restarted twice an hour still checks once a day and one restarted weekly
      // checks within the hour - see ReputationWatchTask. A wake with nothing to
      // check is one select and no lookup.
      std::shared_ptr<ReputationWatchTask> reputationWatchTask = std::shared_ptr<ReputationWatchTask>(new ReputationWatchTask);
      reputationWatchTask->SetReoccurance(ScheduledTask::RunInfinitely);
      reputationWatchTask->SetMinutesBetweenRun(60);
      scheduler_->ScheduleTask(reputationWatchTask);

      // Scheduled backups. Only registered when a schedule is configured in
      // hMailServer.ini ([Settings] ScheduledBackupTime or
      // ScheduledBackupIntervalMinutes), so a default installation gets no backup
      // task at all and behaves exactly as every release before this one did.
      //
      // Registered on a one-minute tick rather than at the backup interval, and that
      // is deliberate. The task only decides whether a run is owed - it hands the run
      // itself to the maintenance work queue - so the tick is cheap, and both
      // schedule modes need a fine-grained clock. A daily 02:00 backup cannot be
      // expressed as SetMinutesBetweenRun(1440): ScheduledTask::SetNextRunTime
      // computes the next run as "now plus the interval" every time the scheduler is
      // created, so a restart at 09:00 would quietly move the backup window to 09:00
      // the following day, and a server restarted every morning would move it every
      // day. The task compares against the wall clock instead, and recovers the time
      // of the last backup from the destination.
      //
      // Not also registered as a RunOnce task at startup, unlike log retention and
      // the consistency check. Those are cheap and idempotent; a backup is neither,
      // and starting one during startup - while the connection pool and the work
      // queues are still being built - is the one moment it should not happen. A
      // backup that is genuinely owed is taken on the first tick a minute later,
      // which is also when the task first reads the destination.
      if (BackupScheduleTask::IsEnabled())
         ScheduleBackups_(scheduler_);

      // Unattended directory synchronisation. Registered only when [LDAP] is switched
      // on AND SyncScheduleMinutes is set, so a default installation - and every
      // installation that upgrades into this build - gets no task at all and no
      // account is ever created without somebody having asked for it.
      //
      // Registered twice, like log retention and the consistency check: once promptly
      // at start-up, then at the configured interval. The start-up run is what makes
      // the schedule survive restarts. ScheduledTask computes its next run as "now
      // plus the interval" each time the scheduler is built, so a daily interval on a
      // server that is restarted every morning would otherwise never fire once - the
      // trap BackupScheduleTask avoids by ticking every minute against the wall clock.
      // A synchronisation is cheap and idempotent, so it can buy the same property
      // much more simply, by just running one.
      const int directorySyncMinutes = DirectorySyncScheduleTask::ScheduleMinutes();

      if (directorySyncMinutes > 0)
      {
         std::shared_ptr<DirectorySyncScheduleTask> directorySyncStartupTask =
            std::shared_ptr<DirectorySyncScheduleTask>(new DirectorySyncScheduleTask);
         directorySyncStartupTask->SetReoccurance(ScheduledTask::RunOnce);
         scheduler_->ScheduleTask(directorySyncStartupTask);

         std::shared_ptr<DirectorySyncScheduleTask> directorySyncTask =
            std::shared_ptr<DirectorySyncScheduleTask>(new DirectorySyncScheduleTask);
         directorySyncTask->SetReoccurance(ScheduledTask::RunInfinitely);
         directorySyncTask->SetMinutesBetweenRun(directorySyncMinutes);
         scheduler_->ScheduleTask(directorySyncTask);

         LOG_APPLICATION(Formatter::Format("DirectorySync: unattended synchronisation is scheduled every {0} "
            "minute(s). It creates and updates accounts; it does not disable them.", directorySyncMinutes));
      }

      // Automatic certificate renewal via ACME (Let's Encrypt).
      if (IniFileSettings::Instance()->GetAcmeEnabled())
      {
         // Once an hour...
         std::shared_ptr<AcmeRenewalTask> acmeTask = std::shared_ptr<AcmeRenewalTask>(new AcmeRenewalTask);
         acmeTask->SetReoccurance(ScheduledTask::RunInfinitely);
         acmeTask->SetMinutesBetweenRun(60);
         scheduler_->ScheduleTask(acmeTask);

         // ...and immediately at startup. Queued last: a RunOnce task goes straight
         // onto the maintenance queue, and this one can end in a restart of the
         // servers (a certificate issued but never deployed is deployed by it),
         // which must not race the scheduling still going on here.
         std::shared_ptr<AcmeRenewalTask> acmeStartupTask = std::shared_ptr<AcmeRenewalTask>(new AcmeRenewalTask);
         acmeStartupTask->SetReoccurance(ScheduledTask::RunOnce);
         scheduler_->ScheduleTask(acmeStartupTask);
      }

   }

   String
   Application::StopServers(int maxWaitSeconds)
   {
      boost::unique_lock<boost::timed_mutex> lock(servers_mutex_, boost::defer_lock);
      const String refused = TakeServersLock_(lock, *MessageIndexer::Instance(), maxWaitSeconds);
      ServersLockHeld_ held(*this, lock);
      if (!refused.IsEmpty())
      {
         LOG_APPLICATION("The servers are not stopped: " + refused);
         return refused;
      }

      StopServers_(StopToStartAgain, *MessageIndexer::Instance());
      return "";
   }

   bool
   Application::ShutDown()
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // The stop before the process ends (Application.h). Until 10 October 2026
   // TerminateServer and the Linux server's exit called StopServers and
   // ExitInstance as they stand, with nothing between them and a reinitialise
   // on another thread - COM's, the REST API's, a backup restore's or an ACME
   // deployment's: StopServers found the servers stopping and returned at once,
   // and ExitInstance closed the database under the reinitialise, which then
   // opened it again or faulted on it, and under the indexer's thread still in
   // its statement.
   //---------------------------------------------------------------------------()
   {
      // What comes after this - a reinitialise queued behind the one under way,
      // a REST request's, COM's Start - is refused, and a reinitialise under way
      // stops short after its stop.
      shutting_down_ = true;

      const std::function<void()> progress = StopProgress_();

      boost::unique_lock<boost::timed_mutex> lock(servers_mutex_, boost::try_to_lock);
      ServersLockHeld_ held(*this, lock);
      if (!lock.owns_lock())
      {
         LOG_APPLICATION("Stopping: a start, a stop or a reinitialise of the servers is under way, and this stop waits for it.");

         // What that may wait for without a limit is the indexer's thread, which
         // a reinitialise's stop waits for until it has ended: waited for here,
         // for the minute this stop would give it. It ends at its next
         // interruption point, both waits return, and the reinitialise stops
         // short. A thread still in its statement after the minute is left to
         // the process's end, with the reinitialise waiting for it - the
         // database closed first but for what is in use, as after a stop of
         // this one's own (CloseDatabaseBeforeExit_), so that what was
         // committed is not lost with the process.
         if (!MessageIndexer::Instance()->Stop(progress, IndexerWaitBeforeExitSeconds))
            return CloseDatabaseBeforeExit_(false, true);

         // The rest of it is bounded - the drain, the sandbox's poller (a minute
         // at most), the work queues (ten seconds each), a start that was under
         // way - and is waited for as long as a stop of this one's own could
         // take, in turns of a second with the service controller told.
         const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() +
            std::chrono::seconds(IniFileSettings::Instance()->GetShutdownDrainSeconds() + 120);
         while (!lock.try_lock_for(boost::chrono::seconds(1)))
         {
            if (progress)
               progress();

            if (std::chrono::steady_clock::now() >= deadline)
            {
               LOG_APPLICATION("Gave up waiting for the start, the stop or the reinitialise of the servers that was under way. The process ends with it.");
               return CloseDatabaseBeforeExit_(true, true);
            }
         }

         held.Taken();
      }

      const bool indexerEnded = StopServers_(StopBeforeExit, *MessageIndexer::Instance());
      return ExitInstance_(StopBeforeExit, indexerEnded);
   }

   String
   Application::TakeServersLock_(boost::unique_lock<boost::timed_mutex> &lock, MessageIndexer &indexer, int maxWaitSeconds, LockPurpose purpose)
   {
      // Not waited for: the stop before the process ends holds the mutex or
      // waits for it, and what follows it is refused anyway (below). A script's
      // call on one of a work queue's threads that waited here for that stop
      // would hold the thread the stop waits for.
      if (shutting_down_)
         return "the server is stopping.";

      // A script that the stop, the start or the reinitialise holding the mutex
      // runs on its own thread - OnError, for an error it reports - asking for
      // another: it would wait for the mutex its own thread holds.
      if (servers_mutex_holder_.load() == std::this_thread::get_id())
         return "a stop, a start or a reinitialise of the servers is under way on this thread, which asked for this from a script it runs.";

      // A reinitialise asked on the message indexer's own thread - by an OnError
      // script the indexer runs, for an error it reports - is refused whatever
      // holds the mutex, before the mutex is asked for, so that it is answered
      // alike either way and nothing is touched. Its stop of the servers ends
      // with the indexer's Stop, which on the indexer's own thread can only ask
      // that thread to end at its next interruption point, and that point is
      // the reinitialise's own next wait: the join of the maintenance queue's
      // threads in ExitInstance_. From 10 October 2026, with the mutex free, the
      // reinitialise was let through and ended there, in boost's
      // thread_interrupted, with the servers stopped, initialized_ false - every
      // COM getter refused - and that queue stopped but still registered, so
      // that a COM Start ran the servers without their scheduled tasks; before
      // that the Stop asked the thread to join itself, which boost refuses, and
      // the servers were left stopping. Taking the request back before those
      // waits would not make it safe: the database's close after them
      // disconnects the connections in use, among them the one the indexer's
      // statement may still hold further up this thread
      // (DatabaseConnectionManager::Disconnect). A stop or a start asked there
      // is made when the mutex is free (MessageIndexer::LockServersUnlessOnItsThread):
      // the stop's request to the indexer is the last thing it does, and the
      // indexer ends at its next interruption point - in what the script asks
      // for next, or once the script has returned - unless a start on that
      // thread takes the request back first (MessageIndexer::Start).
      if (purpose == LockToReinitialise && indexer.IsItsOwnThread())
         return "a reinitialise asked on the message indexer's own thread - by an event script the indexer runs, OnError for an error it reported - is not made there: its stop cannot wait for the thread it runs on, and the database it would close is the one the indexer's statement may be using. The servers go on as they are.";

      // An event script's call waits a few seconds at most, whatever its
      // caller asked (ComServersWaitSeconds): the stop, the start or the
      // reinitialise holding the mutex may be joining the thread the script
      // runs on without a limit - an OnError script's Application.Reinitialize
      // on the sandbox's poller, which a stop joins however long its request
      // takes - and the two would wait for each other for ever. From 10
      // October 2026, when COM's Stop (the Control Panel's Pause) began to
      // hold the mutex, until the re-check of that change, they did; before
      // it, a reinitialise's own lock did the same to another reinitialise.
      const bool fromScript = ScriptServer::IsScriptRunningOnThisThread();
      const int scriptWaitSeconds = ComServersWaitSeconds;
      int waitSeconds = maxWaitSeconds;
      if (fromScript && (waitSeconds <= 0 || waitSeconds > scriptWaitSeconds))
         waitSeconds = scriptWaitSeconds;

      if (!indexer.LockServersUnlessOnItsThread(lock, waitSeconds))
      {
         if (fromScript)
            return Formatter::Format("a stop, a start or a reinitialise of the servers is under way, which a call from an event script waits {0} seconds at most for, since it may be waiting for the thread the script runs on: try again once it has ended.", waitSeconds);

         if (waitSeconds > 0)
            return Formatter::Format("a stop, a start or a reinitialise of the servers is under way, which this waits {0} seconds at most for - not at all on the message indexer's own thread, which it may be waiting for: try again once it has ended.", waitSeconds);

         return "a stop, a start or a reinitialise of the servers is under way, and it may be waiting for the message indexer, on whose thread this was asked.";
      }

      // Begun while this waited.
      if (shutting_down_)
      {
         lock.unlock();
         return "the server is stopping.";
      }

      return "";
   }

   Application::ServersLockHeld_::ServersLockHeld_(Application &application, boost::unique_lock<boost::timed_mutex> &lock) :
      application_(application),
      lock_(lock)
   {
      if (lock_.owns_lock())
         Taken();
   }

   void
   Application::ServersLockHeld_::Taken()
   {
      application_.servers_mutex_holder_ = std::this_thread::get_id();
   }

   Application::ServersLockHeld_::~ServersLockHeld_()
   {
      if (lock_.owns_lock())
         application_.servers_mutex_holder_ = std::thread::id();
   }

   void
   Application::SetStopProgressCallback(std::function<void()> callback)
   {
      boost::lock_guard<boost::mutex> guard(stop_progress_mutex_);
      stop_progress_ = callback;
   }

   std::function<void()>
   Application::StopProgress_() const
   {
      boost::lock_guard<boost::mutex> guard(stop_progress_mutex_);
      return stop_progress_;
   }

   bool
   Application::StopServers_(StopPurpose purpose, MessageIndexer &indexer)
   {
      LOG_APPLICATION("Stopping servers...")

      start_time_ = "";

      // A copy: TerminateServer sets the callback on the service controller's
      // thread, and this stop may be a reinitialise's on another, which held a
      // reference to the member while the callback was set and reset.
      const std::function<void()> progress = StopProgress_();

      // A stop from Running or from Draining, and from nothing else. The state
      // moves to Stopping under the drain lock, so that a drain arriving in the
      // same instant sees Stopping and is refused rather than both of them
      // acting on Running.
      bool wasDraining = false;
      bool serving = false;
      {
         boost::lock_guard<boost::recursive_mutex> guard(drain_mutex_);

         int state = ServerStatus::Instance()->GetState();
         if (state == ServerStatus::StateRunning || state == ServerStatus::StateDraining)
         {
            serving = true;
            wasDraining = state == ServerStatus::StateDraining;
            ServerStatus::Instance()->SetState(ServerStatus::StateStopping);
            drain_since_ = "";

            // The doors first. Until this row a stop kept accepting through the
            // whole drain window and the delivery manager kept handing out queued
            // messages, so a client that connected during the drain was accepted,
            // counted and then cut, and an outbound session could be cut mid-DATA.
            // Now the listeners close and the queue pauses before anything is
            // waited for - and a drain that was entered earlier has done this.
            if (!wasDraining)
            {
               if (!CloseTheDoors_())
                  LOG_APPLICATION("Drain: the listeners are NOT known to be closed (the io threads did not answer); stopping anyway.");
            }
            else
            {
               // Maintenance mode ends here, whoever stops the servers - a service
               // stop, or a reinitialise from SIGHUP, the Control Panel, the REST
               // route, a backup restore or an ACME deployment - and it says so,
               // on the audit trail as well: the operator who entered it recorded
               // "entered", and a "left" with no actor of theirs is the honest
               // record of a drain that a restart ended.
               LOG_APPLICATION("Drain: ended by a stop of the servers (a service stop or a reinitialise); the listeners were already closed.");
               RecordDrainLeftByTheServer_(_T("Ended by a stop of the servers - a service stop or a reinitialise - with the listeners already closed."));
            }
         }
      }

      if (!serving)
      {
         // Stopped already - by COM's Stop, by a reinitialise that stopped short
         // for ShutDown, or never started - and the indexer's thread may still be
         // ending, after a stop made on that thread: waited for as below, so that
         // a database closed after this is not closed under it.
         LOG_DEBUG("Application::~StopServers() - E1");
         return indexer.Stop(progress, purpose == StopBeforeExit ? IndexerWaitBeforeExitSeconds : 0);
      }

      // Migration jobs first: each writes its counters and its resume marks and
      // stops between two commands, and its own connection to a source on this
      // server is then not a session the drain below waits for.
      MigrationManager::Instance()->Stop();

      // Graceful drain: give the sessions and the deliveries in flight a bounded
      // window to finish before we tear the servers down. The state is already
      // StateStopping, so /readyz reports 503 and external load balancers stop
      // routing new traffic (the metrics listener is intentionally kept up until
      // the drain completes). ShutdownDrainSeconds <= 0 (the default) disables
      // the wait entirely; the doors are closed either way. What has already
      // drained is not waited for again: a stop after maintenance mode finds
      // nothing in flight and goes straight on.
      int drainSeconds = IniFileSettings::Instance()->GetShutdownDrainSeconds();
      if (drainSeconds > 0)
      {
         DrainStatus inFlight = GetDrainStatus();
         if (inFlight.sessions > 0 || inFlight.deliveries > 0)
         {
            LOG_APPLICATION(Formatter::Format("Drain: listeners closed; {0} session(s) and {1} delivery(ies) in flight; up to {2} seconds.", inFlight.sessions, inFlight.deliveries, drainSeconds));

            ULONGLONG deadline = ::GetTickCount64() + (ULONGLONG)drainSeconds * 1000;
            int polls = 0;
            while (::GetTickCount64() < deadline)
            {
               inFlight = GetDrainStatus();
               if (inFlight.sessions <= 0 && inFlight.deliveries <= 0)
                  break;

               // About once a second: the service controller's checkpoint, so
               // that a stop honouring a long window is seen to be progressing.
               if (progress && (++polls % 5) == 0)
                  progress();

               Sleep(200);
            }

            inFlight = GetDrainStatus();
            if (inFlight.sessions > 0 || inFlight.deliveries > 0)
            {
               LOG_APPLICATION(Formatter::Format("Graceful shutdown: drain window elapsed with {0} session(s) and {1} delivery(ies) still active; stopping anyway.", inFlight.sessions, inFlight.deliveries));
            }
            else
            {
               LOG_APPLICATION("Graceful shutdown: all sessions and deliveries drained.");
            }
         }
         else if (wasDraining)
         {
            LOG_APPLICATION("Graceful shutdown: already drained; stopping now.");
         }
      }

      // The sandbox's poller, before anything it reads or writes goes: it asks
      // the database and an HTTP service, and puts held messages back in the
      // queue. What it holds stays held and is polled again after the start.
      // Joined, however long a request in flight takes to end (a minute at
      // most), with the service controller's checkpoint meanwhile.
      Sandbox::Stop(progress);

      if (metrics_server_)
      {
         metrics_server_->Stop();
         metrics_server_.reset();
      }

      if (snmp_agent_)
      {
         snmp_agent_->Stop();
         snmp_agent_.reset();
      }

      // Stop the OpenTelemetry exporters (each flushes what it has queued). The
      // log exporter goes last of the three so the others' shutdown log lines
      // still reach a collector.
      // Before the OpenTelemetry exporters, so that their own shutdown lines
      // still reach a collector: this one stops taking entries first and then
      // flushes what it holds on the connection it already has.
      SyslogSink::Instance()->Stop();

      OtelTracer::Instance()->Stop();
      OtelMetricsExporter::Instance()->Stop();
      OtelLogExporter::Instance()->Stop();

      if (rest_api_server_)
      {
         rest_api_server_->Stop();
         rest_api_server_.reset();
      }

      if (web_services_server_)
      {
         web_services_server_->Stop();
         web_services_server_.reset();
      }

      if (manage_sieve_server_)
      {
         manage_sieve_server_->Stop();
         manage_sieve_server_.reset();
      }

      LOG_DEBUG("Application::StopServers() - Removing server work queue");
      // Then remove the main server.
      WorkQueueManager::Instance()->RemoveQueue(server_work_queue_);

      OnServerStopped();

      // Before the io_context goes, and after the acceptors have: no new session can
      // arrive from here, and every task still holding one is joined while the
      // socket it owns is still attached to a context that exists. This is the step
      // whose absence made a slow reverse-DNS lookup fault the process on shutdown -
      // CreateSessionWorkQueues_ has the detail.
      LOG_DEBUG("Application::StopServers() - Draining session work queues");
      RemoveSessionWorkQueues_();

      // Deinitialize servers. This lets go of io_service_; it is freed here unless
      // a queue whose stop gave up on a thread still keeps it - the IOCP queue, a
      // session queue - in which case it is freed after that queue, once the
      // thread has left (WorkQueueManager::SweepRetiredQueues), and not under it.
      // The next start makes its own; the listeners of this one are closed.
      LOG_DEBUG("Application::StopServers() - Destructing IOCP");
      if (io_service_) io_service_.reset();
      LOG_DEBUG("Application::StopServers() - Destructing DeliveryManager");
      if (smtp_delivery_manager_) smtp_delivery_manager_.reset();
      LOG_DEBUG("Application::StopServers() - Destructing FetchManager");
      if (external_fetch_manager_) external_fetch_manager_.reset();
      LOG_DEBUG("Application::StopServers() - Destructing Scheduler");
      // The two task handles a saved setting reaches go with the scheduler that
      // holds the tasks, under the lock the subscribers copy them under. The
      // scheduler itself is let go of after the lock: the last reference may be
      // this one.
      std::shared_ptr<Scheduler> droppedScheduler;
      {
         boost::lock_guard<boost::mutex> handles(scheduler_handles_mutex_);
         greylist_cleaner_task_.reset();
         backup_schedule_task_.reset();
         droppedScheduler = scheduler_;
         scheduler_.reset();
      }
      droppedScheduler.reset();

      // With the scheduled tasks, as when the sweep was one of them: the keys
      // left open stay open until the next start's sweep, or the process's end.
      MailboxKeys::StopIdleSweep();

      LOG_DEBUG("Application::StopServers() - Destructing Rest");
      if (notification_server_) notification_server_.reset();
      if (folder_manager_) folder_manager_.reset();

      // Joined, with the service controller's checkpoint meanwhile: a Reinitialize
      // closes the database as soon as this returns (ExitInstance), and until 10
      // October 2026 the indexer was interrupted and not waited for, so that a pass
      // in the middle of a batch made its next statement through the empty database
      // manager - which on Linux ended the process (MessageIndexer::Stop). Until it
      // has ended when the servers may start again. Before the process ends, a
      // minute at most: twice DatabaseStatementTimeout's default, which ends a
      // statement on most engines, and past that the indexer is in a statement a
      // database that has stopped answering holds - a MySQL write, which no
      // statement timeout ends there, or any statement with the timeout at 0 or on
      // a connection gone silent. Until 10 October 2026 such a stop waited for it
      // without end while the service controller was told it was progressing. The
      // stop now goes on and answers false: ShutDown closes every connection but
      // the one the thread's statement is on, which it leaves open - waiting out
      // SQL Server Compact's flush interval first - and the process ends at once,
      // the thread with it, as it does when a work queue's stop above has given
      // up on one of its threads (CloseDatabaseBeforeExit_,
      // WorkQueue::GetThreadsLeft). Until 10 October 2026 this comment said the
      // process's end cut the statement, but the database was closed under the
      // thread first, and the process ended through the destructors of statics
      // with the thread still running; and so it was under a work queue's thread
      // that a stop had given up on.
      const bool indexerEnded = indexer.Stop(progress, purpose == StopBeforeExit ? IndexerWaitBeforeExitSeconds : 0);

      ServerStatus::Instance()->SetState(ServerStatus::StateStopped);

      LOG_APPLICATION("Servers stopped.")

      return indexerEnded;
   }

   bool
   Application::CloseTheDoors_()
   {
      bool closed = true;

      if (io_service_ && !io_service_->StopAccepting())
         closed = false;

      // The ManageSieve listener is its own accept loop, not one of IOService's
      // acceptors, and kept accepting through the drain until 23 September 2026.
      if (manage_sieve_server_ && !manage_sieve_server_->StopAccepting())
         closed = false;

      if (smtp_delivery_manager_)
         smtp_delivery_manager_->Pause();

      return closed;
   }

   bool
   Application::IsDraining() const
   {
      return ServerStatus::Instance()->GetState() == ServerStatus::StateDraining;
   }

   Application::DrainStatus
   Application::GetDrainStatus()
   {
      DrainStatus status;
      status.draining = IsDraining();

      // Copied under the lock: EnterDrain and LeaveDrain write it on their
      // threads, and a String read while it is written is a torn read.
      {
         boost::lock_guard<boost::recursive_mutex> guard(drain_mutex_);
         status.since = status.draining ? drain_since_ : String();
      }

      status.sessions = SessionManager::Instance()->GetNumberOfConnections();

      // Copies of the pointers: StopServers resets the members on another thread.
      std::shared_ptr<ManageSieveServer> manageSieve = manage_sieve_server_;
      if (manageSieve)
         status.sessions += manageSieve->GetNumberOfSessions();

      std::shared_ptr<SMTPDeliveryManager> deliveryManager = smtp_delivery_manager_;
      status.deliveries = deliveryManager ? deliveryManager->GetDeliveriesInFlight() : 0;

      status.drained = status.draining && status.sessions <= 0 && status.deliveries <= 0;
      return status;
   }

   String
   Application::EnterDrain()
   {
      boost::lock_guard<boost::recursive_mutex> guard(drain_mutex_);

      int state = ServerStatus::Instance()->GetState();
      if (state == ServerStatus::StateDraining)
         return "already draining";
      if (state != ServerStatus::StateRunning)
         return "not running";

      ServerStatus::Instance()->SetState(ServerStatus::StateDraining);
      drain_since_ = Time::GetCurrentDateTime();

      const bool closed = CloseTheDoors_();

      DrainStatus inFlight = GetDrainStatus();
      LOG_APPLICATION(Formatter::Format("Drain: {0}; {1} session(s) and {2} delivery(ies) in flight; until the drain is left.",
         closed ? String("listeners closed") : String("the listeners are NOT known to be closed (the io threads did not answer)"),
         inFlight.sessions, inFlight.deliveries));

      return "";
   }

   String
   Application::LeaveDrain(std::vector<String> &notReopened)
   {
      boost::lock_guard<boost::recursive_mutex> guard(drain_mutex_);
      notReopened.clear();

      if (ServerStatus::Instance()->GetState() != ServerStatus::StateDraining)
         return "not draining";

      // The doors before the queue: a listener that cannot be reopened keeps
      // the state at Draining, and a resumed queue under a drain would be a
      // drain in name only.
      if (io_service_ && !io_service_->StartAccepting(notReopened))
      {
         // The io threads did not answer, so nothing is known to be open. The
         // flag is back to closed (StartAccepting did that), the state stays
         // Draining, and the caller answers 500 with the reason: a Running
         // declared over closed ports would be a state nobody could repair
         // without a restart, and a 200 would be a lie.
         LOG_APPLICATION("Drain: not left - the listeners could not be reopened; the state stays draining.");
         return "the listeners could not be reopened";
      }

      if (manage_sieve_server_ && !manage_sieve_server_->StartAccepting())
         notReopened.push_back(Formatter::Format("{0}:{1} (ManageSieve)", manage_sieve_server_->GetBindAddress(), manage_sieve_server_->GetPort()));

      if (smtp_delivery_manager_)
         smtp_delivery_manager_->Resume();

      drain_since_ = "";
      ServerStatus::Instance()->SetState(ServerStatus::StateRunning);

      if (notReopened.empty())
      {
         LOG_APPLICATION("Drain: left; the listeners are open and delivery has resumed.");
      }
      else
      {
         String list;
         for (size_t i = 0; i < notReopened.size(); i++)
         {
            if (i > 0)
               list += _T(", ");
            list += notReopened[i];
         }

         // High, not Critical: the server is running and the other listeners are
         // open. The acceptor has already said which port and why (HM4316).
         ErrorManager::Instance()->ReportError(ErrorManager::High, 7150, "Application::LeaveDrain",
            Formatter::Format("The drain was left, but the listener(s) on {0} could not be reopened - something else took the port while the listeners were closed; the error log names it (HM4316). The other listeners are open and delivery has resumed. A restart of the service tries every port again.", list));
      }

      return "";
   }

   void
   Application::RecordDrainLeftByTheServer_(const String &detail)
   {
      AuditTrail::Actor actor;
      actor.present = true;
      actor.name = _T("server");
      actor.kind = _T("system");
      actor.interface_name = _T("server");
      AuditScope scope(actor);
      AuditTrail::Instance()->RecordEvent(_T("server"), _T("drain"), _T("left"),
         detail);
   }

   String
   Application::LeaveDrainAsTheServer(const String &reason)
   {
      std::vector<String> notReopened;
      String refusal = LeaveDrain(notReopened);
      if (!refusal.IsEmpty())
      {
         LOG_APPLICATION("Drain: the server tried to leave the drain (" + reason + ") and could not: " + refusal);
         return refusal;
      }

      LOG_APPLICATION("Drain: left by the server. " + reason);
      RecordDrainLeftByTheServer_(reason);
      return "";
   }

   String
   Application::Reinitialize()
   {
      return ReinitializeWith_(*MessageIndexer::Instance());
   }

   String
   Application::ReinitializeWith_(MessageIndexer &indexer)
   {
      // One restart at a time: see the member's comment in Application.h. A
      // second caller waits for the first to finish and then restarts again,
      // which is what it asked for - unless it asks on the message indexer's
      // own thread, or the process is ending (TakeServersLock_).
      boost::unique_lock<boost::timed_mutex> lock(servers_mutex_, boost::defer_lock);
      const String refused = TakeServersLock_(lock, indexer, 0, LockToReinitialise);
      ServersLockHeld_ held(*this, lock);
      if (!refused.IsEmpty())
      {
         // In the application log as well, as a refused start or stop is: an
         // event script, the caller on the indexer's own thread, meets the
         // answer as an error, which reaches the error log only when the script
         // does not handle it, and nothing else said that the server went on
         // without the reinitialise.
         const String answer = "The server was not reinitialised: " + refused;
         LOG_APPLICATION(answer);
         return answer;
      }

      return Reinitialize_(indexer);
   }

   String
   Application::Reinitialize_(MessageIndexer &indexer)
   {
      StopServers_(StopToStartAgain, indexer);

      // A stop before the process ends came while this one stopped, and waits
      // for it (ShutDown): that stop closes the database, and nothing is
      // started again in a process that is ending.
      if (shutting_down_)
         return "The server was not started again: it is stopping.";

      ExitInstance();

      // The reconciled [Settings] map belongs to the database connection that has
      // just been closed, and InitInstance's first LoadSettings is meant to be the
      // file-only bootstrap. Leaving the previous cycle's map in place would make
      // that first load read through a mirror of a database this process is no
      // longer connected to - which matters most in the one case Reinitialize
      // exists for, restoring a backup, where the rows are about to change.
      IniFileSettings::Instance()->ForgetDatabaseSettings();

      // The identity providers' signing keys, cached for an hour by JwksKeySet,
      // are forgotten with the rest: a restart is when an administrator has
      // pointed the server at a different issuer, or the fixtures at a fresh
      // key under the kid the last one used, and a key remembered across it
      // would refuse every token the new issuer signs for the hour.
      JwksKeySet::Instance()->Clear();

      // The IP warm-up's cached schedules and counts (SMTP/IpWarmup.h) are
      // read again from the tables: a restore may have put other rows there,
      // and a count read back is how a restart is proved not to reset a day.
      IpWarmup::Instance()->Forget();

      // The eDiscovery runner's flag outlives the maintenance queue ExitInstance
      // has just taken away, and a runner that queue never started, or one its
      // stop cut off, would otherwise keep every later search and export queued
      // (Util/DiscoverySearch.h). The rows it left running are failed when the
      // runner next starts.
      DiscoverySearch::Forget();

      // The inbound webhook routes are read again - a restore may have put the
      // database back under them - and a drain posted to the asynchronous queue
      // ExitInstance has just taken away is forgotten, since it will never run
      // and would otherwise keep every later message from posting one
      // (Util/InboundWebhooks.h).
      InboundWebhooks::Instance()->Forget();

      // The second factor's wrong-code counts and locks (Util/Totp.h), which
      // the COM sign-in and the REST API share: a restart is the administrator's
      // way to end a lock, as a service restart is. The steps already taken are
      // kept, so that no code is taken a second time across it.
      SecondFactorGuard::ClearBudgets();

      String sErrorMessage;
      if (!InitInstance(sErrorMessage))
         return sErrorMessage;

      // And one that came while this closed the database and opened it again:
      // nothing is started, and that stop closes the database just opened. One
      // that comes after this waits for the start and stops what it started.
      // Not looked at between the two: the stop's ExitInstance would then run
      // straight after this one's, with no InitInstance between them, and let
      // go of what InitInstance makes a second time (OutOfMemoryHandler).
      if (shutting_down_)
         return "The server was not started again: it is stopping.";

      if (!StartServers_())
         return "Failed to start servers";

      return "";
   }

   void
   Application::SubmitPendingEmail()
   {
      LOG_DEBUG("Requesting SMTPDeliveryManager to start message delivery");

      if (smtp_delivery_manager_)
         smtp_delivery_manager_->SetDeliverMessage();
      else
         // We could not notify the SMTP delivery manager, since it does not exit.
         ErrorManager::Instance()->ReportError(ErrorManager::High, 4219, "Application::SubmitPendingEmail", "Could not notify SMTP deliverer about new message, since SMTP deliverer does not exist. The operation requires the SMTP server to be on.");
   }

   std::shared_ptr<WorkQueue>
   Application::GetMaintenanceWorkQueue()
   {
      std::shared_ptr<WorkQueue> pWorkQueue =
         WorkQueueManager::Instance()->GetQueue(maintenance_queue_);
      
      if (!pWorkQueue)
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5118, "Application::GetMaintenanceWorkQueue()", "Maintenance work queue not available.");

      return pWorkQueue;

      
   }

   std::shared_ptr<WorkQueue>
   Application::GetAsyncWorkQueue()
   {
      std::shared_ptr<WorkQueue> pAsynchQueue = WorkQueueManager::Instance()->GetQueue(asynchronous_tasks_queue_);

      if (!pAsynchQueue)
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5118, "Application::GetAsyncWorkQueue()", "Async work queue not available.");

      return pAsynchQueue;
   }

   std::shared_ptr<WorkQueue>
   Application::GetNameLookupWorkQueue()
   {
      std::shared_ptr<WorkQueue> pQueue = WorkQueueManager::Instance()->GetQueue(name_lookup_queue_);

      if (!pQueue)
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5118, "Application::GetNameLookupWorkQueue()", "Name lookup work queue not available.");

      return pQueue;
   }

   int 
   Application::GetUniqueID()
   {
      int iResult = InterlockedIncrement(&unique_id_);

      return iResult;
   }

   void
   Application::OnPropertyChanged(std::shared_ptr<Property> pProperty)
   {
      if (ServerStatus::Instance()->GetState() == ServerStatus::StateStopped)
         return;

      String sPropertyName = pProperty->GetName();

      if (sPropertyName == PROPERTY_MAX_NUMBER_OF_ASYNC_TASKS)
      {
         std::shared_ptr<WorkQueue> pWorkQueue = GetAsyncWorkQueue();

         if (!pWorkQueue)
            return;

         pWorkQueue->SetMaxSimultaneous(pProperty->GetLongValue());
      }
   }

   std::shared_ptr<NotificationServer> 
   Application::GetNotificationServer()
   {
      return notification_server_;
   }

   std::shared_ptr<FolderManager>
   Application::GetFolderManager()
   {
      return folder_manager_;
   }

   namespace
   {
      // What a thread of ServersLockTester's answers, and when, for the test's
      // own thread to wait for with a limit: a thread that waited for ever
      // would otherwise hang the test. Shared, so that a thread the test gives
      // up on and leaves to the process's end has what it writes to.
      struct ServersLockAnswers
      {
         std::mutex lock;
         std::condition_variable changed;

         bool began = false;
         bool holding = false;
         bool let_go = false;
         bool done = false;

         String taking;
         String reinitialised;
         String stopped;
         bool started = true;
         bool took = false;
         bool shut_down = false;

         template <typename Predicate>
         bool WaitFor(std::chrono::seconds within, Predicate predicate)
         {
            std::unique_lock<std::mutex> guard(lock);
            return changed.wait_for(guard, within, predicate);
         }

         template <typename Change>
         void Set(Change change)
         {
            {
               std::lock_guard<std::mutex> guard(lock);
               change();
            }
            changed.notify_all();
         }
      };

      // Long enough for a thread to be made and begin, and for a free mutex
      // to be taken, however slowly the tester runs - under an emulator for
      // another processor, as the aarch64 builds run the testers - and never
      // waited out when it passes.
      const std::chrono::seconds ServersLockWait(60);
   }

   void
   ServersLockTester::Test()
   {
      TestAskedOnTheThreadThatHoldsIt_();
      TestAskedOnTheIndexersThreadWhileAStopWaits_();
      TestAReinitialiseOnTheIndexersThreadIsRefused_();
   }

   void
   ServersLockTester::TestTheStopBeforeExit()
   {
      // The servers stopped, as they are in business-object-tests, so that the
      // stop before the process ends finds nothing to stop; with nothing
      // started, its ExitInstance lets go of nothing either. The process's
      // ending is taken back after each test, and the state put back.
      Application &application = *Application::Instance();

      // Free, unless a thread that Test gave up on, waiting for ever, holds it:
      // what follows would wait for it too.
      {
         boost::unique_lock<boost::timed_mutex> probe(application.servers_mutex_, boost::defer_lock);
         if (!probe.try_lock_for(boost::chrono::seconds(20)))
            throw std::logic_error("ServersLockTester: the servers' mutex was still held twenty seconds after the tests before this one, by a thread one of them left waiting for ever.");
      }

      const ServerStatus::ServerState state = (ServerStatus::ServerState) ServerStatus::Instance()->GetState();
      ServerStatus::Instance()->SetState(ServerStatus::StateStopped);

      try
      {
         TestRefusedOnceTheStopBeforeExitHasBegun_();
         application.shutting_down_ = false;

         TestTheStopBeforeExitWaitsForOneUnderWay_();
         application.shutting_down_ = false;

         TestAWorkQueuesThreadLeftEndsTheProcessWhileItRuns_();
         application.shutting_down_ = false;

         TestAConnectionInUseIsLeftOpenAndTheRestClosed_();
         application.shutting_down_ = false;

         TestComsStopAndStartWaitAMomentAtMost_();
         application.shutting_down_ = false;

         TestAScriptsCallWaitsAMomentAtMost_();
         application.shutting_down_ = false;

         TestTheServicesStartTakesTheLockFirst_();
         application.shutting_down_ = false;
      }
      catch (...)
      {
         application.shutting_down_ = false;
         ServerStatus::Instance()->SetState(state);
         throw;
      }

      ServerStatus::Instance()->SetState(state);
   }

   void
   ServersLockTester::TestAskedOnTheThreadThatHoldsIt_()
   {
      Application &application = *Application::Instance();
      std::shared_ptr<ServersLockAnswers> answers = std::make_shared<ServersLockAnswers>();

      // A thread that takes the mutex as a stop or a start takes it, and then,
      // as a script that stop or start runs would - OnError, for an error it
      // reports - asks for a reinitialise, a stop and a start.
      boost::thread holder([&application, answers]()
      {
         String taking;
         String reinitialised;
         String stopped;
         bool started = true;
         {
            boost::unique_lock<boost::timed_mutex> lock(application.servers_mutex_, boost::defer_lock);
            taking = application.TakeServersLock_(lock, *MessageIndexer::Instance());
            Application::ServersLockHeld_ held(application, lock);
            answers->Set([&]() { answers->holding = true; });

            if (taking.IsEmpty())
            {
               reinitialised = application.Reinitialize();
               stopped = application.StopServers();
               started = application.StartServers();
            }
         }

         answers->Set([&]()
         {
            answers->taking = taking;
            answers->reinitialised = reinitialised;
            answers->stopped = stopped;
            answers->started = started;
            answers->done = true;
         });
      });

      // In a server, a stop, a start or a reinitialise of its own may hold the
      // mutex for a moment first.
      if (!answers->WaitFor(ServersLockWait, [&answers]() { return answers->holding; }))
      {
         holder.detach();
         throw std::logic_error("ServersLockTester: the servers' mutex was not free for the test within a minute.");
      }

      if (!answers->WaitFor(std::chrono::seconds(20), [&answers]() { return answers->done; }))
      {
         // Waiting for the mutex it holds itself, for ever: left to the
         // process's end, with what it writes to.
         holder.detach();
         throw std::logic_error("ServersLockTester: a reinitialise, a stop or a start asked on the thread that holds the servers' mutex - by a script that the stop or the start it is making runs - had not returned after twenty seconds: it waited for the mutex its own thread holds, as it would have for ever.");
      }

      holder.join();

      if (!answers->taking.IsEmpty())
         throw std::logic_error("ServersLockTester: the servers' mutex was refused to the test: the server is stopping.");

      const String onThisThread = _T("under way on this thread");
      if (!answers->reinitialised.Contains(onThisThread))
         throw std::logic_error("ServersLockTester: a reinitialise asked on the thread that holds the servers' mutex was not refused as asked there.");

      if (!answers->stopped.Contains(onThisThread))
         throw std::logic_error("ServersLockTester: a stop asked on the thread that holds the servers' mutex was not refused as asked there.");

      if (answers->started)
         throw std::logic_error("ServersLockTester: a start asked on the thread that holds the servers' mutex answered that it started the servers.");
   }

   void
   ServersLockTester::TestAskedOnTheIndexersThreadWhileAStopWaits_()
   {
      Application &application = *Application::Instance();
      std::shared_ptr<ServersLockAnswers> answers = std::make_shared<ServersLockAnswers>();

      // The indexer's pass, standing in, with no database: once a stop has
      // asked it to end, it asks on its own thread for the servers' mutex, as
      // an OnError script's reinitialise, stop or start does for an error the
      // indexer reports, and ends at the interruption point after that. Until
      // then it makes none, so that the request it sees is the stop's.
      MessageIndexer *standIn = nullptr;
      boost::function<void()> pass = [&application, answers, &standIn]()
      {
         answers->Set([&]() { answers->began = true; });

         for (;;)
         {
            if (boost::this_thread::interruption_requested())
            {
               String taking;
               bool took = false;
               {
                  boost::unique_lock<boost::timed_mutex> lock(application.servers_mutex_, boost::defer_lock);
                  taking = application.TakeServersLock_(lock, *standIn);
                  Application::ServersLockHeld_ held(application, lock);
                  took = lock.owns_lock();
               }

               answers->Set([&]()
               {
                  answers->taking = taking;
                  answers->took = took;
                  answers->done = true;
               });

               boost::this_thread::interruption_point();
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(10));
         }
      };

      MessageIndexer indexer(pass);
      standIn = &indexer;

      indexer.Start();
      if (!answers->WaitFor(ServersLockWait, [&answers]() { return answers->began; }))
         throw std::logic_error("ServersLockTester: the stand-in indexer's thread began no pass within a minute of Start.");

      // Held here, through the same call, as a reinitialise holds the mutex
      // while its stop waits for the indexer. That stop has no limit; this one
      // has, so that the two waiting for each other ends the test rather than
      // hangs it.
      String taking;
      bool ended = false;
      {
         boost::unique_lock<boost::timed_mutex> lock(application.servers_mutex_, boost::defer_lock);
         taking = application.TakeServersLock_(lock, *MessageIndexer::Instance());
         Application::ServersLockHeld_ held(application, lock);

         if (taking.IsEmpty())
            ended = indexer.Stop(std::function<void()>(), 20);
      }

      if (!taking.IsEmpty())
         throw std::logic_error("ServersLockTester: the servers' mutex was refused to the test: the server is stopping.");

      if (!ended)
      {
         indexer.Stop();
         throw std::logic_error("ServersLockTester: a stop that held the servers' mutex waited twenty seconds for the stand-in indexer, whose own thread asked for that mutex - as an OnError script's reinitialise does while another reinitialise waits for the indexer - and the two waited for each other, as they would have for ever.");
      }

      if (!answers->WaitFor(std::chrono::seconds(0), [&answers]() { return answers->done; }))
         throw std::logic_error("ServersLockTester: the stand-in indexer's thread ended without asking for the servers' mutex.");

      if (answers->took || !answers->taking.Contains(_T("on whose thread this was asked")))
         throw std::logic_error("ServersLockTester: the servers' mutex, held by a stop waiting for the message indexer, was not refused on the indexer's own thread.");
   }

   namespace
   {
      // A task that notes that it ran, which a work queue does at once unless it
      // has been stopped, when it turns every task away.
      class NotedTask : public Task
      {
      public:
         explicit NotedTask(const std::shared_ptr<std::atomic<bool>> &ran) :
            Task("ServersLockTester's noted task"),
            ran_(ran)
         {
         }

      protected:
         void DoWork() override
         {
            ran_->store(true);
         }

      private:
         std::shared_ptr<std::atomic<bool>> ran_;
      };

      // Whether queue runs a task within the limit.
      bool RunsATask(const std::shared_ptr<WorkQueue> &queue, std::chrono::seconds within)
      {
         std::shared_ptr<std::atomic<bool>> ran = std::make_shared<std::atomic<bool>>(false);
         queue->AddTask(std::make_shared<NotedTask>(ran));

         const std::chrono::steady_clock::time_point until = std::chrono::steady_clock::now() + within;
         while (!ran->load() && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));

         return ran->load();
      }

      // What TestAReinitialiseOnTheIndexersThreadIsRefused_ asks the stand-in
      // indexer's own thread to make, one ask at a time while the test waits,
      // and what that thread answers.
      struct OwnThreadAsks
      {
         enum Ask
         {
            AskNothing,
            AskReinitialise,
            AskStopOrStart
         };

         std::mutex lock;
         std::condition_variable changed;

         bool began = false;
         Ask ask = AskNothing;
         int asked = 0;
         int made = 0;

         // The last ask's answer: what it returned, what it threw instead,
         // whether a stop's or a start's take of the mutex took it, and whether
         // the thread had been asked to end afterwards.
         String answer;
         String thrown;
         bool took = false;
         bool interruption_after = false;
         bool stop_asked_after = false;

         template <typename Predicate>
         bool WaitFor(std::chrono::seconds within, Predicate predicate)
         {
            std::unique_lock<std::mutex> guard(lock);
            return changed.wait_for(guard, within, predicate);
         }

         template <typename Change>
         void Set(Change change)
         {
            {
               std::lock_guard<std::mutex> guard(lock);
               change();
            }
            changed.notify_all();
         }

         // Asks for what, and waits for the answer: false when none came.
         bool Make(Ask what, std::chrono::seconds within)
         {
            int number = 0;
            Set([&]() { ask = what; number = ++asked; });
            return WaitFor(within, [&]() { return made == number; });
         }
      };
   }

   void
   ServersLockTester::TestAReinitialiseOnTheIndexersThreadIsRefused_()
   {
      Application &application = *Application::Instance();
      std::shared_ptr<OwnThreadAsks> asks = std::make_shared<OwnThreadAsks>();

      // What a reinitialise reaches first after its stop: initialized_, and the
      // maintenance queue whose threads ExitInstance_ joins before anything else.
      // In a server (TestInternals) they are the server's own, read and left as
      // they are; in business-object-tests, which has neither, a queue of the
      // test's under that name, and initialized_ set, for as long as it runs.
      std::shared_ptr<WorkQueue> queue = WorkQueueManager::Instance()->GetQueue(application.maintenance_queue_);
      const bool standsIn = !queue;
      const bool initializedBefore = application.initialized_;
      if (standsIn)
      {
         WorkQueueManager::Instance()->CreateWorkQueue(5, application.maintenance_queue_);
         queue = WorkQueueManager::Instance()->GetQueue(application.maintenance_queue_);
         application.initialized_ = true;
      }

      // Put back however the test ends, once the stand-in below has ended. A
      // stop of the queue the system refuses may not leave a destructor: the
      // process ends, as it would have, said here where it happens.
      struct PutBack
      {
         Application &application;
         const bool standsIn;
         const bool initialized;

         ~PutBack()
         {
            if (!standsIn)
               return;

            try
            {
               WorkQueueManager::Instance()->RemoveQueue(application.maintenance_queue_);
            }
            catch (...)
            {
               std::terminate();
            }

            application.initialized_ = initialized;
         }
      } putBack{application, standsIn, initializedBefore};

      if (!queue)
         throw std::logic_error("ServersLockTester: the test's maintenance queue was not made.");

      const bool initialized = application.initialized_;

      // The indexer's pass, standing in, with no database: what the test asks
      // for it makes on its own thread, counted as an event script's run, as an
      // OnError script the indexer runs for an error it reports would make it,
      // and then says whether its own thread has been asked to end. Between two
      // asks it makes an interruption point, as the indexer's loop does.
      MessageIndexer *standIn = nullptr;
      boost::function<void()> pass = [&application, asks, &standIn]()
      {
         asks->Set([&]() { asks->began = true; });

         for (;;)
         {
            int number = 0;
            OwnThreadAsks::Ask ask = OwnThreadAsks::AskNothing;
            {
               std::unique_lock<std::mutex> guard(asks->lock);
               asks->changed.wait_for(guard, std::chrono::milliseconds(10), [&asks]() { return asks->asked > asks->made; });
               if (asks->asked > asks->made)
               {
                  number = asks->asked;
                  ask = asks->ask;
               }
            }

            if (ask == OwnThreadAsks::AskNothing)
            {
               boost::this_thread::interruption_point();
               continue;
            }

            String answer;
            String thrown;
            bool took = false;
            {
               ScriptServer::ScriptRunOnThisThread script;
               try
               {
                  if (ask == OwnThreadAsks::AskReinitialise)
                  {
                     // COM's Application.Reinitialize, as such a script calls it.
                     answer = application.ReinitializeWith_(*standIn);
                  }
                  else
                  {
                     // A stop's or a start's take of the mutex, let go of at once,
                     // so that nothing is stopped or started. In a server its own
                     // stop, start or reinitialise - a setting's save among them -
                     // may hold the mutex for a moment, which on this thread is
                     // refused rather than waited for: asked again until it is
                     // free, a minute at most.
                     const std::chrono::steady_clock::time_point until = std::chrono::steady_clock::now() + ServersLockWait;
                     for (;;)
                     {
                        boost::unique_lock<boost::timed_mutex> lock(application.servers_mutex_, boost::defer_lock);
                        answer = application.TakeServersLock_(lock, *standIn, Application::ComServersWaitSeconds);
                        Application::ServersLockHeld_ held(application, lock);
                        took = lock.owns_lock();
                        if (took || std::chrono::steady_clock::now() >= until)
                           break;

                        std::this_thread::sleep_for(std::chrono::milliseconds(25));
                     }
                  }
               }
               catch (boost::thread_interrupted &)
               {
                  // What COM's catch-all answered as an unknown error.
                  thrown = _T("boost::thread_interrupted");
               }
               catch (std::exception &error)
               {
                  thrown = String(AnsiString(error.what()));
               }
               catch (...)
               {
                  thrown = _T("an exception of another type");
               }
            }

            const bool interruption = boost::this_thread::interruption_requested();
            bool stopAsked = false;
            {
               boost::lock_guard<boost::mutex> guard(standIn->starterMutex_);
               stopAsked = standIn->stop_asked_;
            }

            asks->Set([&]()
            {
               asks->answer = answer;
               asks->thrown = thrown;
               asks->took = took;
               asks->interruption_after = interruption;
               asks->stop_asked_after = stopAsked;
               asks->made = number;
            });
         }
      };

      MessageIndexer indexer(pass);
      standIn = &indexer;

      indexer.Start();
      if (!asks->WaitFor(ServersLockWait, [&asks]() { return asks->began; }))
         throw std::logic_error("ServersLockTester: the stand-in indexer's thread began no pass within a minute of Start.");

      const String refusedThere = _T("on the message indexer's own thread");

      // An OnError script's Application.Reinitialize on the indexer's own thread,
      // with the mutex free. Until the release review of 6.3.7 (CORE-1) it was
      // let through: its stop asked the indexer's own thread to end, and the
      // join of the maintenance queue's threads after it met that request.
      if (!asks->Make(OwnThreadAsks::AskReinitialise, ServersLockWait))
         throw std::logic_error("ServersLockTester: a reinitialise asked on the stand-in indexer's own thread, with the servers' mutex free, had not returned within a minute.");

      String answer;
      String thrown;
      bool askedToEnd = false;
      {
         std::lock_guard<std::mutex> guard(asks->lock);
         answer = asks->answer;
         thrown = asks->thrown;
         askedToEnd = asks->interruption_after || asks->stop_asked_after;
      }

      const bool queueKept = WorkQueueManager::Instance()->GetQueue(application.maintenance_queue_) == queue;
      const bool initializedKept = application.initialized_ == initialized;

      if (!thrown.IsEmpty())
         throw std::logic_error(std::string("ServersLockTester: a reinitialise asked on the message indexer's own thread, with the servers' mutex free - by an OnError script the indexer runs - was made there and ended part way, in ") + AnsiString(thrown).c_str() + ": its stop asked the indexer's own thread to end, and the join of the maintenance queue's threads after it met that request, leaving the servers stopped, the server unconfigured and that queue stopped but registered, as from 10 October 2026 until the release review of 6.3.7.");

      if (!answer.Contains(refusedThere))
         throw std::logic_error(std::string("ServersLockTester: a reinitialise asked on the message indexer's own thread, with the servers' mutex free, was not refused as one asked there; it answered: ") + AnsiString(answer).c_str());

      if (!initializedKept)
         throw std::logic_error("ServersLockTester: a reinitialise asked on the message indexer's own thread changed whether the server is initialised, and every COM getter would have been refused.");

      if (!queueKept || !RunsATask(queue, ServersLockWait))
         throw std::logic_error("ServersLockTester: a reinitialise asked on the message indexer's own thread removed the maintenance queue, or left it turning every task away - stopped but still registered, so that a start after it ran the servers without their scheduled tasks.");

      if (askedToEnd)
         throw std::logic_error("ServersLockTester: a reinitialise asked on the message indexer's own thread asked that thread to end.");

      // And with the mutex held - here, as a reinitialise on another thread holds
      // it while its stop waits for the indexer - the same refusal, at once,
      // rather than the one for a mutex held by another after a script's few
      // seconds.
      {
         boost::unique_lock<boost::timed_mutex> lock(application.servers_mutex_, boost::defer_lock);
         const String taking = application.TakeServersLock_(lock, *MessageIndexer::Instance());
         Application::ServersLockHeld_ held(application, lock);
         if (!taking.IsEmpty())
            throw std::logic_error("ServersLockTester: the servers' mutex was refused to the test: the server is stopping.");

         const std::chrono::steady_clock::time_point before = std::chrono::steady_clock::now();
         if (!asks->Make(OwnThreadAsks::AskReinitialise, ServersLockWait))
            throw std::logic_error("ServersLockTester: a reinitialise asked on the stand-in indexer's own thread, with the servers' mutex held, had not returned within a minute.");

         const std::chrono::steady_clock::duration waited = std::chrono::steady_clock::now() - before;
         {
            std::lock_guard<std::mutex> guard(asks->lock);
            answer = asks->answer;
            thrown = asks->thrown;
         }

         if (!thrown.IsEmpty() || !answer.Contains(refusedThere))
            throw std::logic_error(std::string("ServersLockTester: a reinitialise asked on the message indexer's own thread, with the servers' mutex held, was not refused as one asked there; it answered: ") + AnsiString(thrown.IsEmpty() ? answer : thrown).c_str());

         // (A copy: chrono takes the constant by reference.)
         const int limit = Application::ComServersWaitSeconds;
         if (waited >= std::chrono::seconds(limit))
            throw std::logic_error("ServersLockTester: a reinitialise asked on the message indexer's own thread, with the servers' mutex held, was refused only after the few seconds a script's call waits for the mutex.");
      }

      // A stop or a start asked there, with the mutex free, takes it: those are
      // refused only while another holds it.
      if (!asks->Make(OwnThreadAsks::AskStopOrStart, ServersLockWait + ServersLockWait))
         throw std::logic_error("ServersLockTester: a stop's or a start's take of the servers' mutex on the stand-in indexer's own thread had not returned within two minutes.");

      bool took = false;
      {
         std::lock_guard<std::mutex> guard(asks->lock);
         answer = asks->answer;
         thrown = asks->thrown;
         took = asks->took;
         askedToEnd = asks->interruption_after || asks->stop_asked_after;
      }

      if (!thrown.IsEmpty() || !took || !answer.IsEmpty())
         throw std::logic_error(std::string("ServersLockTester: a stop or a start asked on the message indexer's own thread, with the servers' mutex free, was refused; it answered: ") + AnsiString(thrown.IsEmpty() ? answer : thrown).c_str());

      if (askedToEnd)
         throw std::logic_error("ServersLockTester: a stop's or a start's take of the servers' mutex on the message indexer's own thread asked that thread to end.");

      indexer.Stop();
   }

   void
   ServersLockTester::TestRefusedOnceTheStopBeforeExitHasBegun_()
   {
      Application &application = *Application::Instance();

      // As ShutDown sets it first.
      application.shutting_down_ = true;

      const String reinitialised = application.Reinitialize();
      const String stopped = application.StopServers();

      boost::unique_lock<boost::timed_mutex> lock(application.servers_mutex_, boost::defer_lock);
      const String taking = application.TakeServersLock_(lock, *MessageIndexer::Instance());
      const bool took = lock.owns_lock();
      if (took)
         lock.unlock();

      if (!reinitialised.Contains(_T("the server is stopping")))
         throw std::logic_error("ServersLockTester: a reinitialise asked once the stop before the process ends had begun was not refused: one queued behind a reinitialise under way started the servers again in a process that was ending.");

      if (!stopped.Contains(_T("the server is stopping")))
         throw std::logic_error("ServersLockTester: a stop asked once the stop before the process ends had begun was not refused.");

      if (took || !taking.Contains(_T("the server is stopping")))
         throw std::logic_error("ServersLockTester: the servers' mutex was taken for a start asked once the stop before the process ends had begun.");

      boost::unique_lock<boost::timed_mutex> after(application.servers_mutex_, boost::try_to_lock);
      if (!after.owns_lock())
         throw std::logic_error("ServersLockTester: a refused reinitialise or stop left the servers' mutex held.");
   }

   void
   ServersLockTester::TestTheStopBeforeExitWaitsForOneUnderWay_()
   {
      Application &application = *Application::Instance();
      std::shared_ptr<ServersLockAnswers> answers = std::make_shared<ServersLockAnswers>();

      // A reinitialise under way on a thread of its own: the mutex held until
      // the test lets it go - a minute at most.
      boost::thread reinitialise([&application, answers]()
      {
         boost::unique_lock<boost::timed_mutex> lock(application.servers_mutex_, boost::defer_lock);
         const String taking = application.TakeServersLock_(lock, *MessageIndexer::Instance());
         Application::ServersLockHeld_ held(application, lock);
         answers->Set([&]() { answers->taking = taking; answers->holding = true; });

         answers->WaitFor(ServersLockWait, [&answers]() { return answers->let_go; });
      });

      if (!answers->WaitFor(ServersLockWait, [&answers]() { return answers->holding; }) || !answers->taking.IsEmpty())
      {
         answers->Set([&]() { answers->let_go = true; });
         reinitialise.join();
         throw std::logic_error("ServersLockTester: the servers' mutex was not taken for the reinitialise the test stands in for.");
      }

      // The stop before the process ends, on a thread of its own: the
      // service's stop, a SIGTERM's.
      boost::thread stop([&application, answers]()
      {
         const bool ended = application.ShutDown();
         answers->Set([&]() { answers->shut_down = ended; answers->done = true; });
      });

      // It waits for the reinitialise, however long that holds the mutex, the
      // indexer's thread apart, which it stops itself: there is none here.
      const bool returnedEarly = answers->WaitFor(std::chrono::seconds(2), [&answers]() { return answers->done; });

      // And a reinitialise asked meanwhile - a REST request's, queued behind
      // the one under way - is refused, whichever of the two takes the mutex
      // when it is let go of.
      // Since the stop has begun, at once: waited for, it would hold its thread
      // until the stop let go of the mutex.
      std::shared_ptr<String> queuedAnswer = std::make_shared<String>();
      std::shared_ptr<std::atomic<bool>> queuedReturned = std::make_shared<std::atomic<bool>>(false);
      boost::thread queued([&application, queuedAnswer, queuedReturned]()
      {
         *queuedAnswer = application.Reinitialize();
         queuedReturned->store(true);
      });

      bool refusedAtOnce = false;
      for (int i = 0; i < 200 && !refusedAtOnce; i++)
      {
         std::this_thread::sleep_for(std::chrono::milliseconds(25));
         refusedAtOnce = queuedReturned->load();
      }

      answers->Set([&]() { answers->let_go = true; });
      reinitialise.join();

      const bool returned = answers->WaitFor(std::chrono::seconds(20), [&answers]() { return answers->done; });
      if (returned)
         stop.join();
      else
         stop.detach();

      const bool queuedJoined = queued.try_join_for(boost::chrono::seconds(20));
      if (!queuedJoined)
         queued.detach();

      if (returnedEarly)
         throw std::logic_error("ServersLockTester: the stop before the process ends returned while a reinitialise held the servers' mutex: it went straight on and closed the database under the reinitialise, as a service stop or a SIGTERM did until 10 October 2026.");

      if (!returned)
         throw std::logic_error("ServersLockTester: the stop before the process ends had not returned twenty seconds after the reinitialise it waited for let go of the servers' mutex.");

      if (!answers->shut_down)
         throw std::logic_error("ServersLockTester: the stop before the process ends, with no thread left running, answered that one was, and the database would have been left open.");

      if (!queuedJoined)
         throw std::logic_error("ServersLockTester: a reinitialise asked while the stop before the process ends waited had not returned twenty seconds after the mutex was let go of.");

      if (!queuedAnswer->Contains(_T("the server is stopping")))
         throw std::logic_error("ServersLockTester: a reinitialise asked while the stop before the process ends waited was not refused: queued behind the one under way, it would have started the servers again in a process that was ending.");

      if (!refusedAtOnce)
         throw std::logic_error("ServersLockTester: a reinitialise asked while the stop before the process ends waited was refused only once the mutex was let go of: it waited for it first, as a script's call on a thread that stop waits for would.");
   }

   void
   ServersLockTester::TestComsStopAndStartWaitAMomentAtMost_()
   {
      Application &application = *Application::Instance();
      std::shared_ptr<ServersLockAnswers> answers = std::make_shared<ServersLockAnswers>();

      // A reinitialise under way, as in the test before: the mutex held until
      // the test lets it go, a minute at most.
      boost::thread reinitialise([&application, answers]()
      {
         boost::unique_lock<boost::timed_mutex> lock(application.servers_mutex_, boost::defer_lock);
         const String taking = application.TakeServersLock_(lock, *MessageIndexer::Instance());
         Application::ServersLockHeld_ held(application, lock);
         answers->Set([&]() { answers->taking = taking; answers->holding = true; });

         answers->WaitFor(ServersLockWait, [&answers]() { return answers->let_go; });
      });

      if (!answers->WaitFor(ServersLockWait, [&answers]() { return answers->holding; }) || !answers->taking.IsEmpty())
      {
         answers->Set([&]() { answers->let_go = true; });
         reinitialise.join();
         throw std::logic_error("ServersLockTester: the servers' mutex was not taken for the reinitialise the test stands in for.");
      }

      // COM's Stop and Start, each on a thread of its own, as a Control
      // Panel's or a script's.
      struct ComCall
      {
         std::atomic<bool> returned{false};
         String answer;
         bool started = true;
         std::chrono::steady_clock::duration took{};
      };
      std::shared_ptr<ComCall> stop = std::make_shared<ComCall>();
      std::shared_ptr<ComCall> start = std::make_shared<ComCall>();

      boost::thread stopper([&application, stop]()
      {
         const std::chrono::steady_clock::time_point before = std::chrono::steady_clock::now();
         stop->answer = application.StopServers(Application::ComServersWaitSeconds);
         stop->took = std::chrono::steady_clock::now() - before;
         stop->returned.store(true);
      });

      boost::thread starter([&application, start]()
      {
         const std::chrono::steady_clock::time_point before = std::chrono::steady_clock::now();
         start->started = application.StartServers(Application::ComServersWaitSeconds, &start->answer);
         start->took = std::chrono::steady_clock::now() - before;
         start->returned.store(true);
      });

      // While the reinitialise still holds the mutex.
      const std::chrono::steady_clock::time_point until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
      while ((!stop->returned.load() || !start->returned.load()) && std::chrono::steady_clock::now() < until)
         std::this_thread::sleep_for(std::chrono::milliseconds(25));

      const bool bothReturned = stop->returned.load() && start->returned.load();

      // A call that still waits finds the process ending once it takes the
      // mutex, and so stops and starts nothing.
      application.shutting_down_ = true;
      answers->Set([&]() { answers->let_go = true; });
      reinitialise.join();

      const bool joined = stopper.try_join_for(boost::chrono::seconds(20)) && starter.try_join_for(boost::chrono::seconds(20));
      application.shutting_down_ = false;
      if (!joined)
      {
         stopper.detach();
         starter.detach();
      }

      if (!bothReturned)
         throw std::logic_error("ServersLockTester: COM's Stop or Start had not returned twenty seconds into a reinitialise under way: it waited for the reinitialise, whose stop waits for the message indexer without a limit, as it did from 10 October 2026 until the limit of a few seconds.");

      if (!joined)
         throw std::logic_error("ServersLockTester: COM's Stop or Start had not ended twenty seconds after the reinitialise let go of the mutex.");

      const String underWay = _T("under way");
      if (!stop->answer.Contains(underWay))
         throw std::logic_error("ServersLockTester: COM's Stop during a reinitialise under way was not refused as one under way.");

      if (start->started || !start->answer.Contains(underWay))
         throw std::logic_error("ServersLockTester: COM's Start during a reinitialise under way was not refused as one under way.");

      // Waited for a moment first, so that a reinitialise about to end is not
      // refused: the limit, give or take the clock's grain.
      // (A copy: the constant is declared in the class, and chrono takes it by reference.)
      const int limit = Application::ComServersWaitSeconds;
      const std::chrono::steady_clock::duration atLeast = std::chrono::seconds(limit) - std::chrono::milliseconds(500);
      if (stop->took < atLeast || start->took < atLeast)
         throw std::logic_error("ServersLockTester: COM's Stop or Start during a reinitialise under way was refused without waiting the few seconds it waits.");
   }

   void
   ServersLockTester::TestTheServicesStartTakesTheLockFirst_()
   {
      Application &application = *Application::Instance();
      std::shared_ptr<ServersLockAnswers> answers = std::make_shared<ServersLockAnswers>();

      // A stop holds the mutex - the one before the process ends, which the
      // service takes from the moment it reports itself running - until the
      // test lets it go.
      boost::thread stop([&application, answers]()
      {
         boost::unique_lock<boost::timed_mutex> lock(application.servers_mutex_, boost::defer_lock);
         const String taking = application.TakeServersLock_(lock, *MessageIndexer::Instance());
         Application::ServersLockHeld_ held(application, lock);
         answers->Set([&]() { answers->taking = taking; answers->holding = true; });

         answers->WaitFor(ServersLockWait, [&answers]() { return answers->let_go; });
      });

      if (!answers->WaitFor(ServersLockWait, [&answers]() { return answers->holding; }) || !answers->taking.IsEmpty())
      {
         answers->Set([&]() { answers->let_go = true; });
         stop.join();
         throw std::logic_error("ServersLockTester: the servers' mutex was not taken for the stop the test stands in for.");
      }

      // The service's start, on a thread of its own as ServiceMain's is. It
      // waits for the mutex before InitInstance; the stop it waited for has
      // ended the process's servers when it lets go (shutting_down_), so the
      // start is refused there and opens nothing - in business-object-tests,
      // which has no database, an InitInstance would fail on the way, saying
      // something else.
      std::shared_ptr<String> startAnswer = std::make_shared<String>();
      std::shared_ptr<std::atomic<bool>> startReturned = std::make_shared<std::atomic<bool>>(false);
      std::shared_ptr<std::atomic<bool>> finishCalled = std::make_shared<std::atomic<bool>>(false);
      boost::thread start([&application, startAnswer, startReturned, finishCalled]()
      {
         String error;
         application.InitInstanceAndStart(error, [finishCalled]() { finishCalled->store(true); return false; });
         *startAnswer = error;
         startReturned->store(true);
      });

      std::this_thread::sleep_for(std::chrono::seconds(1));
      const bool returnedEarly = startReturned->load();

      application.shutting_down_ = true;
      answers->Set([&]() { answers->let_go = true; });
      stop.join();

      const bool joined = start.try_join_for(boost::chrono::seconds(20));
      if (!joined)
         start.detach();

      if (returnedEarly)
         throw std::logic_error("ServersLockTester: the service's start returned while a stop held the servers' mutex: it went on to InitInstance without it, and a stop arriving then closed the database under InitInstance, as the installer's stop after its start for the database step could until 10 October 2026.");

      if (!joined)
         throw std::logic_error("ServersLockTester: the service's start had not returned twenty seconds after the stop let go of the servers' mutex.");

      if (!startAnswer->Contains(_T("the server is stopping")) || finishCalled->load())
         throw std::logic_error("ServersLockTester: the service's start, which waited for a stop before the process ends, was not refused once that stop let go of the servers' mutex, before it opened anything.");
   }

   namespace
   {
      // A task that makes no interruption point until the test lets it go - a
      // delivery, a script or a lookup in a database or a host that has stopped
      // answering - so that its queue's stop gives up on it.
      class HeldTask : public Task
      {
      public:
         explicit HeldTask(const std::shared_ptr<ServersLockAnswers> &answers) :
            Task("ServersLockTester's held task"),
            answers_(answers)
         {
         }

      protected:
         void DoWork() override
         {
            answers_->Set([this]() { answers_->began = true; });
            answers_->WaitFor(std::chrono::seconds(120), [this]() { return answers_->let_go; });
         }

      private:
         std::shared_ptr<ServersLockAnswers> answers_;
      };

      // The process's database manager stood in for by the test's, and put
      // back however the test ends.
      class StandInDatabase
      {
      public:
         StandInDatabase(std::shared_ptr<DatabaseConnectionManager> &slot, boost::mutex &guard,
            const std::shared_ptr<DatabaseConnectionManager> &standIn) :
            slot_(slot),
            guard_(guard)
         {
            boost::lock_guard<boost::mutex> lock(guard_);
            replaced_ = slot_;
            slot_ = standIn;
         }

         // The stand-in let go of after the lock, as Application lets go of a
         // manager. A lock the system refuses would leave the process's
         // manager the test's stand-in, and an exception may not leave a
         // destructor: the process ends, as it would have, said here where it
         // happens.
         ~StandInDatabase()
         {
            try
            {
               std::shared_ptr<DatabaseConnectionManager> standIn;
               {
                  boost::lock_guard<boost::mutex> lock(guard_);
                  standIn.swap(slot_);
                  slot_ = replaced_;
               }
            }
            catch (...)
            {
               std::terminate();
            }
         }

         StandInDatabase(const StandInDatabase &) = delete;
         StandInDatabase &operator=(const StandInDatabase &) = delete;

      private:
         std::shared_ptr<DatabaseConnectionManager> &slot_;
         boost::mutex &guard_;
         std::shared_ptr<DatabaseConnectionManager> replaced_;
      };
   }

   void
   ServersLockTester::TestAWorkQueuesThreadLeftEndsTheProcessWhileItRuns_()
   {
      Application &application = *Application::Instance();
      std::shared_ptr<ServersLockAnswers> answers = std::make_shared<ServersLockAnswers>();

      // The database stood in for by one idle connection, which nothing uses.
      std::vector<std::shared_ptr<DALConnection>> connections;
      std::shared_ptr<DatabaseConnectionManager> database = DatabaseConnectionManagerTester::MakeManagerOfStandIns(1, connections);
      StandInDatabase standIn(application.db_manager_, application.managers_mutex_, database);

      const int before = WorkQueue::GetThreadsLeft();
      int left = before;
      bool endedWhileHeld = true;
      bool keptWhileHeld = false;
      bool closedWhileHeld = false;
      bool noLongerCounted = false;
      bool endedAfter = false;
      bool letGoOfAfter = false;
      {
         WorkQueue queue(1, "ServersLockTester's queue");

         // The task let go of before the queue goes, however this scope ends,
         // so that the queue's destructor joins its thread.
         struct LetGo
         {
            std::shared_ptr<ServersLockAnswers> answers;
            ~LetGo() { answers->Set([this]() { answers->let_go = true; }); }
         } letGo{answers};

         queue.Start();
         queue.AddTask(std::make_shared<HeldTask>(answers));

         if (!answers->WaitFor(ServersLockWait, [&answers]() { return answers->began; }))
            throw std::logic_error("ServersLockTester: the work queue's thread began no task within a minute.");

         // Ten seconds, and it gives up.
         queue.Stop();
         left = WorkQueue::GetThreadsLeft();

         endedWhileHeld = application.ShutDown();
         application.shutting_down_ = false;
         keptWhileHeld = application.GetDBManager() == database;
         closedWhileHeld = !connections[0]->IsConnected();

         // The thread ends once its task is let go of, and is no longer counted
         // from then on - with the queue still here, nothing having joined it.
         answers->Set([&answers]() { answers->let_go = true; });
         const std::chrono::steady_clock::time_point until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
         for (;;)
         {
            noLongerCounted = WorkQueue::GetThreadsLeft() == before;
            if (noLongerCounted || std::chrono::steady_clock::now() >= until)
               break;

            std::this_thread::sleep_for(std::chrono::milliseconds(25));
         }

         // With nothing left, the stop ends the process the ordinary way.
         if (noLongerCounted)
         {
            endedAfter = application.ShutDown();
            application.shutting_down_ = false;
            letGoOfAfter = !application.GetDBManager();
         }
      }

      if (left != before + 1)
         throw std::logic_error("ServersLockTester: a thread a work queue's stop gave up on, still running, was not counted (WorkQueue::GetThreadsLeft).");

      if (endedWhileHeld)
         throw std::logic_error("ServersLockTester: the stop before the process ends answered that nothing was left running while a work queue's stop had given up on a thread, and the process would have ended through the destructors of statics under it.");

      if (!keptWhileHeld)
         throw std::logic_error("ServersLockTester: the stop before the process ends let go of the database manager while a thread a work queue's stop gave up on still ran: a statement it made next would have gone through none.");

      if (!closedWhileHeld)
         throw std::logic_error("ServersLockTester: the stop before the process ends left a database connection that nothing used open while a thread a work queue's stop gave up on still ran: on SQL Server Compact, what was committed since its last write would have been lost with the process.");

      if (!noLongerCounted)
         throw std::logic_error("ServersLockTester: a thread a work queue's stop gave up on was still counted twenty seconds after it had ended, nothing having joined it: from 10 October 2026 until the re-check of that change every later stop of the process took it for one still running.");

      if (!endedAfter || !letGoOfAfter)
         throw std::logic_error("ServersLockTester: once the thread a work queue's stop gave up on had ended, the stop before the process ends still answered that something was left, or kept the database manager.");
   }

   void
   ServersLockTester::TestAConnectionInUseIsLeftOpenAndTheRestClosed_()
   {
      Application &application = *Application::Instance();

      // The database stood in for by three connections, one of them in use -
      // a statement that has not ended, a transaction, a COM call's.
      std::vector<std::shared_ptr<DALConnection>> connections;
      std::shared_ptr<DatabaseConnectionManager> database = DatabaseConnectionManagerTester::MakeManagerOfStandIns(3, connections);
      StandInDatabase standIn(application.db_manager_, application.managers_mutex_, database);

      String error;
      std::shared_ptr<DALConnection> inUse = database->BeginTransaction(error);
      if (!inUse)
         throw std::logic_error("ServersLockTester: the stand-in database handed out no connection.");

      const bool ended = application.ShutDown();
      application.shutting_down_ = false;

      const bool kept = application.GetDBManager() == database;
      int idleClosed = 0;
      for (const std::shared_ptr<DALConnection> &connection : connections)
      {
         if (connection != inUse && !connection->IsConnected())
            idleClosed++;
      }

      const bool inUseOpen = inUse->IsConnected();

      // Nothing handed out after the close - asked on a thread of its own,
      // since a pool that waited for the connection in use would not answer
      // until it is handed back below.
      std::shared_ptr<std::atomic<bool>> asked = std::make_shared<std::atomic<bool>>(false);
      std::shared_ptr<std::atomic<bool>> handedOut = std::make_shared<std::atomic<bool>>(false);
      boost::thread asker([database, asked, handedOut]()
      {
         String askError;
         std::shared_ptr<DALConnection> after = database->BeginTransaction(askError);
         handedOut->store(after != nullptr);
         if (after)
            database->RollbackTransaction(after, askError);
         asked->store(true);
      });

      const std::chrono::steady_clock::time_point until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
      while (!asked->load() && std::chrono::steady_clock::now() < until)
         std::this_thread::sleep_for(std::chrono::milliseconds(10));
      const bool answeredAtOnce = asked->load();

      // Closed once its caller hands it back.
      database->CommitTransaction(inUse, error);
      const bool closedWhenBack = !inUse->IsConnected();

      if (!asker.try_join_for(boost::chrono::seconds(20)))
         asker.detach();

      // And with nothing in use, the stop ends the process the ordinary way.
      const bool endedAfter = application.ShutDown();
      application.shutting_down_ = false;
      const bool letGoOfAfter = !application.GetDBManager();

      if (ended)
         throw std::logic_error("ServersLockTester: the stop before the process ends answered that nothing was left while a database connection was in use, and the process would have ended through the destructors of statics under its caller.");

      if (!inUseOpen)
         throw std::logic_error("ServersLockTester: the stop before the process ends closed a database connection in use under its caller, as it did until 10 October 2026 (PQfinish, mysql_close under the statement).");

      if (idleClosed != 2)
         throw std::logic_error("ServersLockTester: the stop before the process ends left a database connection that nothing used open, alongside one in use: on SQL Server Compact, what was committed since its last write would have been lost with the process, as from 10 October 2026 until the re-check of that change.");

      if (!kept)
         throw std::logic_error("ServersLockTester: the stop before the process ends let go of the database manager while a connection was in use: a statement its caller made next would have gone through none.");

      if (!answeredAtOnce || handedOut->load())
         throw std::logic_error("ServersLockTester: a database connection asked for after the stop before the process ends had closed the database was handed out, or waited for.");

      if (!closedWhenBack)
         throw std::logic_error("ServersLockTester: the connection in use during the stop before the process ends was not closed when its caller handed it back.");

      if (!endedAfter || !letGoOfAfter)
         throw std::logic_error("ServersLockTester: once no connection was in use, the stop before the process ends still answered that something was left, or kept the database manager.");
   }
   void
   ServersLockTester::TestAScriptsCallWaitsAMomentAtMost_()
   {
      Application &application = *Application::Instance();
      std::shared_ptr<ServersLockAnswers> answers = std::make_shared<ServersLockAnswers>();

      // COM's Stop (the Control Panel's Pause) holding the mutex, as it does
      // while it joins the sandbox's poller without a limit, until the test
      // lets it go - a minute at most.
      boost::thread stop([&application, answers]()
      {
         boost::unique_lock<boost::timed_mutex> lock(application.servers_mutex_, boost::defer_lock);
         const String taking = application.TakeServersLock_(lock, *MessageIndexer::Instance());
         Application::ServersLockHeld_ held(application, lock);
         answers->Set([&]() { answers->taking = taking; answers->holding = true; });

         answers->WaitFor(ServersLockWait, [&answers]() { return answers->let_go; });
      });

      if (!answers->WaitFor(ServersLockWait, [&answers]() { return answers->holding; }) || !answers->taking.IsEmpty())
      {
         answers->Set([&]() { answers->let_go = true; });
         stop.join();
         throw std::logic_error("ServersLockTester: the servers' mutex was not taken for the stop the test stands in for.");
      }

      // An OnError script's Application.Reinitialize on that poller's thread,
      // standing in: a reinitialise asked with no limit of its own, on a thread
      // counted as running a script.
      struct ScriptCall
      {
         std::atomic<bool> returned{false};
         String answer;
         std::chrono::steady_clock::duration took{};
      };
      std::shared_ptr<ScriptCall> call = std::make_shared<ScriptCall>();
      boost::thread poller([&application, call]()
      {
         ScriptServer::ScriptRunOnThisThread script;
         const std::chrono::steady_clock::time_point before = std::chrono::steady_clock::now();
         call->answer = application.Reinitialize();
         call->took = std::chrono::steady_clock::now() - before;
         call->returned.store(true);
      });

      // While the stop still holds the mutex - as it would for ever, joining
      // the poller.
      const std::chrono::steady_clock::time_point until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
      while (!call->returned.load() && std::chrono::steady_clock::now() < until)
         std::this_thread::sleep_for(std::chrono::milliseconds(25));

      const bool returnedWhileHeld = call->returned.load();

      // A call that still waits finds the process ending once it takes the
      // mutex, and so reinitialises nothing.
      application.shutting_down_ = true;
      answers->Set([&]() { answers->let_go = true; });
      stop.join();

      const bool joined = poller.try_join_for(boost::chrono::seconds(20));
      application.shutting_down_ = false;
      if (!joined)
         poller.detach();

      if (!returnedWhileHeld)
         throw std::logic_error("ServersLockTester: a reinitialise asked from an event script had not returned twenty seconds into a stop that held the servers' mutex: it waited for the stop, which may be joining the script's own thread - the sandbox's poller, for COM's Stop - and the two would have waited for each other for ever, as from 10 October 2026 until the re-check of that change.");

      if (!joined)
         throw std::logic_error("ServersLockTester: a reinitialise asked from an event script had not ended twenty seconds after the stop let go of the servers' mutex.");

      if (!call->answer.Contains(_T("event script")))
         throw std::logic_error("ServersLockTester: a reinitialise asked from an event script during a stop that held the servers' mutex was not refused as a script's call.");

      // A moment first, so that a stop about to end is not refused: the limit,
      // give or take the clock's grain. (A copy: chrono takes the constant by
      // reference.)
      const int limit = Application::ComServersWaitSeconds;
      if (call->took < std::chrono::seconds(limit) - std::chrono::milliseconds(500))
         throw std::logic_error("ServersLockTester: a reinitialise asked from an event script during a stop was refused without waiting the few seconds it waits.");
   }

   namespace
   {
      // What a handler or a task IOServiceLifetimeTester holds waits on, and
      // what it says, shared so that a thread the test gives up on has what it
      // reads. The hold is bounded, so that a test that fails never leaves a
      // thread waiting for ever.
      struct LifetimeHold
      {
         std::mutex lock;
         std::condition_variable changed;
         bool began = false;
         bool let_go = false;
         bool ended = false;

         // Not a Boost interruption point: a stop's interrupt() does not reach
         // it, as it does not reach a scanner's socket or a script's loop.
         void Hold()
         {
            Set([this]() { began = true; });
            {
               std::unique_lock<std::mutex> guard(lock);
               changed.wait_for(guard, std::chrono::seconds(90), [this]() { return let_go; });
            }
            Set([this]() { ended = true; });
         }

         void LetGo()
         {
            Set([this]() { let_go = true; });
         }

         bool HasEnded()
         {
            std::lock_guard<std::mutex> guard(lock);
            return ended;
         }

         template <typename Predicate>
         bool WaitFor(std::chrono::seconds within, Predicate predicate)
         {
            std::unique_lock<std::mutex> guard(lock);
            return changed.wait_for(guard, within, predicate);
         }

         template <typename Change>
         void Set(Change change)
         {
            {
               std::lock_guard<std::mutex> guard(lock);
               change();
            }
            changed.notify_all();
         }
      };

      class LifetimeHeldTask : public Task
      {
      public:
         explicit LifetimeHeldTask(const std::shared_ptr<LifetimeHold> &hold) :
            Task("IOServiceLifetimeTester's held task"),
            hold_(hold)
         {
         }

      protected:
         void DoWork() override
         {
            hold_->Hold();
         }

      private:
         std::shared_ptr<LifetimeHold> hold_;
      };

      template <typename Condition>
      bool WaitUntil(std::chrono::seconds bound, Condition condition)
      {
         const std::chrono::steady_clock::time_point until = std::chrono::steady_clock::now() + bound;
         for (;;)
         {
            if (condition())
               return true;

            if (std::chrono::steady_clock::now() >= until)
               return false;

            std::this_thread::sleep_for(std::chrono::milliseconds(25));
         }
      }
   }

   void
   IOServiceLifetimeTester::Test()
   {
      if (ServerStatus::Instance()->GetState() != ServerStatus::StateRunning)
         throw std::logic_error("IOServiceLifetimeTester: the servers were not running, and the test stops and starts them.");

      // Both are run whatever the first answers, and both answers given.
      const String queueTask = TestAQueueTaskHeld_();
      const String listenerHandler = TestAListenersHandlerHeld_();

      if (queueTask.IsEmpty() && listenerHandler.IsEmpty())
         return;

      String said = _T("IOServiceLifetimeTester:");
      if (!queueTask.IsEmpty())
      {
         said += _T(" with a task of the asynchronous-task queue held, ");
         said += queueTask;
      }
      if (!listenerHandler.IsEmpty())
      {
         said += _T(" with a handler on a listener's strand held, ");
         said += listenerHandler;
      }

      throw std::logic_error(AnsiString(said).c_str());
   }

   std::shared_ptr<TCPServer>
   IOServiceLifetimeTester::ListenerOn_(IOService &service, const IPAddress &address, int port)
   {
      boost::lock_guard<boost::mutex> guard(service.tcp_servers_mutex_);
      for (const std::shared_ptr<TCPServer> &listener : service.tcp_servers_)
      {
         if (listener->GetPort() == port && listener->GetAddress().GetAddress() == address.GetAddress())
            return listener;
      }

      return std::shared_ptr<TCPServer>();
   }

   String
   IOServiceLifetimeTester::TestAQueueTaskHeld_()
   {
      Application &application = *Application::Instance();
      WorkQueueManager &manager = *WorkQueueManager::Instance();

      std::shared_ptr<WorkQueue> queue = application.GetAsyncWorkQueue();
      std::weak_ptr<IOService> seen = application.GetIOService();
      if (!queue || seen.expired())
         return _T("the asynchronous-task queue or the TCP/IP service did not exist.");

      // A task of one of the session queues, which are given the TCP/IP service
      // to keep alive (Application::CreateSessionWorkQueues_): a finalisation
      // waiting on a scanner, say, which the stop cannot interrupt.
      std::shared_ptr<LifetimeHold> hold = std::make_shared<LifetimeHold>();
      queue->AddTask(std::make_shared<LifetimeHeldTask>(hold));
      queue.reset();

      if (!hold->WaitFor(std::chrono::seconds(60), [&hold]() { return hold->began; }))
      {
         hold->LetGo();
         return _T("the queue began no task within a minute.");
      }

      const int before = WorkQueue::GetThreadsLeft();

      // Ten seconds, and the queue's stop gives up on the held thread.
      const String refused = application.StopServers();

      // Then only the held thread is still counted, and a sweep frees nothing
      // that it keeps.
      const bool settled = WaitUntil(std::chrono::seconds(30), [before]() { return WorkQueue::GetThreadsLeft() == before + 1; });
      manager.SweepRetiredQueues();

      const bool heldThroughTheStop = !hold->HasEnded();
      const bool letGoOfByTheServers = !application.GetIOService();
      const bool keptWhileHeld = !seen.expired();

      hold->LetGo();
      const bool ended = hold->WaitFor(std::chrono::seconds(60), [&hold]() { return hold->ended; });
      const bool left = ended && WaitUntil(std::chrono::seconds(30), [before]() { return WorkQueue::GetThreadsLeft() == before; });
      manager.SweepRetiredQueues();
      const bool freedAfter = seen.expired();

      String startRefused;
      const bool started = application.StartServers(0, &startRefused);

      if (!refused.IsEmpty())
         return String(_T("the stop of the servers was refused: ")) + refused;

      if (!heldThroughTheStop)
         return _T("the stop of the servers returned only once the task had ended: it waited for the thread its queue's stop had given up on.");

      if (!settled)
         return _T("the thread the stop gave up on was not counted, or another was still counted thirty seconds after the stop (WorkQueue::GetThreadsLeft).");

      if (!letGoOfByTheServers)
         return _T("the servers had not let go of the TCP/IP service after the stop.");

      if (!keptWhileHeld)
         return _T("the TCP/IP service was freed while the task, which the stop had given up on, still ran in its queue.");

      if (!left)
         return _T("the thread the stop gave up on had not left within half a minute of its task's end.");

      if (!freedAfter)
         return _T("the TCP/IP service was still kept after the task had ended and a sweep had run.");

      if (!started)
         return String(_T("the servers did not start again after the test: ")) + startRefused;

      return String();
   }

   String
   IOServiceLifetimeTester::TestAListenersHandlerHeld_()
   {
      Application &application = *Application::Instance();
      WorkQueueManager &manager = *WorkQueueManager::Instance();

      std::shared_ptr<IOService> service = application.GetIOService();
      if (!service)
         return _T("the TCP/IP service did not exist.");

      // A listening port, and the strand its accept completions run on. A
      // handler held there stands in for HandleAccept running the
      // OnClientConnect script, on one of the TCP/IP service's own threads -
      // those of the IOCP queue, which is given the service to keep alive
      // (IOService::DoWork).
      //
      // Looked for over a few seconds, not once: the listeners are made by that
      // DoWork, the service's own task on a thread of the main server queue,
      // which sets the event StartServers waits for before it makes the first
      // of them, so the start that ends TestAQueueTaskHeld_ can return - and
      // say "Servers started." - before any port listens.
      const int listenerWaitSeconds = 5;
      std::shared_ptr<TCPServer> listener;
      WaitUntil(std::chrono::seconds(listenerWaitSeconds), [&service, &listener]()
      {
         boost::lock_guard<boost::mutex> guard(service->tcp_servers_mutex_);
         for (const std::shared_ptr<TCPServer> &candidate : service->tcp_servers_)
         {
            if (candidate->IsListening())
            {
               listener = candidate;
               return true;
            }
         }

         return false;
      });

      if (!listener)
         return Formatter::Format(_T("no port of the TCP/IP service was listening {0} seconds after the servers had started."), listenerWaitSeconds);

      const IPAddress address = listener->GetAddress();
      const int port = listener->GetPort();
      std::weak_ptr<IOService> seen = service;

      std::shared_ptr<LifetimeHold> hold = std::make_shared<LifetimeHold>();
      boost::asio::dispatch(listener->GetStrand(), [hold]() { hold->Hold(); });
      listener.reset();
      service.reset();

      if (!hold->WaitFor(std::chrono::seconds(60), [&hold]() { return hold->began; }))
      {
         hold->LetGo();
         return _T("the listener's strand ran nothing within a minute.");
      }

      const int before = WorkQueue::GetThreadsLeft();

      // The drain's request to close that listener waits on the strand, and the
      // IOCP queue's stop gives up on the held thread after its ten seconds.
      const String refused = application.StopServers();

      const bool settled = WaitUntil(std::chrono::seconds(30), [before]() { return WorkQueue::GetThreadsLeft() == before + 1; });
      manager.SweepRetiredQueues();

      const bool heldThroughTheStop = !hold->HasEnded();
      const bool keptWhileHeld = !seen.expired();

      // Started again with the thread still on the old service's strand: the
      // new service's listener on that port listens.
      String startRefused;
      const bool started = application.StartServers(0, &startRefused);

      const bool listening = started && WaitUntil(std::chrono::seconds(20), [&application, &address, port]()
      {
         std::shared_ptr<IOService> current = application.GetIOService();
         if (!current)
            return false;

         std::shared_ptr<TCPServer> again = IOServiceLifetimeTester::ListenerOn_(*current, address, port);
         return again && again->IsListening();
      });

      const bool stillHeldAtTheStart = !hold->HasEnded();

      hold->LetGo();
      const bool ended = hold->WaitFor(std::chrono::seconds(60), [&hold]() { return hold->ended; });
      const bool left = ended && WaitUntil(std::chrono::seconds(30), [before]() { return WorkQueue::GetThreadsLeft() == before; });
      manager.SweepRetiredQueues();
      const bool freedAfter = seen.expired();

      if (!refused.IsEmpty())
         return String(_T("the stop of the servers was refused: ")) + refused;

      if (!heldThroughTheStop)
         return _T("the stop of the servers returned only once the handler had ended: it waited for the thread the IOCP queue's stop had given up on.");

      if (!settled)
         return _T("the thread the stop gave up on was not counted, or another was still counted thirty seconds after the stop (WorkQueue::GetThreadsLeft).");

      if (!keptWhileHeld)
         return _T("the TCP/IP service was freed while the handler, which the stop had given up on, still ran on one of its threads.");

      if (!started)
         return String(_T("the servers did not start again: ")) + startRefused;

      if (!stillHeldAtTheStart)
         return _T("the handler had ended before the start was looked at, so the start was not tested beside it.");

      if (!listening)
         return Formatter::Format(_T("the start after the stop did not listen on {0}:{1} within twenty seconds, the port of the listener whose strand the held thread was on."), String(address.ToString()), port);

      if (!left)
         return _T("the thread the stop gave up on had not left within half a minute of the handler's end.");

      if (!freedAfter)
         return _T("the old TCP/IP service was still kept after the handler had ended and a sweep had run.");

      return String();
   }
}
