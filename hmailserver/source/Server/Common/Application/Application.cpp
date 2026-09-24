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

#include "../Util/Time.h"
#include "../Util/ServerStatus.h"
#include "../Util/Languages.h"
#include "../Util/Utilities.h"
#include "../Util/MetricsServer.h"
#include "../Util/RateLimiter.h"
#include "../Util/JwksKeySet.h"
#include "../Util/OtelTracer.h"
#include "../Util/OtelMetricsExporter.h"
#include "../Util/OtelLogExporter.h"
#include "../Util/SyslogSink.h"
#include "../Mime/MimeCode.h"

#include "Property.h"
#include "Version.h"

#include "../../SMTP/SMTPDeliveryManager.h"
#include "../../IMAP/IMAPConfiguration.h"
#include "../../ExternalFetcher/ExternalFetchManager.h"

#include "../Threading/WorkQueueManager.h"
#include "../Threading/WorkQueue.h"
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

#include "../BO/Message.h"
#include "../BO/Domain.h"
#include "../Persistence/MailboxMove.h"
#include "MigrationManager.h"
#include "../Persistence/PersistentMessage.h"
#include "../Persistence/PersistentDomain.h"
#include "../Persistence/SchemaUpgrader.h"
#include "../Util/AuditTrail.h"
#include "RemoveExpiredRecords.h"
#include "ScheduledMailTask.h"
#include "../Util/AutoBanFirewall.h"
#include "../Util/CompromiseDetector.h"
#include "../AntiVirus/Sandbox.h"
#include "LogRetentionTask.h"
#include "ArchiveRetentionTask.h"
#include "MailboxRetentionTask.h"
#include "MetricsHistoryTask.h"
#include "QuarantineDigestTask.h"
#include "UpdateCheckTask.h"
#include "../Util/UpdateApplyToken.h"
#include "../Util/UpdateChecker.h"
#include "IMAPExpungeRetentionTask.h"
#include "MessageStoreConsistencyTask.h"
#include "WorkQueueHealthTask.h"
#include "BounceMaintenanceTask.h"
#include "ComplaintMaintenanceTask.h"
#include "../Util/DiskSpace.h"
#include "../Util/AlertManager.h"
#include "../Util/PushNotifier.h"
#include "../Util/WebhookManager.h"
#include "DkimRotationTask.h"
#include "ReputationWatchTask.h"
#include "BackupScheduleTask.h"
#include "../LDAP/DirectorySyncTask.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif



namespace HM
{
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
#if _WIN64
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

      // Language needed for COM API
      Languages::Instance()->Load();

      // Create the backup manager that manages backup tasks...
      backup_manager_ = std::shared_ptr<BackupManager>(new BackupManager);

      LOG_DEBUG("Application::InitInstance - Connecting to database...");
      if (!OpenDatabase(sErrorMessage))
      {
         // We could not open the database. We can just
         // as well stop now since there's nothing to do.
         return false;
      }
      
      if (!OnDatabaseConnected(sErrorMessage))
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

      // Which settings the environment sets, said where an operator reads the
      // start: no interface will change them (docs/EnvironmentConfiguration.md).
      EnvironmentSettings::AnnounceManaged();

      // The firewall is made to match the auto-ban ranges before a listener is
      // open: a rule for a ban that expired while the service was down is removed
      // here, and one for a ban still in force is put back if it is missing.
      AutoBanFirewall::Synchronise(_T("start-up"));

      // The accounts the compromised-account detection locked before this
      // start are held again before a listener is open: the lock is a row in
      // hm_accountreview, and the hold on the name is in memory.
      CompromiseDetector::LoadLocks();

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

      db_manager_ = std::shared_ptr<DatabaseConnectionManager>(new DatabaseConnectionManager);
      bool bConnectedSuccessfully = db_manager_->CreateConnections(sErrorMessage);

      last_connect_error_message_ = sErrorMessage;

