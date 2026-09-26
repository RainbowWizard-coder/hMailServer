// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The REST API's operator tools: the three things an administrator could do
// only from a COM script until now. See RestApiServer.h.
//
//    POST   /api/v1/accounts/{address}/export     the mailbox as .eml files on the server's disk
//    POST   /api/v1/addresses/erase               every trace of an address the account's deletion left
//    GET    /api/v1/administrator/second-factor   whether the administrator password has a second factor
//    POST   /api/v1/administrator/second-factor   enrol one; the answer carries the otpauth URI, once
//    DELETE /api/v1/administrator/second-factor   disable it
//
// All four are the administrator password's alone (IsApiKeyRoute_): the
// export writes wherever on the server's disk the caller names, the erasure
// reaches every domain's tables, and the second factor is the credential
// store itself. A key or a role, however wide, is refused them before any
// permission is consulted.
#include "StdAfx.h"
#include "RestApiServer.h"
#include "AddressTraceEraser.h"
#include "JsonDocument.h"
#include "MessageExport.h"
#include "PersonExport.h"
#include "FileUtilities.h"
#include "GUIDCreator.h"
#include "Totp.h"
#include "Unicode.h"
#include "../Application/IniFileSettings.h"
#include "../BO/Account.h"
#include "../Persistence/PersistentAccount.h"

