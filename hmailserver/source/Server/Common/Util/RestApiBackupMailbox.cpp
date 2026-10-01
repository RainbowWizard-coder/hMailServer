// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// RESTORING ONE MAILBOX, OR ONE MESSAGE, FROM A BACKUP - THE FOUR ROUTES
//
//   GET  /api/v1/backup/archives                            the archives in the destination
//   GET  /api/v1/backup/{archive}/mailboxes/{address}       an account as an archive holds it
//   POST /api/v1/backup/{archive}/mailboxes/{address}/restore
//   GET  /api/v1/backup/restore-mailbox/{job}               what became of one
//
// The whole restore's shape, kept: everything that can be established is
// established here, on the HTTP worker, bounded at the same two minutes
// (HandleBackupRestore_'s RESTORE_ARCHIVE_OPEN_TIMEOUT_MS, restated below for
// the same reason), so that a wrong request is a 400 or a 404 with the reason
// in the administrator's hands and nothing on the server has been touched; the
// restore itself then runs on the maintenance queue under the backup manager's
// one run slot, and what it did is read back from its job. The operation is
// MailboxRestore; this file is its JSON.

#include "StdAfx.h"
#include "RestApiServer.h"
#include "HttpServer.h"
#include "JsonDocument.h"
#include "Unicode.h"
#include "AuditTrail.h"
#include "Compression.h"
#include "FileUtilities.h"
#include "Time.h"
#include "../Application/BackupManager.h"
#include "../Application/BackupRetention.h"
#include "../Application/Configuration.h"
#include "../Application/Logger.h"
#include "../Application/MailboxRestore.h"
#include "../BO/Account.h"
#include "../Cache/CacheContainer.h"
#include "DiscoveryHolds.h"