      if (!bConnectedSuccessfully)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Critical, 4354, "Application::OpenDatabase", "hMailServer failed to connect to the database server. Error message: " + sErrorMessage);
         return false;
      }

      return true;
   }


   bool
   Application::OnDatabaseConnected(String &sErrorMessage)
   {
      LOG_DEBUG("Application::OnDatabaseConnected()");

      int iDBVersion = GetDBManager()->GetCurrentDatabaseVersion();

      if (iDBVersion == 0)
      {
         sErrorMessage = "Database version could not be detected.";
         last_connect_error_message_ = sErrorMessage;
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
         LOG_APPLICATION(Formatter::Format("Schema: the database is at version {0} and this build needs {1}; [Database] UpgradeOnStart is set, so the upgrade chain runs now, one step at a time.", iDBVersion, requiredVersion));

         SchemaUpgrader::Outcome outcome = GetDBManager()->UpgradeSchema(requiredVersion, IniFileSettings::Instance()->GetDBScriptDirectory(),
            [](const String &line) { LOG_APPLICATION("Schema: " + line); });

         if (!outcome.ok)
         {
            sErrorMessage = Formatter::Format("The database is too old for this version of hMailServer, and the upgrade [Database] UpgradeOnStart asked for stopped at step {0}: {1} The database is at version {2}; the steps before that one have been applied and it will resume there. Fix the cause and start the service again, or run hMailServer Database updater (DBUpdater.exe) / hmailserver --upgrade-database. Required database version: {3}",
               outcome.failed_step, outcome.error, outcome.version, requiredVersion);
            last_connect_error_message_ = sErrorMessage;
            ErrorManager::Instance()->ReportError(ErrorManager::Critical, 5011, "Application::OnDatabaseConnected", sErrorMessage);
            return false;
         }

         iDBVersion = outcome.version;
         LOG_APPLICATION(Formatter::Format("Schema: upgraded in {0} step(s); the database is at version {1}.", outcome.steps, iDBVersion));
      }

      if (iDBVersion < Configuration::Instance()->GetRequiredDBVersion()
          || iDBVersion > Configuration::Instance()->GetRequiredDBVersion())
      {
         String sErrorMessage;

         if (iDBVersion > Configuration::Instance()->GetRequiredDBVersion())
            sErrorMessage = "The database is too new for this version of hMailServer. Please upgrade hMailServer.";
         else
            sErrorMessage = "The database is too old for this version of hMailServer. Please run hMailServer Database updater (DBUpdater.exe) to upgrade it.";

         String sVersionInfo;
         sVersionInfo.Format(_T(" Database version: %d, Required database version: %d"), iDBVersion, Configuration::Instance()->GetRequiredDBVersion());

         sErrorMessage += sVersionInfo;
         
         last_connect_error_message_ = sErrorMessage;

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
   // Do uninitialization.
   //---------------------------------------------------------------------------()
   {
      initialized_ = false;

      // Close work queue
      WorkQueueManager::Instance()->RemoveQueue(maintenance_queue_);

      // Normally already gone: StopServers removes these while io_service_ is still
      // alive, which is the only ordering that is safe. Kept here as a backstop for
      // the one path that skips it - a StartServers that failed before the state
      // reached Running, after which StopServers returns at its own first check.
      RemoveSessionWorkQueues_();

      // Backup manager is created by initinstance so should be destroyed here.
      if (backup_manager_) 
         backup_manager_.reset();

      LOG_DEBUG("Application::ExitInstance - Closing database connection...");
      CloseDatabase();

      MimeEnvironment::SetAutoFolding(false);

      OutOfMemoryHandler::Terminate();
 
      return 0;
    
   }

   void
   Application::CloseDatabase()
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Close the database connection.
   //---------------------------------------------------------------------------()
   {
      if (db_manager_)
      {
         try
         {
            db_manager_->Disconnect();
         }
         catch (...)
         {

         }

         db_manager_.reset();
      }
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
   // Creating them here, before io_service_ exists, and removing them in
   // StopServers before io_service_ is reset, makes the ordering an invariant of
   // the two functions rather than something each new queue has to remember.
   //---------------------------------------------------------------------------()
   {
      // Processes asynchronous tasks from clients - principally message
      // finalization, which is what sends the final "250 OK".
      WorkQueueManager::Instance()->CreateWorkQueue(Configuration::Instance()->GetAsynchronousThreads(),
                                                    asynchronous_tasks_queue_);

      // Separate queue for blocking name lookups made on behalf of a live
      // session. These can stall for seconds on an address whose reverse zone
      // is unreachable, so they are kept off the asynchronous task queue that
      // finalizes (and acknowledges) received messages. Saturating this queue
      // only delays the lookups themselves, which degrade to "Unknown".
      WorkQueueManager::Instance()->CreateWorkQueue(Configuration::Instance()->GetAsynchronousThreads(),
                                                    name_lookup_queue_);

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
   // not. RemoveQueue joins the queue's worker threads, so on return no task can
   // still be holding a connection.
   //
   // Safe to call twice - RemoveQueue on a name that is not registered is a
   // no-op - which is what lets ExitInstance keep it as a backstop for a
   // StartServers that failed before the state reached Running.
   //---------------------------------------------------------------------------()
   {
      WorkQueueManager::Instance()->RemoveQueue(asynchronous_tasks_queue_);

      WorkQueueManager::Instance()->RemoveQueue(name_lookup_queue_);
   }

   bool
   Application::StartServers()
   {
      LOG_APPLICATION("Starting servers...")

      if (ServerStatus::Instance()->GetState() != ServerStatus::StateStopped)
      {
         LOG_DEBUG("Application::~StartServers() - E1");
         return false;
      }

      ServerStatus::Instance()->SetState(ServerStatus::StateStarting);

      SpamProtection::Instance()->Load();

      // Before io_service_, so that no listener can ever accept a connection on a
      // context whose session queues do not exist yet.
      CreateSessionWorkQueues_();

      io_service_ = std::shared_ptr<IOService>(new IOService);
      io_service_->Initialize();
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
   // first. The two that reach the scheduler do nothing while a reinitialise holds
   // its mutex: that reinitialise is rebuilding the scheduler from the published
   // values, and a handle to the one it is tearing down is not worth racing for.
      // A service stop does not take that mutex, so the scheduler and the two task
      // handles are copied under scheduler_handles_mutex_, which the stop's reset
      // takes too. Each reads the value it takes when it runs, as Subscribe asks,
      // and runs after the save's reload lock is released (IniFileSettings::Tell_).
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
         boost::unique_lock<boost::mutex> rebuilding(reinitialize_mutex_, boost::try_to_lock);
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

         boost::unique_lock<boost::mutex> rebuilding(reinitialize_mutex_, boost::try_to_lock);
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
      // makes one - each pass is one select that finds nothing.
      std::shared_ptr<WebhookTask> webhookTask = std::shared_ptr<WebhookTask>(new WebhookTask);
      webhookTask->SetReoccurance(ScheduledTask::RunInfinitely);
      webhookTask->SetMinutesBetweenRun(1);
      scheduler_->ScheduleTask(webhookTask);

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

   void 
   Application::StopServers()
   {
      LOG_APPLICATION("Stopping servers...")

      start_time_ = "";

      // A stop from Running or from Draining, and from nothing else. The state
      // moves to Stopping under the drain lock, so that a drain arriving in the
      // same instant sees Stopping and is refused rather than both of them
      // acting on Running.
      bool wasDraining = false;
      {
         boost::lock_guard<boost::recursive_mutex> guard(drain_mutex_);

         int state = ServerStatus::Instance()->GetState();
         if (state != ServerStatus::StateRunning && state != ServerStatus::StateDraining)
         {
            LOG_DEBUG("Application::~StopServers() - E1");
            return;
         }

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
               if (stop_progress_ && (++polls % 5) == 0)
                  stop_progress_();

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
      Sandbox::Stop(stop_progress_);

      if (metrics_server_)
      {
         metrics_server_->Stop();
         metrics_server_.reset();
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

      // Deinitialize servers
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
      
      LOG_DEBUG("Application::StopServers() - Destructing Rest");
      if (notification_server_) notification_server_.reset();
      if (folder_manager_) folder_manager_.reset();

      MessageIndexer::Instance()->Stop();
      
      ServerStatus::Instance()->SetState(ServerStatus::StateStopped);

      LOG_APPLICATION("Servers stopped.")

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
      // One restart at a time: see the member's comment in Application.h. A
      // second caller waits for the first to finish and then restarts again,
      // which is what it asked for.
      boost::lock_guard<boost::mutex> guard(reinitialize_mutex_);

      StopServers();
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

      String sErrorMessage;
      if (!InitInstance(sErrorMessage))
         return sErrorMessage;

      if (!StartServers())
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
}
