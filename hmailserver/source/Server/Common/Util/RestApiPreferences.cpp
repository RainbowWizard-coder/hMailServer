// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The signed-in account's preferences: GET and PUT /api/v1/me/preferences, a
// small key/value store the webmail keeps its choices in - the theme, the
// density, the undo-send delay, which folders to notify for - so that they
// follow the account between browsers instead of living in one browser's
// storage. The server attaches no meaning to a key; the page does.
//
// The store is hm_accountprefs (schema 6033): one row per account and key, the
// value a string of at most 4000 characters, at most 100 keys per account. PUT
// merges: each string member of the body is written, each null member is
// removed, and everything else in the body is refused - so a page that knows
// one key never disturbs another's.
//
// Two keys are the person's own record of an administrator's support access,
// and the server does read them: support_allowed, the consent without which no
// support session opens, and support_last, the note of who opened one and
// when, which the webmail shows until the person dismisses it. A support
// session may change neither - it would be the administrator erasing or
// rewriting the trace of their own visit, or granting themselves the consent
// - and an app password, a mail program's credential, may not give the
// consent (it may still withdraw it, the safe direction).

#include "StdAfx.h"
#include "RestApiServer.h"
#include "HttpServer.h"
#include "JsonDocument.h"
#include "Unicode.h"
#include "../BO/Account.h"
#include "../SQL/SQLCommand.h"
#include "../SQL/SQLStatement.h"
#include "../SQL/DALRecordset.h"
#include "../Application/Application.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      const int PreferenceKeyMaximum = 64;
      const int PreferenceValueMaximum = 4000;
      const int PreferencesPerAccount = 100;
      // The listener already refuses a body above 64 KiB; this is the same figure, stated here.
      const size_t PreferencesBodyMaximum = 64 * 1024;

      AnsiString Int64Text(__int64 value)
      {
         AnsiString text;
         text.Format("%I64d", value);
         return text;
      }

      // A key is a short name: letters, digits, dot, dash and underscore.
      bool ValidPreferenceKey(const std::string &key)
      {
         if (key.empty() || (int) key.size() > PreferenceKeyMaximum)
            return false;

         for (size_t i = 0; i < key.size(); i++)
         {
            char c = key[i];
            bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
            if (!ok)
               return false;
         }

         return true;
      }

      // Whether a key names one the server reads, in any case. The store
      // compares prefname under the database's collation, which on MySQL
      // (utf8mb4_general_ci), SQL Server and SQL Server Compact ignores case:
      // "Support_Last" there IS support_last - a null for it removes that row,
      // and "SUPPORT_ALLOWED" written as "1" is found by the support-session
      // route's own lookup of support_allowed. An exact comparison would let
      // either spelling past the refusals below.
      bool SameKey(const std::string &key, const char *name)
      {
         const size_t length = strlen(name);
         if (key.size() != length)
            return false;

         for (size_t i = 0; i < length; i++)
         {
            char c = key[i];
            if (c >= 'A' && c <= 'Z')
               c = (char) (c - 'A' + 'a');
            if (c != name[i])
               return false;
         }

         return true;
      }

      bool FindPreference(__int64 accountId, const String &name, __int64 &prefId)
      {
         prefId = 0;

         SQLCommand command("select prefid from hm_accountprefs where prefaccountid = @ACCOUNTID and prefname = @NAME");
         command.AddParameter("@ACCOUNTID", accountId);
         command.AddParameter("@NAME", name);

         std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
         if (!recordset || recordset->IsEOF())
            return false;

         prefId = recordset->GetInt64Value("prefid");
         return prefId > 0;
      }

      int CountPreferences(__int64 accountId)
      {
         SQLCommand command("select count(*) as prefcount from hm_accountprefs where prefaccountid = @ACCOUNTID");
         command.AddParameter("@ACCOUNTID", accountId);

         std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
         if (!recordset || recordset->IsEOF())
            return 0;

         return (int) recordset->GetLongValue("prefcount");
      }

      bool WritePreference(__int64 accountId, const String &name, const String &value)
      {
         __int64 prefId = 0;
         SQLStatement statement;
         statement.SetTable("hm_accountprefs");

         if (FindPreference(accountId, name, prefId))
         {
            statement.SetStatementType(SQLStatement::STUpdate);
            statement.AddColumn("prefvalue", value);
            statement.SetWhereClause("prefid = " + Int64Text(prefId));
            return Application::Instance()->GetDBManager()->Execute(statement);
         }

         statement.SetStatementType(SQLStatement::STInsert);
         statement.SetIdentityColumn("prefid");
         statement.AddColumnInt64("prefaccountid", accountId);
         statement.AddColumn("prefname", name);
         statement.AddColumn("prefvalue", value);
         return Application::Instance()->GetDBManager()->Execute(statement, &prefId) && prefId > 0;
      }

      bool RemovePreference(__int64 accountId, const String &name)
      {
         SQLCommand command("delete from hm_accountprefs where prefaccountid = @ACCOUNTID and prefname = @NAME");
         command.AddParameter("@ACCOUNTID", accountId);
         command.AddParameter("@NAME", name);
         return Application::Instance()->GetDBManager()->Execute(command);
      }
   }

   // A failed query and a missing row are two different answers: the first is
   // false, and the caller says so; the second is true with nothing in value.
   bool
   RestApiServer::ReadPreference_(__int64 accountId, const String &name, String &value)
   {
      value.Empty();

      SQLCommand command("select prefvalue from hm_accountprefs where prefaccountid = @ACCOUNTID and prefname = @NAME");
      command.AddParameter("@ACCOUNTID", accountId);
      command.AddParameter("@NAME", name);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset)
         return false;

      if (!recordset->IsEOF())
         value = recordset->GetStringValue("prefvalue");

      return true;
   }

   // The same cap PUT keeps: a route that writes one key of its own may not
   // take the hundred-and-first slot either. That refusal is reported apart
   // from a write that failed, so the route can answer it as PUT
   // /api/v1/me/preferences does, with a 400 and the same words.
   bool
   RestApiServer::WritePreference_(__int64 accountId, const String &name, const String &value, bool &full)
   {
      full = false;

      __int64 existing = 0;
      if (!FindPreference(accountId, name, existing) && CountPreferences(accountId) >= PreferencesPerAccount)
      {
         full = true;
         return false;
      }

      return WritePreference(accountId, name, value);
   }

   AnsiString
   RestApiServer::PreferencesJson_(__int64 accountId)
   {
      SQLCommand command("select prefname, prefvalue from hm_accountprefs where prefaccountid = @ACCOUNTID order by prefname");
      command.AddParameter("@ACCOUNTID", accountId);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset)
         return "";

      AnsiString json = "{\"preferences\":{";
      bool first = true;
      while (!recordset->IsEOF())
      {
         if (!first)
            json += ",";
         first = false;
         json += "\"" + JsonEscape_(Utf8_(recordset->GetStringValue("prefname"))) + "\":\"" +
                 JsonEscape_(Utf8_(recordset->GetStringValue("prefvalue"))) + "\"";
         recordset->MoveNext();
      }
      json += "}}";
      return json;
   }

   HttpResponse
   RestApiServer::HandleMePreferences_(const Caller &caller)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      AnsiString json = PreferencesJson_(account->GetID());
      if (json.IsEmpty())
         return BuildResponse_(500, "{\"error\":\"the preferences could not be read\"}");

      return BuildResponse_(200, json);
   }

   HttpResponse
   RestApiServer::HandleMePreferencesPut_(const Caller &caller, const AnsiString &requestBody)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      if (requestBody.size() > PreferencesBodyMaximum)
         return BuildResponse_(413, "{\"error\":\"the body is too large\"}");

      JsonValue document;
      std::string parseError;
      if (!JsonValue::Parse(std::string(requestBody.c_str(), requestBody.size()), document, parseError) || !document.IsObject())
         return BuildResponse_(400, "{\"error\":\"the body must be a JSON object of string values\"}");

      const std::vector<std::pair<std::string, JsonValue> > &members = document.Members();
      if (members.empty())
         return BuildResponse_(400, "{\"error\":\"nothing to change\"}");
      if ((int) members.size() > PreferencesPerAccount)
         return BuildResponse_(400, "{\"error\":\"at most 100 members in one request\"}");

      // Everything is checked before anything is written, so a bad member
      // leaves the store as it was.
      for (size_t i = 0; i < members.size(); i++)
      {
         if (!ValidPreferenceKey(members[i].first))
            return BuildResponse_(400, "{\"error\":\"a key is 1 to 64 letters, digits, dots, dashes or underscores: " + JsonEscape_(AnsiString(members[i].first.c_str())) + "\"}");

         const JsonValue &value = members[i].second;

         // In any case (SameKey): the store's comparison may ignore it.
         const bool consentKey = SameKey(members[i].first, "support_allowed");
         const bool supportKey = consentKey || SameKey(members[i].first, "support_last");
         if (supportKey && !caller.support_by.IsEmpty())
            return BuildResponse_(403, "{\"error\":\"not in a support session: support_allowed and support_last are the account holder's record of support access\",\"code\":\"support_session\"}");
         if (consentKey && !value.IsNull())
         {
            HttpResponse refusal;
            if (RefuseAppPasswordCaller_(caller, "allowing support access", refusal))
               return refusal;
         }

         if (value.IsNull())
            continue;
         if (!value.IsString())
            return BuildResponse_(400, "{\"error\":\"a value is a string, or null to remove it: " + JsonEscape_(AnsiString(members[i].first.c_str())) + "\"}");
         String text;
         Unicode::MultiByteToWide(AnsiString(value.AsString().c_str()), text);
         if (text.GetLength() > PreferenceValueMaximum)
            return BuildResponse_(400, "{\"error\":\"a value is at most 4000 characters: " + JsonEscape_(AnsiString(members[i].first.c_str())) + "\"}");
      }

      // The count this request would leave, found before anything is written,
      // so a request that would pass the cap is refused whole.
      int count = CountPreferences(account->GetID());
      std::vector<bool> presentBefore(members.size(), false);
      int resulting = count;
      for (size_t i = 0; i < members.size(); i++)
      {
         String key;
         Unicode::MultiByteToWide(AnsiString(members[i].first.c_str()), key);
         __int64 existing = 0;
         presentBefore[i] = FindPreference(account->GetID(), key, existing);
         if (members[i].second.IsNull())
         {
            if (presentBefore[i])
               resulting--;
         }
         else if (!presentBefore[i])
            resulting++;
      }
      if (resulting > PreferencesPerAccount)
         return BuildResponse_(400, "{\"error\":\"at most 100 preferences\"}");

      for (size_t i = 0; i < members.size(); i++)
      {
         String key;
         Unicode::MultiByteToWide(AnsiString(members[i].first.c_str()), key);
         const JsonValue &value = members[i].second;
         bool present = presentBefore[i];

         if (value.IsNull())
         {
            if (present)
            {
               if (!RemovePreference(account->GetID(), key))
                  return BuildResponse_(500, "{\"error\":\"the preference could not be removed\"}");
            }
            continue;
         }

         String text;
         Unicode::MultiByteToWide(AnsiString(value.AsString().c_str()), text);
         if (!WritePreference(account->GetID(), key, text))
            return BuildResponse_(500, "{\"error\":\"the preference could not be saved\"}");
      }

      AnsiString json = PreferencesJson_(account->GetID());
      if (json.IsEmpty())
         return BuildResponse_(500, "{\"error\":\"the preferences could not be read\"}");

      return BuildResponse_(200, json);
   }
}