#include <boost/filesystem.hpp>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // The same bound HandleBackupRestore_ puts on reading an archive's index,
      // for the same reason: this runs on one of four HTTP workers, and an
      // archive on a drive that has stopped answering must not hold one for
      // as long as the drive stays that way. The listing's staging of the
      // account's files for subjects is under the same bound.
      const unsigned int MAILBOX_RESTORE_OPEN_TIMEOUT_MS = 120000;

      // The header an encrypted archive's passphrase travels in on the GET
      // listing, where there is no body: never the query string, which is
      // logged by proxies and kept in histories.
      const char *const PASSPHRASE_HEADER = "x-hmailserver-backup-passphrase";
   }

   AnsiString
   RestApiServer::BackupJsonQuoted_(const String &value)
   {
      return "\"" + JsonEscape_(Utf8_(value)) + "\"";
   }

   HttpResponse
   RestApiServer::BackupRefusal_(int statusCode, const String &reason)
   {
      AnsiString body;
      body.Format("{\"error\":\"%hs\"}", JsonEscape_(Utf8_(reason)).c_str());
      return BuildResponse_(statusCode, body);
   }

   HttpResponse
   RestApiServer::HandleBackupArchiveList_()
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // The archives in the backup destination, newest first, read by the same
   // rule retention reads them by: only names this server generates. An
   // administrator's own copies in the same directory are not listed, and are
   // still restorable by their path inside it.
   //---------------------------------------------------------------------------()
   {
      String destination = Configuration::Instance()->GetBackupDestination();

      while (destination.EndsWith(FileUtilities::PathSeparator))
         destination = destination.Left(destination.GetLength() - 1);

      AnsiString body = "{\"destination\":" + BackupJsonQuoted_(destination) + ",\"archives\":[";

      if (!destination.IsEmpty() && FileUtilities::DirectoryExists(destination))
      {
         std::vector<BackupArchive> archives = BackupRetention::ListArchives(destination);

         bool first = true;

         for (size_t i = archives.size(); i > 0; i--)
         {
            const BackupArchive &archive = archives[i - 1];
            String path = FileUtilities::Combine(destination, archive.file_name);

            if (!first)
               body += ",";
            first = false;

            // Read from the archive's own header, as the restore's audit row
            // is; it costs a few bytes off the front of each file.
            const bool encrypted = Compression::GetArchiveEncryption(path) == Compression::EncryptionHeaders;

            // A whole message store's archive is routinely past what a long
            // holds on Windows, and the picker shows this number.
            boost::system::error_code sizeError;
            const boost::uintmax_t size = boost::filesystem::file_size(boost::filesystem::path(path.c_str()), sizeError);

            AnsiString entry;
            entry.Format("{\"name\":%hs,\"path\":%hs,\"created\":%hs,\"size\":%I64u,\"encrypted\":%hs}",
               BackupJsonQuoted_(archive.file_name).c_str(), BackupJsonQuoted_(path).c_str(),
               BackupJsonQuoted_(Time::GetTimeStampFromDateTime(archive.created)).c_str(),
               (unsigned __int64) (sizeError ? 0 : size), encrypted ? "true" : "false");

            body += entry;
         }
      }

      body += "]}";

      return BuildResponse_(200, body);
   }

   HttpResponse
   RestApiServer::HandleBackupMailboxList_(const Route &route, const AnsiString &request)
   {
      String archiveNamed;
      String address;
      Unicode::MultiByteToWide(route.identifier, archiveNamed);
      Unicode::MultiByteToWide(route.name, address);

      String passphrase;
      Unicode::MultiByteToWide(GetHeader_(request, PASSPHRASE_HEADER), passphrase);

      String folderFilter;
      Unicode::MultiByteToWide(QueryParameter_(route.query, "folder"), folderFilter);

      String archiveFile;
      String failureReason;
      bool notFound = false;

      if (!MailboxRestore::ResolveArchive(archiveNamed, archiveFile, notFound, failureReason))
         return BackupRefusal_(notFound ? 404 : 400, failureReason);

      ArchivedMailbox mailbox;
      bool openTimedOut = false;
      bool accountNotFound = false;

      if (!MailboxRestore::ReadMailbox(archiveFile, passphrase, address, MAILBOX_RESTORE_OPEN_TIMEOUT_MS, &openTimedOut,
                                       mailbox, accountNotFound, failureReason))
      {
         return BackupRefusal_(openTimedOut ? 504 : (accountNotFound ? 404 : 400), failureReason);
      }

      if (!folderFilter.IsEmpty())
      {
         std::vector<ArchivedFolder> kept;

         for (size_t i = 0; i < mailbox.folders.size(); i++)
         {
            if (MailboxRestore::FolderPathText(mailbox.folders[i].path).CompareNoCase(folderFilter) == 0)
               kept.push_back(mailbox.folders[i]);
         }

         mailbox.folders = kept;
      }

      // The subjects, when the listing is small enough to read them for;
      // the listing is good without them and says why they are missing.
      String subjectsNote;
      const bool subjects = MailboxRestore::ReadSubjects(archiveFile, passphrase, mailbox, MAILBOX_RESTORE_OPEN_TIMEOUT_MS, subjectsNote);

      // Senders, dates and subjects of somebody's mail as it was: a read of the
      // mailbox, on the audit trail as the live mailbox's are
      // (RestApiAccountResources.cpp, RecordMailRead_). A folder asked for is
      // not named in the row, as no folder is: its name is the mailbox's.
      const String listed = folderFilter.IsEmpty()
         ? _T("its folders and messages in the backup ") + archiveFile + _T(" listed")
         : _T("the messages of one of its folders in the backup ") + archiveFile + _T(" listed");

      String archiveAddress = address;
      archiveAddress.ToLower();
      RecordMailRead_(_T("account"), address, _T("mailbox-read"), listed,
                      _T("backup ") + archiveFile + _T(" ") + archiveAddress + _T(" ") + folderFilter);

      return BuildResponse_(200, Utf8_(MailboxRestore::ListingJson(archiveFile, mailbox, subjects, subjectsNote)));
   }

   HttpResponse
   RestApiServer::HandleBackupMailboxRestore_(const Route &route, const AnsiString &requestBody)
   {
      std::shared_ptr<BackupManager> manager = Application::Instance()->GetBackupManager();
      if (!manager)
         return BuildResponse_(503, "{\"error\":\"the backup manager is not running\"}");

      MailboxRestore::Request request;
      Unicode::MultiByteToWide(route.identifier, request.archive);
      Unicode::MultiByteToWide(route.name, request.address);

      if (!requestBody.IsEmpty())
      {
         JsonValue document;
         std::string parseError;
         if (!JsonValue::Parse(std::string(requestBody.c_str(), requestBody.GetLength()), document, parseError) || !document.IsObject())
            return BackupRefusal_(400, _T("the body must be a JSON object"));

         const std::vector<std::pair<std::string, JsonValue> > &members = document.Members();

         for (size_t i = 0; i < members.size(); i++)
         {
            const std::string &key = members[i].first;
            const JsonValue &value = members[i].second;

            if (key == "mode")
            {
               if (!value.IsString())
                  return BackupRefusal_(400, _T("mode must be merge or replace"));

               String mode;
               Unicode::MultiByteToWide(AnsiString(value.AsString().c_str()), mode);

               if (!MailboxRestore::ParseMode(mode, request.mode))
                  return BackupRefusal_(400, _T("mode must be merge or replace"));

               request.mode_given = true;
               continue;
            }

            if (key == "message")
            {
               if (value.IsNull())
                  continue;

               if (!value.IsNumber() || value.AsNumber() < 1.0 || value.AsNumber() != (double) (__int64) value.AsNumber())
                  return BackupRefusal_(400, _T("message must be a message id from the archive's listing"));

               request.message_id = (__int64) value.AsNumber();
               continue;
            }

            if (key == "folder" || key == "passphrase")
            {
               if (value.IsNull())
                  continue;

               if (!value.IsString())
                  return BackupRefusal_(400, String(key.c_str()) + _T(" must be a string"));

               String text;
               Unicode::MultiByteToWide(AnsiString(value.AsString().c_str()), text);

               if (key == "folder")
                  request.folder = text;
               else
                  request.passphrase = text;

               continue;
            }

            return BackupRefusal_(400, String(key.c_str()) + _T(" is not a field of a mailbox restore: the fields are mode, message, folder and passphrase"));
         }
      }

      // Replace deletes what is in the mailbox now, which a legal hold forbids:
      // refused before the archive is opened, so the answer does not wait on it
      // (docs/Discovery.md). Merge and a single message delete nothing.
      if (request.message_id == 0 && request.mode == MailboxRestore::ModeReplace)
      {
         std::shared_ptr<const Account> held = CacheContainer::Instance()->GetAccount(request.address);
         if (held && DiscoveryHolds::IsAccountHeld(held->GetID()))
            return BackupRefusal_(409, DiscoveryHolds::ReplaceRefusal(held->GetAddress()));
      }

      int statusCode = 400;
      String failureReason;

      std::shared_ptr<MailboxRestore> job = MailboxRestore::Prepare(request, MAILBOX_RESTORE_OPEN_TIMEOUT_MS, statusCode, failureReason);

      if (!job)
         return BackupRefusal_(statusCode, failureReason);

      if (!manager->StartMailboxRestore(job, failureReason))
      {
         // The job was registered by Prepare; it will not run, and its status
         // must say so rather than "queued" for the life of the process.
         MailboxRestore::MarkNotStarted(job->GetJob(), failureReason);

         AnsiString conflict;
         conflict.Format("{\"error\":\"%hs\",\"status\":\"%hs\"}",
            JsonEscape_(Utf8_(failureReason)).c_str(), JsonEscape_(Utf8_(manager->GetStatus())).c_str());
         return BuildResponse_(409, conflict);
      }

      // The archive's path and the account, never the passphrase.
      LOG_APPLICATION("RestApi: Mailbox restore " + StringParser::IntToString(job->GetJob()) + " started: " + job->Describe() + ".");

      // And the audit trail, on this thread, while the actor ProcessRequest_
      // installed is still on it - the request, as a whole restore's is
      // recorded, since the restore runs on the maintenance work queue with
      // nobody on it. See HandleBackupRestore_.
      AuditTrail::Instance()->RecordEvent(_T("backup"), job->GetRequest().archive_file, _T("mailbox restore started"),
         job->Describe());

      AnsiString body;
      body.Format("{\"job\":%d,\"state\":\"queued\",\"archive\":%hs,\"address\":%hs,\"mode\":%hs,\"message\":%I64d,\"status\":\"/api/v1/backup/restore-mailbox/%d\"}",
         job->GetJob(), BackupJsonQuoted_(job->GetRequest().archive_file).c_str(), BackupJsonQuoted_(job->GetRequest().address).c_str(),
         BackupJsonQuoted_(job->GetRequest().message_id > 0 ? String(_T("message")) : MailboxRestore::ModeName(job->GetRequest().mode)).c_str(),
         job->GetRequest().message_id, job->GetJob());

      return BuildResponse_(202, body);
   }

   HttpResponse
   RestApiServer::HandleBackupMailboxRestoreStatus_(const Route &route)
   {
      MailboxRestore::Outcome outcome;

      if (route.record_id <= 0 || route.record_id > 0x7fffffff || !MailboxRestore::GetOutcome((int) route.record_id, outcome))
         return BackupRefusal_(404, _T("no such mailbox restore job; the last fifty are kept for the life of the process"));

      return BuildResponse_(200, Utf8_(MailboxRestore::OutcomeJson(outcome)));
   }
}
