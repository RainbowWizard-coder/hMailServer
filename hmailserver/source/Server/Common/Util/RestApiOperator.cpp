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
#include "Totp.h"
#include "Unicode.h"
#include "../Application/IniFileSettings.h"
#include "../BO/Account.h"
#include "../Persistence/PersistentAccount.h"

#include <string>

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
   }

   HttpResponse
   RestApiServer::HandleAccountExport_(const Route &route, const AnsiString &requestBody)
   {
      std::shared_ptr<Account> account = std::shared_ptr<Account>(new Account());
      if (!PersistentAccount::ReadObject(account, String(route.identifier)) || account->GetID() == 0)
         return BuildResponse_(404, "{\"error\":\"account not found\"}");

      JsonValue body;
      if (!ParseObject(requestBody, body))
         return BuildResponse_(400, "{\"error\":\"The body must be a JSON object with directory.\"}");

      String directory = StringMember(body, "directory");
      directory.Trim();
      if (directory.IsEmpty())
         return BuildResponse_(400, "{\"error\":\"directory is required: a directory on the server's disk the export is written into.\"}");

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

      int removed = 0;
      if (!AddressTraceEraser::Erase(address, includeArchive, removed))
         return BuildResponse_(500, "{\"error\":\"Some traces could not be removed - the hMailServer error log names the stores to retry. What could be removed was; run the erasure again once the cause is dealt with.\"}");

      String logMessage;
      logMessage.Format(_T("Erased %d trace(s) of %s on an operator's request over the REST API%s."), removed, address.c_str(),
         includeArchive ? _T(", the archive included") : _T(""));
      LOG_APPLICATION(logMessage);

      AnsiString answer;
      answer.Format("{\"removed\":%d,\"include_archive\":%s}", removed, includeArchive ? "true" : "false");
      return BuildResponse_(200, answer);
   }

   HttpResponse
   RestApiServer::HandleAdministratorSecondFactor_(const Route &route)
   {
      auto ini = IniFileSettings::Instance();

      if (route.kind == RouteAdminSecondFactorGet)
         return BuildResponse_(200, AnsiString("{\"enrolled\":") + (ini->IsAdministratorTotpEnrolled() ? "true" : "false") + "}");

      if (route.kind == RouteAdminSecondFactorDisable)
      {
         if (!ini->SetAdministratorTotpSecret(String()))
            return BuildResponse_(500, "{\"error\":\"The secret could not be removed from hMailServer.INI. The account the server runs as needs write access to that file.\"}");

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
         return BuildResponse_(500, "{\"error\":\"The secret could not be written to hMailServer.INI. The account the server runs as needs write access to that file.\"}");

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
         ",\"/api/v1/accounts/{address}/export\":{\"post\":{\"summary\":\"Write the mailbox to a directory on the server's disk as .eml files\","
         "\"description\":\"Every message of the account, one .eml file per message named by its UID, in directories mirroring the folder tree under the directory named - what a person exercising a data-access right is handed, and what Account.ExportMessages does over COM. Stops on the first failure with 500 naming the file, so a partial export never reports success. The administrator password's alone: the directory is anywhere on the server's disk the service can write.\","
         "\"requestBody\":{\"required\":true,\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"properties\":{\"directory\":{\"type\":\"string\"}},\"required\":[\"directory\"]}}}},"
         "\"responses\":{\"200\":{\"description\":\"exported (the count) and directory\"},\"400\":{\"description\":\"No directory\"},\"404\":{\"description\":\"Unknown account\"},\"500\":{\"description\":\"A file could not be copied; the export is incomplete\"}}}}"
         ",\"/api/v1/addresses/erase\":{\"post\":{\"summary\":\"Erase every trace of an address that deleting its account leaves\","
         "\"description\":\"Quarantine entries, message-trace rows, greylisting triplets, distribution-list memberships and aliases on either side, and with include_archive the address's archive tree - what Utilities.EraseAddressTraces does over COM. Nothing found is success with removed 0; 500 means a step failed and the error log names the store to retry. The administrator password's alone: it reaches every domain's tables.\","
         "\"requestBody\":{\"required\":true,\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"properties\":{\"address\":{\"type\":\"string\"},\"include_archive\":{\"type\":\"boolean\"}},\"required\":[\"address\"]}}}},"
         "\"responses\":{\"200\":{\"description\":\"removed (the count) and include_archive\"},\"400\":{\"description\":\"No address, or include_archive not a boolean\"},\"500\":{\"description\":\"A store could not be cleared\"}}}}"
         ",\"/api/v1/administrator/second-factor\":{"
         "\"get\":{\"summary\":\"Whether the administrator password has a second factor\",\"description\":\"enrolled: true when a one-time code is asked for beside the administrator password. The administrator password's alone.\",\"responses\":{\"200\":{\"description\":\"enrolled\"}}},"
         "\"post\":{\"summary\":\"Enrol a second factor on the administrator password\",\"description\":\"Generates the secret, stores it, and answers the otpauth URI an authenticator app scans - the only moment the secret leaves the server. Enforced from this moment. Refused with 409 while no administrator password is set, and while a factor is already enrolled: disable it first, because a second enrolment would replace the secret every authenticator app holds. What Settings.EnrolAdministratorTOTP does over COM.\",\"responses\":{\"200\":{\"description\":\"enrolled: true and uri\"},\"409\":{\"description\":\"No administrator password, or already enrolled\"},\"500\":{\"description\":\"The secret could not be stored\"}}},"
         "\"delete\":{\"summary\":\"Disable the administrator password's second factor\",\"description\":\"The next logon needs the password alone. What Settings.DisableAdministratorTOTP does over COM.\",\"responses\":{\"200\":{\"description\":\"enrolled: false\"},\"500\":{\"description\":\"The secret could not be removed\"}}}}";

      return AnsiString(paths);
   }
}
