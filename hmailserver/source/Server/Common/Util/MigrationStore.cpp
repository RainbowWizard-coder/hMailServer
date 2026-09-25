// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// See MigrationStore.h. Every read is a parameterised SQLCommand, every write an
// SQLStatement or a parameterised command, through the one statement chokepoint -
// which is also how a migration created, started, paused or deleted over REST
// reaches the audit trail (AuditTrail.cpp lists hm_migrations and
// hm_migrationjobs), while the runner's own writes, made with no actor on its
// thread, do not.

#include "StdAfx.h"
#include "MigrationStore.h"
#include "AuditTrail.h"
#include "Crypt.h"
#include "../SQL/SQLCommand.h"
#include "../SQL/SQLStatement.h"
#include "../SQL/DALRecordset.h"
#include "../Application/Application.h"

#include <ctime>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   const int MigrationStore::MaximumNameLength;
   const int MigrationStore::MaximumHostLength;
   const int MigrationStore::MaximumUserLength;
   const int MigrationStore::MaximumErrorLength;
   const int MigrationStore::MaximumFolderLength;
   const int MigrationStore::MaximumReportBytes;

   namespace
   {
      const char *MigrationColumns =
         "migrationid, migrationname, migrationkind, migrationhost, migrationport, migrationsecurity, "
         "migrationoauthprovider, migrationdavurl, migrationconcurrency, migrationmessagesperminute, "
         "migrationstate, migrationcreated, migrationstartedat, migrationfinishedat";

      // Without the two ntext columns: what a listing needs.
      const char *JobColumns =
         "mjid, mjmigrationid, mjsourceuser, mjsigninhash, mjtargetaccountid, mjstate, mjphase, "
         "mjfolderstotal, mjfoldersdone, mjmessagestotal, mjmessagesdone, mjbytesdone, mjskipped, "
         "mjlasterror, mjattempts, mjnextattempt, mjstartedat, mjfinishedat";

      const char *FolderColumns =
         "mfid, mfjobid, mffolder, mftargetfolder, mfuidvalidity, mflastuid, mfmessages, mfdone";

      void ReadMigration(std::shared_ptr<DALRecordset> recordset, MigrationRecord &migration)
      {
         migration.id = recordset->GetInt64Value("migrationid");
         migration.name = recordset->GetStringValue("migrationname");
         migration.kind = recordset->GetLongValue("migrationkind");
         migration.host = recordset->GetStringValue("migrationhost");
         migration.port = recordset->GetLongValue("migrationport");
         migration.security = recordset->GetLongValue("migrationsecurity");
         migration.oauthProvider = recordset->GetStringValue("migrationoauthprovider");
         migration.davUrl = recordset->GetStringValue("migrationdavurl");
         migration.concurrency = recordset->GetLongValue("migrationconcurrency");
         migration.messagesPerMinute = recordset->GetLongValue("migrationmessagesperminute");
         migration.state = recordset->GetLongValue("migrationstate");
         migration.created = recordset->GetInt64Value("migrationcreated");
         migration.startedAt = recordset->GetInt64Value("migrationstartedat");
         migration.finishedAt = recordset->GetInt64Value("migrationfinishedat");
      }

      void ReadJob(std::shared_ptr<DALRecordset> recordset, MigrationJobRecord &job)
      {
         job.id = recordset->GetInt64Value("mjid");
         job.migrationId = recordset->GetInt64Value("mjmigrationid");
         job.sourceUser = recordset->GetStringValue("mjsourceuser");
         job.signInHash = recordset->GetStringValue("mjsigninhash");
         job.targetAccountId = recordset->GetInt64Value("mjtargetaccountid");
         job.state = recordset->GetLongValue("mjstate");
         job.phase = recordset->GetLongValue("mjphase");
         job.foldersTotal = recordset->GetLongValue("mjfolderstotal");
         job.foldersDone = recordset->GetLongValue("mjfoldersdone");
         job.messagesTotal = recordset->GetLongValue("mjmessagestotal");
         job.messagesDone = recordset->GetLongValue("mjmessagesdone");
         job.bytesDone = recordset->GetInt64Value("mjbytesdone");
         job.skipped = recordset->GetLongValue("mjskipped");
         job.lastError = recordset->GetStringValue("mjlasterror");
         job.attempts = recordset->GetLongValue("mjattempts");
         job.nextAttempt = recordset->GetInt64Value("mjnextattempt");
         job.startedAt = recordset->GetInt64Value("mjstartedat");
         job.finishedAt = recordset->GetInt64Value("mjfinishedat");
      }

      void ReadFolder(std::shared_ptr<DALRecordset> recordset, MigrationFolderRecord &folder)
      {
         folder.id = recordset->GetInt64Value("mfid");
         folder.jobId = recordset->GetInt64Value("mfjobid");
         folder.folder = recordset->GetStringValue("mffolder");
         folder.targetFolder = recordset->GetStringValue("mftargetfolder");
         folder.uidValidity = recordset->GetStringValue("mfuidvalidity");
         folder.lastUid = recordset->GetInt64Value("mflastuid");
         folder.messages = recordset->GetLongValue("mfmessages");
         folder.done = recordset->GetLongValue("mfdone") != 0;
      }

      // A value cut to what its column holds, so a long sentence from a remote
      // server cannot make a write fail on one backend and succeed on another.
      String Fit(const String &value, int maximum)
      {
         return value.GetLength() <= maximum ? value : value.Mid(0, maximum);
      }

      void AddMigrationColumns(SQLStatement &statement, const MigrationRecord &migration)
      {
         statement.AddColumn("migrationname", Fit(migration.name, MigrationStore::MaximumNameLength));
         statement.AddColumn("migrationkind", (long) migration.kind);
         statement.AddColumn("migrationhost", Fit(migration.host, MigrationStore::MaximumHostLength));
         statement.AddColumn("migrationport", (long) migration.port);
         statement.AddColumn("migrationsecurity", (long) migration.security);
         statement.AddColumn("migrationoauthprovider", Fit(migration.oauthProvider, 32));
         statement.AddColumn("migrationdavurl", Fit(migration.davUrl, 255));
         statement.AddColumn("migrationconcurrency", (long) migration.concurrency);
         statement.AddColumn("migrationmessagesperminute", (long) migration.messagesPerMinute);
         statement.AddColumn("migrationstate", (long) migration.state);
         statement.AddColumnInt64("migrationcreated", migration.created);
         statement.AddColumnInt64("migrationstartedat", migration.startedAt);
         statement.AddColumnInt64("migrationfinishedat", migration.finishedAt);
      }

      void AddJobProgressColumns(SQLStatement &statement, const MigrationJobRecord &job)
      {
         statement.AddColumn("mjstate", (long) job.state);
         statement.AddColumn("mjphase", (long) job.phase);
         statement.AddColumn("mjfolderstotal", (long) job.foldersTotal);
         statement.AddColumn("mjfoldersdone", (long) job.foldersDone);
         statement.AddColumn("mjmessagestotal", (long) job.messagesTotal);
         statement.AddColumn("mjmessagesdone", (long) job.messagesDone);
         statement.AddColumnInt64("mjbytesdone", job.bytesDone);
         statement.AddColumn("mjskipped", (long) job.skipped);
         statement.AddColumn("mjlasterror", Fit(job.lastError, MigrationStore::MaximumErrorLength));
         statement.AddColumn("mjattempts", (long) job.attempts);
         statement.AddColumnInt64("mjnextattempt", job.nextAttempt);
         statement.AddColumnInt64("mjstartedat", job.startedAt);
         statement.AddColumnInt64("mjfinishedat", job.finishedAt);
         statement.AddColumn("mjreport", job.report);
      }
   }

   __int64
   MigrationStore::Now()
   {
      return static_cast<__int64>(time(nullptr));
   }

   bool
   MigrationStore::CreateMigration(MigrationRecord &migration)
   {
      if (migration.created == 0)
         migration.created = Now();

      SQLStatement statement;
      statement.SetTable("hm_migrations");
      statement.SetStatementType(SQLStatement::STInsert);
      statement.SetIdentityColumn("migrationid");
      AddMigrationColumns(statement, migration);

      __int64 id = 0;
      if (!Application::Instance()->GetDBManager()->Execute(statement, &id) || id <= 0)
         return false;

      migration.id = id;
      return true;
   }

   bool
   MigrationStore::GetMigration(__int64 id, MigrationRecord &migration)
   {
      SQLCommand command(String(AnsiString("select ") + MigrationColumns + " from hm_migrations where migrationid = @ID"));
      command.AddParameter("@ID", id);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset || recordset->IsEOF())
         return false;

      ReadMigration(recordset, migration);
      return true;
   }

   bool
   MigrationStore::ListMigrations(std::vector<MigrationRecord> &migrations)
   {
      migrations.clear();

      SQLCommand command(String(AnsiString("select ") + MigrationColumns + " from hm_migrations order by migrationid asc"));

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset)
         return false;

      while (!recordset->IsEOF())
      {
         MigrationRecord migration;
         ReadMigration(recordset, migration);
         migrations.push_back(migration);
         recordset->MoveNext();
      }

      return true;
   }

   bool
   MigrationStore::UpdateMigration(const MigrationRecord &migration)
   {
      SQLStatement statement;
      statement.SetTable("hm_migrations");
      statement.SetStatementType(SQLStatement::STUpdate);
      AddMigrationColumns(statement, migration);

      String where;
      where.Format(_T("migrationid = %I64d"), migration.id);
      statement.SetWhereClause(where);

      return Application::Instance()->GetDBManager()->Execute(statement);
   }

   bool
   MigrationStore::DeleteMigration(__int64 id)
   {
      // The foreign keys cascade, and the rows are removed explicitly as well,
      // child first, so that a backend whose tables were made without the keys -
      // a MySQL table left on MyISAM by hand - is not left holding orphans.
      std::vector<MigrationJobRecord> jobs;
      if (!ListJobs(id, false, jobs))
         return false;

      for (const MigrationJobRecord &job : jobs)
      {
         if (!DeleteFolders(job.id))
            return false;
      }

      SQLCommand deleteJobs("delete from hm_migrationjobs where mjmigrationid = @ID");
      deleteJobs.AddParameter("@ID", id);
      if (!Application::Instance()->GetDBManager()->Execute(deleteJobs))
         return false;

      SQLCommand deleteMigration("delete from hm_migrations where migrationid = @ID");
      deleteMigration.AddParameter("@ID", id);
      return Application::Instance()->GetDBManager()->Execute(deleteMigration);
   }

   bool
   MigrationStore::CreateJob(MigrationJobRecord &job, const String &auditName)
   {
      SQLStatement statement;
      statement.SetTable("hm_migrationjobs");
      statement.SetStatementType(SQLStatement::STInsert);
      statement.SetIdentityColumn("mjid");
      statement.AddColumnInt64("mjmigrationid", job.migrationId);
      statement.AddColumn("mjsourceuser", Fit(job.sourceUser, MaximumUserLength));
      statement.AddColumn("mjsourcesecret", job.sourceSecret.IsEmpty() ? String() : Crypt::Instance()->ProtectSecret(job.sourceSecret));
      statement.AddColumn("mjsigninhash", Fit(job.signInHash, 64));
      statement.AddColumnInt64("mjtargetaccountid", job.targetAccountId);
      AddJobProgressColumns(statement, job);

      __int64 id = 0;
      bool saved = false;
      if (auditName.IsEmpty())
      {
         saved = Application::Instance()->GetDBManager()->Execute(statement, &id);
      }
      else
      {
         // The row names an account by its number; the trail names the job
         // as a person knows it.
         AuditDescription described(_T("hm_migrationjobs"), auditName);
         saved = Application::Instance()->GetDBManager()->Execute(statement, &id);
      }

      if (!saved || id <= 0)
         return false;

      job.id = id;
      return true;
   }

   bool
   MigrationStore::GetJob(__int64 id, MigrationJobRecord &job)
   {
      SQLCommand command(String(AnsiString("select ") + JobColumns + ", mjsourcesecret, mjreport from hm_migrationjobs where mjid = @ID"));
      command.AddParameter("@ID", id);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset || recordset->IsEOF())
         return false;

      ReadJob(recordset, job);

      const String stored = recordset->GetStringValue("mjsourcesecret");
      job.sourceSecret = stored.IsEmpty() ? String() : Crypt::Instance()->UnprotectSecret(stored);
      job.report = recordset->GetStringValue("mjreport");
      return true;
   }

   bool
   MigrationStore::ListJobs(__int64 migrationId, bool withReport, std::vector<MigrationJobRecord> &jobs)
   {
      jobs.clear();

      SQLCommand command(withReport
         ? String(AnsiString("select ") + JobColumns + ", mjreport from hm_migrationjobs where mjmigrationid = @ID order by mjid asc")
         : String(AnsiString("select ") + JobColumns + " from hm_migrationjobs where mjmigrationid = @ID order by mjid asc"));
      command.AddParameter("@ID", migrationId);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset)
         return false;

      while (!recordset->IsEOF())
      {
         MigrationJobRecord job;
         ReadJob(recordset, job);
         if (withReport)
            job.report = recordset->GetStringValue("mjreport");
         jobs.push_back(job);
         recordset->MoveNext();
      }

      return true;
   }

   bool
   MigrationStore::ListActiveJobs(std::vector<MigrationJobRecord> &jobs)
   {
      jobs.clear();

      SQLCommand command(String(AnsiString("select ") + JobColumns + " from hm_migrationjobs where mjstate = @QUEUED or mjstate = @RUNNING or mjstate = @PAUSED order by mjid asc"));
      command.AddParameter("@QUEUED", (int) JobQueued);
      command.AddParameter("@RUNNING", (int) JobRunning);
      command.AddParameter("@PAUSED", (int) JobPaused);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset)
         return false;

      while (!recordset->IsEOF())
      {
         MigrationJobRecord job;
         ReadJob(recordset, job);
         jobs.push_back(job);
         recordset->MoveNext();
      }

      return true;
   }

   bool
   MigrationStore::UpdateJob(const MigrationJobRecord &job, const String &auditName)
   {
      SQLStatement statement;
      statement.SetTable("hm_migrationjobs");
      statement.SetStatementType(SQLStatement::STUpdate);
      AddJobProgressColumns(statement, job);

      String where;
      where.Format(_T("mjid = %I64d"), job.id);
      statement.SetWhereClause(where);

      if (auditName.IsEmpty())
         return Application::Instance()->GetDBManager()->Execute(statement);

      AuditDescription described(_T("hm_migrationjobs"), auditName);
      return Application::Instance()->GetDBManager()->Execute(statement);
   }

   bool
   MigrationStore::MarkJobRunning(__int64 jobId)
   {
      SQLCommand command("update hm_migrationjobs set mjstate = @RUNNING where mjid = @ID");
      command.AddParameter("@RUNNING", (int) JobRunning);
      command.AddParameter("@ID", jobId);
      return Application::Instance()->GetDBManager()->Execute(command);
   }

   bool
   MigrationStore::ChangeJobStates(__int64 migrationId, int fromState, int toState)
   {
      SQLCommand command("update hm_migrationjobs set mjstate = @TOSTATE, mjnextattempt = 0 where mjmigrationid = @ID and mjstate = @FROMSTATE");
      command.AddParameter("@TOSTATE", toState);
      command.AddParameter("@ID", migrationId);
      command.AddParameter("@FROMSTATE", fromState);
      return Application::Instance()->GetDBManager()->Execute(command);
   }

   bool
   MigrationStore::SaveSecret(__int64 jobId, const String &secret, bool clearSignIn)
   {
      if (clearSignIn)
      {
         SQLCommand command("update hm_migrationjobs set mjsourcesecret = @SECRET, mjsigninhash = @HASH where mjid = @ID");
         command.AddParameter("@SECRET", secret.IsEmpty() ? String() : Crypt::Instance()->ProtectSecret(secret));
         command.AddParameter("@HASH", String());
         command.AddParameter("@ID", jobId);
         return Application::Instance()->GetDBManager()->Execute(command);
      }

      SQLCommand command("update hm_migrationjobs set mjsourcesecret = @SECRET where mjid = @ID");
      command.AddParameter("@SECRET", secret.IsEmpty() ? String() : Crypt::Instance()->ProtectSecret(secret));
      command.AddParameter("@ID", jobId);
      return Application::Instance()->GetDBManager()->Execute(command);
   }

   bool
   MigrationStore::SaveSignInHash(__int64 jobId, const String &hash)
   {
      SQLCommand command("update hm_migrationjobs set mjsigninhash = @HASH where mjid = @ID");
      command.AddParameter("@HASH", hash);
      command.AddParameter("@ID", jobId);
      return Application::Instance()->GetDBManager()->Execute(command);
   }

   int
   MigrationStore::RequeueRunningJobs()
   {
      std::vector<MigrationJobRecord> jobs;
      if (!ListActiveJobs(jobs))
         return 0;

      int requeued = 0;
      for (const MigrationJobRecord &job : jobs)
      {
         if (job.state != JobRunning)
            continue;

         SQLCommand command("update hm_migrationjobs set mjstate = @QUEUED where mjid = @ID and mjstate = @RUNNING");
         command.AddParameter("@QUEUED", (int) JobQueued);
         command.AddParameter("@ID", job.id);
         command.AddParameter("@RUNNING", (int) JobRunning);
         if (Application::Instance()->GetDBManager()->Execute(command))
            requeued++;
      }

      return requeued;
   }

   bool
   MigrationStore::CountActiveJobs(__int64 migrationId, int &count)
   {
      count = 0;

      SQLCommand command("select count(*) as activejobs from hm_migrationjobs where mjmigrationid = @ID and (mjstate = @QUEUED or mjstate = @RUNNING or mjstate = @PAUSED)");
      command.AddParameter("@ID", migrationId);
      command.AddParameter("@QUEUED", (int) JobQueued);
      command.AddParameter("@RUNNING", (int) JobRunning);
      command.AddParameter("@PAUSED", (int) JobPaused);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset)
         return false;

      if (!recordset->IsEOF())
         count = recordset->GetLongValue("activejobs");
      return true;
   }

   bool
   MigrationStore::FindActiveJob(__int64 targetAccountId, const String &sourceUser, __int64 &jobId)
   {
      jobId = 0;

      String lowered = sourceUser;
      lowered.MakeLower();

      SQLCommand command("select mjid from hm_migrationjobs where mjtargetaccountid = @ACCOUNTID and lower(mjsourceuser) = @SOURCEUSER and (mjstate = @QUEUED or mjstate = @RUNNING or mjstate = @PAUSED)");
      command.AddParameter("@ACCOUNTID", targetAccountId);
      command.AddParameter("@SOURCEUSER", lowered);
      command.AddParameter("@QUEUED", (int) JobQueued);
      command.AddParameter("@RUNNING", (int) JobRunning);
      command.AddParameter("@PAUSED", (int) JobPaused);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset)
         return false;

      if (!recordset->IsEOF())
         jobId = recordset->GetInt64Value("mjid");
      return true;
   }

   bool
   MigrationStore::ListDoneJobsOfAccount(__int64 targetAccountId, std::vector<MigrationJobRecord> &jobs)
   {
      jobs.clear();

      SQLCommand command(String(AnsiString("select ") + JobColumns + ", mjreport from hm_migrationjobs where mjtargetaccountid = @ACCOUNTID and mjstate = @DONE order by mjid asc"));
      command.AddParameter("@ACCOUNTID", targetAccountId);
      command.AddParameter("@DONE", (int) JobDone);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset)
         return false;

      while (!recordset->IsEOF())
      {
         MigrationJobRecord job;
         ReadJob(recordset, job);
         job.report = recordset->GetStringValue("mjreport");
         jobs.push_back(job);
         recordset->MoveNext();
      }

      return true;
   }

   bool
   MigrationStore::CancelJobsOfAccount(__int64 accountId, const String &reason)
   {
      // Asked first, so that deleting an account no migration names - nearly
      // every account ever deleted - writes nothing, and puts no row in the
      // audit trail for a change that changed nothing.
      SQLCommand count("select count(*) as activejobs from hm_migrationjobs where mjtargetaccountid = @ACCOUNTID and (mjstate = @QUEUED or mjstate = @RUNNING or mjstate = @PAUSED)");
      count.AddParameter("@ACCOUNTID", accountId);
      count.AddParameter("@QUEUED", (int) JobQueued);
      count.AddParameter("@RUNNING", (int) JobRunning);
      count.AddParameter("@PAUSED", (int) JobPaused);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(count);
      if (!recordset)
         return false;
      if (recordset->IsEOF() || recordset->GetLongValue("activejobs") == 0)
         return true;

      SQLCommand command("update hm_migrationjobs set mjstate = @CANCELLED, mjlasterror = @REASON, mjnextattempt = 0, mjfinishedat = @NOW where mjtargetaccountid = @ACCOUNTID and (mjstate = @QUEUED or mjstate = @RUNNING or mjstate = @PAUSED)");
      command.AddParameter("@CANCELLED", (int) JobCancelled);
      command.AddParameter("@REASON", Fit(reason, MaximumErrorLength));
      command.AddParameter("@NOW", Now());
      command.AddParameter("@ACCOUNTID", accountId);
      command.AddParameter("@QUEUED", (int) JobQueued);
      command.AddParameter("@RUNNING", (int) JobRunning);
      command.AddParameter("@PAUSED", (int) JobPaused);
      return Application::Instance()->GetDBManager()->Execute(command);
   }

   bool
   MigrationStore::ListFolders(__int64 jobId, std::vector<MigrationFolderRecord> &folders)
   {
      folders.clear();

      SQLCommand command(String(AnsiString("select ") + FolderColumns + " from hm_migrationfolders where mfjobid = @JOBID order by mfid asc"));
      command.AddParameter("@JOBID", jobId);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset)
         return false;

      while (!recordset->IsEOF())
      {
         MigrationFolderRecord folder;
         ReadFolder(recordset, folder);
         folders.push_back(folder);
         recordset->MoveNext();
      }

      return true;
   }

   bool
   MigrationStore::SaveFolder(MigrationFolderRecord &folder)
   {
      SQLStatement statement;
      statement.SetTable("hm_migrationfolders");
      statement.AddColumnInt64("mfjobid", folder.jobId);
      statement.AddColumn("mffolder", Fit(folder.folder, MaximumFolderLength));
      statement.AddColumn("mftargetfolder", Fit(folder.targetFolder, MaximumFolderLength));
      statement.AddColumn("mfuidvalidity", Fit(folder.uidValidity, 64));
      statement.AddColumnInt64("mflastuid", folder.lastUid);
      statement.AddColumn("mfmessages", (long) folder.messages);
      statement.AddColumn("mfdone", (long) (folder.done ? 1 : 0));

      if (folder.id == 0)
      {
         statement.SetStatementType(SQLStatement::STInsert);
         statement.SetIdentityColumn("mfid");

         __int64 id = 0;
         if (!Application::Instance()->GetDBManager()->Execute(statement, &id) || id <= 0)
            return false;

         folder.id = id;
         return true;
      }

      statement.SetStatementType(SQLStatement::STUpdate);

      String where;
      where.Format(_T("mfid = %I64d"), folder.id);
      statement.SetWhereClause(where);

      return Application::Instance()->GetDBManager()->Execute(statement);
   }

   bool
   MigrationStore::DeleteFolders(__int64 jobId)
   {
      SQLCommand command("delete from hm_migrationfolders where mfjobid = @JOBID");
      command.AddParameter("@JOBID", jobId);
      return Application::Instance()->GetDBManager()->Execute(command);
   }
}