#include <mutex>
#include <set>
#include <string>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      bool ParseObject(const AnsiString &requestBody, JsonValue &body)
      {
         std::string parseError;
         std::string text(requestBody.c_str(), (size_t) requestBody.GetLength());
         return JsonValue::Parse(text, body, parseError) && body.IsObject();
      }

      String StringMember(const JsonValue &body, const char *key)
      {
         const JsonValue *member = body.Get(key);
         if (!member || !member->IsString())
            return String();

         String value;
         Unicode::MultiByteToWide(AnsiString(member->AsString().c_str()), value);
         return value;
      }

      // The archives being built for a download, by account. The HTTP server
      // has four workers, which serve the webmail and the whole API as well,
      // and building an archive holds one for as long as it takes to read and
      // deflate the whole mailbox: one at a time for an account, and two at a
      // time on the server, so that downloads can never take every worker.
      const size_t MaxArchiveDownloadsAtOnce = 2;
      std::mutex archiveDownloadsMutex;
      std::set<__int64> archiveDownloads;

      class ArchiveDownloadSlot
      {
      public:
         enum Outcome { Taken, AccountBusy, ServerBusy };

         explicit ArchiveDownloadSlot(__int64 accountId) :
            account_id_(accountId),
            outcome_(Taken)
         {
            std::lock_guard<std::mutex> guard(archiveDownloadsMutex);

            if (archiveDownloads.find(accountId) != archiveDownloads.end())
               outcome_ = AccountBusy;
            else if (archiveDownloads.size() >= MaxArchiveDownloadsAtOnce)
               outcome_ = ServerBusy;
            else
               archiveDownloads.insert(accountId);
         }

         ~ArchiveDownloadSlot()
         {
            if (outcome_ != Taken)
               return;

            std::lock_guard<std::mutex> guard(archiveDownloadsMutex);
            archiveDownloads.erase(account_id_);
         }

         Outcome GetOutcome() const { return outcome_; }

      private:
         ArchiveDownloadSlot(const ArchiveDownloadSlot &);
         ArchiveDownloadSlot &operator=(const ArchiveDownloadSlot &);

         __int64 account_id_;
         Outcome outcome_;
      };
   }

   HttpResponse
   RestApiServer::HandleAccountExport_(const Route &route, const AnsiString &requestBody)
   {
      std::shared_ptr<Account> account = std::shared_ptr<Account>(new Account());
      if (!PersistentAccount::ReadObject(account, String(route.identifier)) || account->GetID() == 0)
         return BuildResponse_(404, "{\"error\":\"account not found\"}");

      // A zero-access mailbox's mail is not the administrator's to export:
      // the files would be ciphertext, and the row says the read is refused
      // (RestApiZeroAccess.cpp). The person's own export, over /api/v1/me,
      // is theirs and stays open.
      {
         HttpResponse refusal;
         if (RefuseZeroAccessRead_(account, refusal))
            return refusal;
      }

      JsonValue body;
      if (!ParseObject(requestBody, body))
         return BuildResponse_(400, "{\"error\":\"The body must be a JSON object with directory.\"}");

      String directory = StringMember(body, "directory");
      directory.Trim();
      String archive = StringMember(body, "archive");
      archive.Trim();

      if (directory.IsEmpty() && archive.IsEmpty())
         return BuildResponse_(400, "{\"error\":\"directory or archive is required: a directory on the server's disk the .eml files are written into, or the path of the .zip archive to write everything the server holds on the account into.\"}");

      if (!archive.IsEmpty())
      {
         // One archive rather than a directory of files: everything the server
         // holds on the person, in the form GET /export.zip downloads, written
         // to the server's disk for the sizes a download will not carry.
         PersonExport::Summary summary;
         String error;
         if (!PersonExport::ExportArchive(account, archive, summary, error))
            return BuildResponse_(500, "{\"error\":\"" + JsonEscape_(Utf8_(error)) + "\"}");

         String logMessage;
         logMessage.Format(_T("Exported everything held on %s - %d message(s), %d contact(s), %d calendar object(s), %d log line(s) - to an archive on an operator's request over the REST API."),
                           account->GetAddress().c_str(), summary.messages, summary.contacts, summary.calendarObjects, summary.logLines);
         LOG_APPLICATION(logMessage);

         AnsiString answer;
         answer.Format("{\"exported\":%d,\"folders\":%d,\"contacts\":%d,\"calendar_objects\":%d,\"log_lines\":%d,\"archive\":\"",
                       summary.messages, summary.folders, summary.contacts, summary.calendarObjects, summary.logLines);
         return BuildResponse_(200, answer + JsonEscape_(Utf8_(archive)) + "\"}");
      }

      int exported = 0;
      String error;
      if (!MessageExport::ExportAccount(account, directory, exported, error))
         return BuildResponse_(500, "{\"error\":\"" + JsonEscape_(Utf8_(error)) + "\"}");

      String logMessage;
      logMessage.Format(_T("Exported %d message(s) of %s on an operator's request over the REST API."), exported, account->GetAddress().c_str());
      LOG_APPLICATION(logMessage);

      AnsiString answer;
      answer.Format("{\"exported\":%d,\"directory\":\"", exported);
      return BuildResponse_(200, answer + JsonEscape_(Utf8_(directory)) + "\"}");
   }

   // The archive built into the temp directory, read, and removed: what the
   // Deck's link and the person's own page download. In memory once, which is
   // why there is a ceiling; past it, the operator writes it to disk. Two
   // things are refused before any work is done: a mailbox whose mail alone
   // is past four times the ceiling - mail does not deflate that far as a
   // rule, and building it would take a worker and the memory for nothing -
   // and a second archive while one is being built for the same account, or
   // while two are being built on the server (ArchiveDownloadSlot).
   HttpResponse
   RestApiServer::ArchiveDownload_(std::shared_ptr<Account> account)
   {
      const size_t Ceiling = 500 * 1024 * 1024;
      const __int64 MailCeiling = 4 * (__int64) Ceiling;

      ArchiveDownloadSlot slot(account->GetID());
      if (slot.GetOutcome() == ArchiveDownloadSlot::AccountBusy)
         return BuildResponse_(409, "{\"error\":\"An archive of this account is already being built. Wait for that download to finish.\"}");

      if (slot.GetOutcome() == ArchiveDownloadSlot::ServerBusy)
      {
         HttpResponse busy = BuildResponse_(429, "{\"error\":\"The server is building as many archives as it builds at once. Try again in a minute.\"}");
         busy.extra_headers += "Retry-After: 60\r\n";
         return busy;
      }

      const __int64 mailBytes = PersistentAccount::GetMessageBoxSize(account->GetID());
      if (mailBytes > MailCeiling)
         return BuildResponse_(413, "{\"error\":\"The mail alone is past 2 GB, which as an archive would be larger than the 500 MB a download carries; an administrator can write it to the server's disk with POST /api/v1/accounts/{address}/export and archive.\"}");

      String temporary = IniFileSettings::Instance()->GetTempDirectory();
      if (temporary.IsEmpty())
         return BuildResponse_(500, "{\"error\":\"The server has no temporary directory to build the archive in.\"}");

      String zipPath = temporary + FileUtilities::PathSeparator + _T("export-") + GUIDCreator::GetGUID() + _T(".zip");

      PersonExport::Summary summary;
      String error;
      if (!PersonExport::ExportArchive(account, zipPath, summary, error))
      {
         FileUtilities::DeleteFile(zipPath);
         return BuildResponse_(500, "{\"error\":\"" + JsonEscape_(Utf8_(error)) + "\"}");
      }

      unsigned __int64 size = 0;
      FileUtilities::FileSize64(zipPath, size);
      if (size > (unsigned __int64) Ceiling)
      {
         FileUtilities::DeleteFile(zipPath);
         return BuildResponse_(413, "{\"error\":\"The archive is larger than 500 MB, which a download does not carry; an administrator can write it to the server's disk with POST /api/v1/accounts/{address}/export and archive.\"}");
      }

      AnsiString bytes;
      {
         boost::filesystem::path path(zipPath.begin(), zipPath.end());
         boost::filesystem::ifstream in(path, std::ios::in | std::ios::binary);
         if (in)
         {
            std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            bytes = content;
         }
      }
      FileUtilities::DeleteFile(zipPath);

      if (bytes.empty())
         return BuildResponse_(500, "{\"error\":\"The archive could not be read back.\"}");

      String logMessage;
      logMessage.Format(_T("Everything held on %s - %d message(s), %d contact(s), %d calendar object(s), %d log line(s) - was downloaded as one archive."),
                        account->GetAddress().c_str(), summary.messages, summary.contacts, summary.calendarObjects, summary.logLines);
      LOG_APPLICATION(logMessage);

      std::string fileName = Utf8_(account->GetAddress());
      for (size_t i = 0; i < fileName.size(); i++)
      {
         if (fileName[i] == '"' || fileName[i] == '\\' || fileName[i] == '/' || fileName[i] < 0x20)
            fileName[i] = '_';
      }

      HttpResponse response;
      response.status = 200;
      response.content_type = "application/zip";
      response.body = bytes;
      response.extra_headers =
         "Content-Disposition: attachment; filename=\"" + AnsiString(fileName) + ".zip\"\r\n"
         "X-Content-Type-Options: nosniff\r\n"
         "Content-Security-Policy: sandbox\r\n"
         "Cache-Control: no-store\r\n";
      return response;
   }

   HttpResponse
   RestApiServer::HandleAccountExportArchive_(const Route &route)
   {
      std::shared_ptr<Account> account = std::shared_ptr<Account>(new Account());
      if (!PersistentAccount::ReadObject(account, String(route.identifier)) || account->GetID() == 0)
         return BuildResponse_(404, "{\"error\":\"account not found\"}");

      {
         HttpResponse refusal;
         if (RefuseZeroAccessRead_(account, refusal))
            return refusal;
      }

      return ArchiveDownload_(account);
   }

   HttpResponse
   RestApiServer::HandleMeExportArchive_(const Caller &caller)
   {
      if (!caller.account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      // Everything the account holds, in one download, is not for a program's
      // credential: an app password is what a phone's mail client keeps, it
      // passes the second factor by design, and the archive is the whole
      // account - the settings, the rules, the address book and every folder.
      // As on the other self-service routes that act for the account holder.
      {
         HttpResponse refusal;
         if (RefuseAppPasswordCaller_(caller, "downloading the account's export", refusal))
            return refusal;
      }

      // Read afresh rather than the session's const copy: the export walks the
      // folder tree and the stores through the account object.
      std::shared_ptr<Account> account = std::shared_ptr<Account>(new Account());
      if (!PersistentAccount::ReadObject(account, caller.account->GetAddress()) || account->GetID() == 0)
         return BuildResponse_(404, "{\"error\":\"account not found\"}");

      return ArchiveDownload_(account);
   }

   HttpResponse
   RestApiServer::HandleAddressErase_(const AnsiString &requestBody)
   {
      JsonValue body;
      if (!ParseObject(requestBody, body))
         return BuildResponse_(400, "{\"error\":\"The body must be a JSON object with address and, optionally, include_archive.\"}");

      String address = StringMember(body, "address");
      address.Trim();
      if (address.IsEmpty() || address.Find(_T("@")) < 1)
         return BuildResponse_(400, "{\"error\":\"address is required: a full e-mail address whose traces are to be erased.\"}");

      const JsonValue *archive = body.Get("include_archive");
      if (archive && !archive->IsNull() && !archive->IsBool())
         return BuildResponse_(400, "{\"error\":\"include_archive must be true or false.\"}");
      const bool includeArchive = archive && archive->IsBool() && archive->AsBool();

      const JsonValue *logs = body.Get("include_logs");
      if (logs && !logs->IsNull() && !logs->IsBool())
         return BuildResponse_(400, "{\"error\":\"include_logs must be true or false.\"}");
      const bool includeLogs = logs && logs->IsBool() && logs->AsBool();

      int removed = 0;
      int held = 0;
      if (!AddressTraceEraser::Erase(address, includeArchive, includeLogs, removed, held))
         return BuildResponse_(500, "{\"error\":\"Some traces could not be removed - the hMailServer error log names the stores to retry. What could be removed was; run the erasure again once the cause is dealt with.\"}");

      // The line names no address: with include_logs every line that named it
      // has just been rewritten, and this one would put it back.
      String logMessage;
      logMessage.Format(_T("Erased %d trace(s) of an address on an operator's request over the REST API%s%s; %d left under a legal hold."), removed,
         includeArchive ? _T(", the archive included") : _T(""), includeLogs ? _T(", the logs rewritten") : _T(""), held);
      LOG_APPLICATION(logMessage);

      // held: what a legal hold kept in place (docs/Discovery.md) - said, so that
      // the operator answering the person can tell them what was kept and why.
      AnsiString answer;
      answer.Format("{\"removed\":%d,\"include_archive\":%s,\"include_logs\":%s,\"held\":%d}", removed, includeArchive ? "true" : "false", includeLogs ? "true" : "false", held);
      return BuildResponse_(200, answer);
   }

   HttpResponse
   RestApiServer::HandleAdministratorSecondFactor_(const Route &route)
   {
      auto ini = IniFileSettings::Instance();

      if (route.kind == RouteAdminSecondFactorGet)
         return BuildResponse_(200, AnsiString("{\"enrolled\":") + (ini->IsAdministratorTotpEnrolled() ? "true" : "false") + "}");

      // [Security] lives in hMailServer.INI, which the Windows service's own
      // account may not write (docs/ServiceAccount.md): 409, with what to do.
      if (!IniFileSettings::InitializationFileWritable())
      {
#ifdef HM_PLATFORM_POSIX
         const String insteadDo = _T("To turn it off, root deletes AdministratorTotpSecret from [Security] in the file and restarts the service. Enrolling one needs a file the service's account may write, which the packages do not give it.");
#else
         const String insteadDo = _T("To turn it off, an administrator deletes AdministratorTotpSecret from [Security] in the file with an editor run as an administrator and restarts the service. To enrol one, move the service to LocalSystem for the enrolment (hMailServer.exe --service-account localsystem /Register from an elevated prompt, restart the service), enrol, then move it back (hMailServer.exe --service-account virtual /Register, restart).");
#endif
         const String refusal = IniFileSettings::InitializationFileRefusal(_T("The administrator's second factor"), insteadDo);
         return BuildResponse_(409, "{\"error\":\"" + JsonEscape_(Utf8_(refusal)) + "\"}");
      }

      if (route.kind == RouteAdminSecondFactorDisable)
      {
         if (!ini->SetAdministratorTotpSecret(String()))
            return BuildResponse_(500, "{\"error\":\"The secret could not be removed from hMailServer.INI. Nothing has been changed; see the hMailServer error log.\"}");

         SecondFactorGuard::ForgetOwner(_T("administrator"));

         LOG_APPLICATION(_T("The administrator's second factor was disabled over the REST API."));
         return BuildResponse_(200, "{\"enrolled\":false}");
      }

      // Enrol. A second factor guards a password: with none set, anyone
      // authenticates anonymously and a code would guard nothing. And one that
      // is already enrolled is not silently replaced - every authenticator app
      // that holds the old secret would stop working without a word - so a
      // second enrolment is refused until the first is disabled.
      if (ini->GetAdministratorPassword().IsEmpty())
         return BuildResponse_(409, "{\"error\":\"Set the administrator password first. A second factor protects a password, and there is none to protect.\"}");

      if (ini->IsAdministratorTotpEnrolled())
         return BuildResponse_(409, "{\"error\":\"A second factor is already enrolled. Disable it first: a second enrolment would replace the secret every authenticator app already holds.\"}");

      const AnsiString secret = Totp::GenerateSecret();
      if (secret.IsEmpty())
         return BuildResponse_(500, "{\"error\":\"The random number generator failed, so no second factor was enrolled. Nothing has been stored.\"}");

      if (!ini->SetAdministratorTotpSecret(String(secret)))
         return BuildResponse_(500, "{\"error\":\"The secret could not be written to hMailServer.INI. Nothing has been changed; see the hMailServer error log.\"}");

      // The codes, wrong or taken, that were about the secret this replaces
      // (SecondFactorGuard) are forgotten with it.
      SecondFactorGuard::ForgetOwner(_T("administrator"));

      // The URI carries the secret, and this answer is the only moment it
      // leaves the server. Enforced from this moment: the next logon with the
      // administrator password needs the code.
      LOG_APPLICATION(_T("The administrator's second factor was enrolled over the REST API."));
      return BuildResponse_(200, "{\"enrolled\":true,\"uri\":\"" + JsonEscape_(Totp::BuildOtpAuthUri("Administrator", secret)) + "\"}");
   }

   AnsiString
   RestApiServer::OpenApiOperatorPaths_()
   {
      static const char *paths =
         ",\"/api/v1/accounts/{address}/export.zip\":{\"get\":{\"summary\":\"Everything the server holds on the account, as one archive\","
         "\"description\":\"application/zip, as a download: every folder with mail as an mbox under mail/, the address book as contacts.vcf, every calendar object under calendars/, the webmail's preferences, the active Sieve script, the log lines that name the address, an account.xml in the backup's form (settings, rules, app passwords, fetch accounts, sender list, S/MIME keys, files, scheduled mail, password history) less the credentials - no password hash or its scheme, authenticator seed, app password, recovery code, password-history or shared-file password hash, push subscription key or wrapped mailbox key, nor a fetch account's password or token - and a manifest.json that counts what is there: what a person exercising article 20 is handed. A folder past 256 MB is written as mbox parts. Built in the temp directory and held in memory once, so an archive past 500 MB, or mail past 2 GB before anything is built, is refused with 413: write it to disk with the POST and archive instead. One archive at a time for an account (409) and two on the server (429, Retry-After). A zero-access mailbox is refused with 403. The administrator password's alone.\","
         "\"responses\":{\"200\":{\"description\":\"The archive\"},\"403\":{\"description\":\"A zero-access mailbox\"},\"404\":{\"description\":\"Unknown account\"},\"409\":{\"description\":\"An archive of this account is being built\"},\"413\":{\"description\":\"Past 500 MB, or mail past 2 GB\"},\"429\":{\"description\":\"Two archives are being built on the server\"},\"500\":{\"description\":\"The archive could not be built\"}}}}"
         ",\"/api/v1/accounts/{address}/export\":{\"post\":{\"summary\":\"Write the mailbox to a directory on the server's disk as .eml files, or everything held on the account to one .zip archive there\","
         "\"description\":\"Every message of the account, one .eml file per message named by its UID, in directories mirroring the folder tree under the directory named - what a person exercising a data-access right is handed, and what Account.ExportMessages does over COM. Stops on the first failure with 500 naming the file, so a partial export never reports success. The administrator password's alone: the directory is anywhere on the server's disk the service can write.\","
         "\"requestBody\":{\"required\":true,\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"properties\":{\"directory\":{\"type\":\"string\"},\"archive\":{\"type\":\"string\",\"description\":\"The path of the .zip to write on the server's disk; what GET /export.zip downloads, with no size ceiling. One of directory and archive.\"}}}}}},"
         "\"responses\":{\"200\":{\"description\":\"exported (the count) and directory\"},\"400\":{\"description\":\"No directory\"},\"404\":{\"description\":\"Unknown account\"},\"500\":{\"description\":\"A file could not be copied; the export is incomplete\"}}}}"
         ",\"/api/v1/addresses/erase\":{\"post\":{\"summary\":\"Erase every trace of an address that deleting its account leaves\","
         "\"description\":\"Quarantine entries, message-trace rows, greylisting triplets, distribution-list memberships and aliases on either side, the rows naming the address in other accounts' address books and first-contact memory, the messages held for review it sent or is a recipient of (discarded), the messages that expire (the address taken out of the recipients of another sender's, which the others go on reading; its own, and one left with no recipient, ended), the password-protected messages to it and the replies it wrote (ended), the recipients other accounts remember writing to, the out-of-office memory, and the S/MIME certificates and OpenPGP keys other accounts keep of it as a correspondent; with include_archive the address's archive tree; and with include_logs the server's own log files rewritten so that every occurrence of the address reads [erased] - what Utilities.EraseAddressTraces and Utilities.EraseAddressLogLines do over COM. A legal hold outranks it: an archived copy a hold covers, and a message held for review, that expires or is password-protected of a held account, stays, and held counts them; when the holds themselves cannot be read the erasure fails with 500, naming them, rather than count what it left as held. Nothing found is success with removed 0; 500 means a step failed and the error log names the store to retry. The administrator password's alone: it reaches every domain's tables.\","
         "\"requestBody\":{\"required\":true,\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"properties\":{\"address\":{\"type\":\"string\"},\"include_archive\":{\"type\":\"boolean\"},\"include_logs\":{\"type\":\"boolean\"}},\"required\":[\"address\"]}}}},"
         "\"responses\":{\"200\":{\"description\":\"removed (the count), include_archive, include_logs, and held (what a legal hold kept in place)\"},\"400\":{\"description\":\"No address, or include_archive not a boolean\"},\"500\":{\"description\":\"A store could not be cleared\"}}}}"
         ",\"/api/v1/administrator/second-factor\":{"
         "\"get\":{\"summary\":\"Whether the administrator password has a second factor\",\"description\":\"enrolled: true when a one-time code is asked for beside the administrator password. The administrator password's alone.\",\"responses\":{\"200\":{\"description\":\"enrolled\"}}},"
         "\"post\":{\"summary\":\"Enrol a second factor on the administrator password\",\"description\":\"Generates the secret, stores it, and answers the otpauth URI an authenticator app scans - the only moment the secret leaves the server. Enforced from this moment. Refused with 409 while no administrator password is set, and while a factor is already enrolled: disable it first, because a second enrolment would replace the secret every authenticator app holds; and while hMailServer.INI, where the secret is kept, is read-only to the account the service runs as - the Windows service's own account (docs/ServiceAccount.md) - with the sentence saying what an administrator does instead. What Settings.EnrolAdministratorTOTP does over COM.\",\"responses\":{\"200\":{\"description\":\"enrolled: true and uri\"},\"409\":{\"description\":\"No administrator password, already enrolled, or hMailServer.INI read-only to the service\"},\"500\":{\"description\":\"The secret could not be stored\"}}},"
         "\"delete\":{\"summary\":\"Disable the administrator password's second factor\",\"description\":\"The next logon needs the password alone. Refused with 409 while hMailServer.INI is read-only to the account the service runs as, with the sentence saying what an administrator does instead. What Settings.DisableAdministratorTOTP does over COM.\",\"responses\":{\"200\":{\"description\":\"enrolled: false\"},\"409\":{\"description\":\"hMailServer.INI read-only to the service\"},\"500\":{\"description\":\"The secret could not be removed\"}}}}";

      return AnsiString(paths);
   }
}
