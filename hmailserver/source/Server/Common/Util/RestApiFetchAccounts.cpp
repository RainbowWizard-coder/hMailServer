// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The REST API's external (fetch) accounts: what InterfaceFetchAccounts and InterfaceFetchAccount do over COM. See RestApiServer.h.
//
// An external account is a remote POP3 or IMAP mailbox the server collects
// into one of its own accounts, on a schedule. Over COM it is
// Account.FetchAccounts: Add, the setters, Save, Delete and DownloadNow. Here
// it is a sub-resource of the account that owns it, so that a key restricted
// to named domains is scoped by the address in the path exactly as the
// account routes are, and a fetch account is never reachable by its id alone:
//
//    GET    /api/v1/accounts/{address}/fetch-accounts               the account's fetch accounts
//    POST   /api/v1/accounts/{address}/fetch-accounts               create one
//    GET    /api/v1/accounts/{address}/fetch-accounts/{id}          one of them
//    PUT    /api/v1/accounts/{address}/fetch-accounts/{id}          change any subset of its fields
//    DELETE /api/v1/accounts/{address}/fetch-accounts/{id}          delete it, with the UIDs it remembers
//    POST   /api/v1/accounts/{address}/fetch-accounts/{id}/download collect now
//
// And the reader's own, under /api/v1/me/fetch-accounts with the same six
// verbs: the same handlers, given the signed-in account's address, and a body
// narrowed first. A reader sets where to collect from and how often; what the
// collected mail is then subjected to is the server's - the spam and virus
// filters on, the recipients not re-read from the message's headers (which
// would let a message someone else wrote deliver itself to other people here),
// no routing by recipient. The host has to be public unless an administrator
// allows private ones, the interval is at least 15 minutes, and an account has
// at most ReaderFetchAccountsMax of them.
//
// Each handler does what the COM member does with the same persistence call:
// a create is InterfaceFetchAccounts::Add + the setters + InterfaceFetchAccount::Save,
// which is PersistentFetchAccount::SaveObject; a delete goes through the
// account's FetchAccounts collection as InterfaceFetchAccount::Delete does, so
// the row and its UID list go together; DownloadNow is
// PersistentFetchAccount::SetRetryNow and ExternalFetchManager::SetCheckNow,
// and answers 202 because the collection happens on the fetcher's thread - a
// caller watches locked go false, as the fixtures do. The remote password is
// write-only, as it is over COM.

#include "StdAfx.h"
#include "RestApiServer.h"
#include "JsonDocument.h"
#include "Unicode.h"
#include "../BO/Account.h"
#include "../BO/FetchAccount.h"
#include "../BO/FetchAccounts.h"
#include "../Persistence/PersistentAccount.h"
#include "../Persistence/PersistentFetchAccount.h"
#include "../../ExternalFetcher/ExternalFetchManager.h"
#include "../Application/Configuration.h"
#include "../Application/EffectiveSetting.h"
#include "HttpsClient.h"
#include "ReaderOAuth.h"
#include "Parsing/StringParser.h"

#include <cmath>
#include <functional>
#include <string>
#include <vector>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      typedef std::function<AnsiString(const String &)> Quote;

      const char *ServerTypeWord(FetchAccount::ServerType type)
      {
         return type == FetchAccount::IMAP ? "imap" : "pop3";
      }

      const char *SecurityWord(ConnectionSecurity security)
      {
         switch (security)
         {
         case CSSSL:
            return "tls";
         case CSSTARTTLSOptional:
            return "starttls_optional";
         case CSSTARTTLSRequired:
            return "starttls_required";
         case CSNone:
         default:
            return "none";
         }
      }

      bool ParseSecurityWord(const std::string &word, ConnectionSecurity &security)
      {
         AnsiString value = word.c_str();

         if (value.CompareNoCase("none") == 0)
            security = CSNone;
         else if (value.CompareNoCase("tls") == 0)
            security = CSSSL;
         else if (value.CompareNoCase("starttls_optional") == 0)
            security = CSSTARTTLSOptional;
         else if (value.CompareNoCase("starttls_required") == 0)
            security = CSSTARTTLSRequired;
         else
            return false;

         return true;
      }

      AnsiString ErrorBody(const Quote &quote, const String &sentence)
      {
         AnsiString body;
         body.Format("{\"error\":\"%hs\"}", quote(sentence).c_str());
         return body;
      }

      String Utf8ToString(const std::string &utf8)
      {
         String value;
         Unicode::MultiByteToWide(AnsiString(utf8.c_str()), value);
         return value;
      }

      // One folder name per line, as the mirror keeps them, as a JSON array:
      // every name, or only those marked with a tab and a star as a view of the
      // other folders (IMAPClientConnection::HandleList_).
      AnsiString NamesJson(const String &lines, const Quote &quote, bool copiesOnly = false)
      {
         AnsiString json = "[";
         bool first = true;
         for (String name : StringParser::SplitString(lines, _T("\n")))
         {
            const bool copy = name.EndsWith(_T("\t*"));
            if (copy)
               name = name.Mid(0, name.GetLength() - 2);
            if (name.IsEmpty() || (copiesOnly && !copy))
               continue;
            json += (first ? "\"" : ",\"") + quote(name) + "\"";
            first = false;
         }
         return json + "]";
      }

      // A fetch account as every route emits it. The password and the refresh
      // token are the columns left out.
      AnsiString FetchAccountJson(std::shared_ptr<FetchAccount> fetchAccount, const Quote &quote)
      {
         AnsiString entry;
         entry.Format("{\"id\":%I64d,\"name\":\"%hs\",\"server_address\":\"%hs\",\"port\":%d,\"server_type\":\"%hs\"",
            fetchAccount->GetID(),
            quote(fetchAccount->GetName()).c_str(),
            quote(fetchAccount->GetServerAddress()).c_str(),
            fetchAccount->GetPort(),
            ServerTypeWord(fetchAccount->GetServerType()));

         AnsiString numbers;
         numbers.Format(",\"username\":\"%hs\",\"minutes_between_fetch\":%d,\"days_to_keep_messages\":%d,\"connection_security\":\"%hs\",\"mime_recipient_headers\":\"%hs\"",
            quote(fetchAccount->GetUsername()).c_str(),
            fetchAccount->GetMinutesBetweenTry(),
            fetchAccount->GetDaysToKeep(),
            SecurityWord(fetchAccount->GetConnectionSecurity()),
            quote(fetchAccount->GetMIMERecipientHeaders()).c_str());
         entry += numbers;

         auto flag = [&entry](const char *name, bool value)
         {
            entry += ",\"";
            entry += name;
            entry += value ? "\":true" : "\":false";
         };

         flag("enabled", fetchAccount->GetActive());
         flag("process_mime_recipients", fetchAccount->GetProcessMIMERecipients());
         flag("process_mime_date", fetchAccount->GetProcessMIMEDate());
         flag("use_antispam", fetchAccount->GetUseAntiSpam());
         flag("use_antivirus", fetchAccount->GetUseAntiVirus());
         flag("enable_route_recipients", fetchAccount->GetEnableRouteRecipients());
         flag("mirror_folders", fetchAccount->GetMirrorFolders());
         flag("locked", PersistentFetchAccount::IsLocked(fetchAccount->GetID()));

         // Signed in with a provider; the folders the other server listed, and
         // the reader's choice of them - pending until they have made one.
         const bool pending = fetchAccount->GetMirrorSkip() == _T("*");
         flag("mirror_pending", pending);
         entry += ",\"oauth_provider\":\"" + quote(fetchAccount->GetOAuthProvider()) + "\"";
         entry += ",\"mirror_seen\":" + NamesJson(fetchAccount->GetMirrorSeen(), quote);
         entry += ",\"mirror_copies\":" + NamesJson(fetchAccount->GetMirrorSeen(), quote, true);
         entry += ",\"mirror_skip\":" + NamesJson(pending ? String() : fetchAccount->GetMirrorSkip(), quote);

         AnsiString collected;
         collected.Format(",\"collected_messages\":%d", PersistentFetchAccount::CountUIDs(fetchAccount->GetID()));
         entry += collected;

         entry += ",\"next_download_time\":\"" + quote(fetchAccount->GetNextTry()) + "\"}";
         return entry;
      }

      // The fields a body may carry, for both the create and the update.
      const char *const KnownKeys[] =
      {
         "name", "server_address", "port", "server_type", "username", "password", "enabled",
         "minutes_between_fetch", "days_to_keep_messages", "connection_security",
         "process_mime_recipients", "process_mime_date", "use_antispam", "use_antivirus",
         "enable_route_recipients", "mime_recipient_headers", "mirror_folders", "mirror_skip"
      };

      bool ReadString(const JsonValue &object, const char *key, String &out, bool &given, AnsiString &error)
      {
         const JsonValue *member = object.Get(key);
         if (!member || member->IsNull())
            return true;

         if (!member->IsString())
         {
            error.Format("%hs must be a string", key);
            return false;
         }

         out = Utf8ToString(member->AsString());
         given = true;
         return true;
      }

      bool ReadBool(const JsonValue &object, const char *key, bool &out, AnsiString &error)
      {
         const JsonValue *member = object.Get(key);
         if (!member || member->IsNull())
            return true;

         if (!member->IsBool())
         {
            error.Format("%hs must be true or false", key);
            return false;
         }

         out = member->AsBool();
         return true;
      }

      bool ReadInteger(const JsonValue &object, const char *key, long minimum, long maximum, long &out, AnsiString &error)
      {
         const JsonValue *member = object.Get(key);
         if (!member || member->IsNull())
            return true;

         double number = member->IsNumber() ? member->AsNumber() : 0.0;

         if (!member->IsNumber() || std::floor(number) != number ||
             number < (double) minimum || number > (double) maximum)
         {
            error.Format("%hs must be a whole number between %ld and %ld", key, minimum, maximum);
            return false;
         }

         out = (long) number;
         return true;
      }

      // Applies a body to a fetch account, new or existing. Every member is read
      // and checked before any is applied, so a refused body changes nothing;
      // the sentence names the member. A string longer than its column is
      // refused rather than cut, because a name that came back shorter than it
      // was written would be a different fetch account.
      bool ApplyBody(const JsonValue &body, std::shared_ptr<FetchAccount> fetchAccount, AnsiString &error)
      {
         for (const std::pair<std::string, JsonValue> &member : body.Members())
         {
            bool known = false;
            for (size_t i = 0; i < sizeof(KnownKeys) / sizeof(KnownKeys[0]); i++)
            {
               if (member.first == KnownKeys[i])
               {
                  known = true;
                  break;
               }
            }
            if (!known)
            {
               error = AnsiString("unknown field: ") + AnsiString(member.first.substr(0, 48).c_str());
               return false;
            }
         }

         String name = fetchAccount->GetName(), serverAddress = fetchAccount->GetServerAddress();
         String username = fetchAccount->GetUsername(), password = fetchAccount->GetPassword();
         String headers = fetchAccount->GetMIMERecipientHeaders(), serverType, security;
         bool nameGiven = false, serverGiven = false, usernameGiven = false, passwordGiven = false;
         bool headersGiven = false, typeGiven = false, securityGiven = false;

         if (!ReadString(body, "name", name, nameGiven, error) ||
             !ReadString(body, "server_address", serverAddress, serverGiven, error) ||
             !ReadString(body, "username", username, usernameGiven, error) ||
             !ReadString(body, "password", password, passwordGiven, error) ||
             !ReadString(body, "mime_recipient_headers", headers, headersGiven, error) ||
             !ReadString(body, "server_type", serverType, typeGiven, error) ||
             !ReadString(body, "connection_security", security, securityGiven, error))
            return false;

         long port = fetchAccount->GetPort();
         long minutes = fetchAccount->GetMinutesBetweenTry();
         long days = fetchAccount->GetDaysToKeep();

         if (!ReadInteger(body, "port", 1, 65535, port, error) ||
             !ReadInteger(body, "minutes_between_fetch", 1, 100000, minutes, error) ||
             !ReadInteger(body, "days_to_keep_messages", -1, 100000, days, error))
            return false;

         bool enabled = fetchAccount->GetActive();
         bool processRecipients = fetchAccount->GetProcessMIMERecipients();
         bool processDate = fetchAccount->GetProcessMIMEDate();
         bool antiSpam = fetchAccount->GetUseAntiSpam();
         bool antiVirus = fetchAccount->GetUseAntiVirus();
         bool routeRecipients = fetchAccount->GetEnableRouteRecipients();
         bool mirror = fetchAccount->GetMirrorFolders();

         if (!ReadBool(body, "enabled", enabled, error) ||
             !ReadBool(body, "process_mime_recipients", processRecipients, error) ||
             !ReadBool(body, "process_mime_date", processDate, error) ||
             !ReadBool(body, "use_antispam", antiSpam, error) ||
             !ReadBool(body, "use_antivirus", antiVirus, error) ||
             !ReadBool(body, "enable_route_recipients", routeRecipients, error) ||
             !ReadBool(body, "mirror_folders", mirror, error))
            return false;

         // The folders not to bring over, by the names the other server listed.
         // Given, it is the reader's choice made, so the "*" that waits for one
         // goes with it.
         String mirrorSkip = fetchAccount->GetMirrorSkip();
         if (const JsonValue *member = body.Get("mirror_skip"))
         {
            if (!member->IsNull())
            {
               if (!member->IsArray())
               {
                  error = "mirror_skip must be an array of folder names";
                  return false;
               }
               String joined;
               for (const JsonValue &item : member->Items())
               {
                  if (!item.IsString() || item.AsString().empty() || item.AsString().size() > 1000 ||
                      item.AsString().find_first_of("\r\n") != std::string::npos)
                  {
                     error = "mirror_skip holds folder names, each a string of one line";
                     return false;
                  }
                  joined += Utf8ToString(item.AsString()) + _T("\n");
               }
               if (joined.GetLength() > 100000)
               {
                  error = "mirror_skip is at most 100000 characters";
                  return false;
               }
               mirrorSkip = joined;
            }
         }

         FetchAccount::ServerType type = fetchAccount->GetServerType();
         if (typeGiven)
         {
            AnsiString word = serverType;
            if (word.CompareNoCase("pop3") == 0)
               type = FetchAccount::POP3;
            else if (word.CompareNoCase("imap") == 0)
               type = FetchAccount::IMAP;
            else
            {
               error = "server_type must be pop3 or imap";
               return false;
            }
         }

         ConnectionSecurity connectionSecurity = fetchAccount->GetConnectionSecurity();
         if (securityGiven && !ParseSecurityWord(std::string(AnsiString(security).c_str()), connectionSecurity))
         {
            error = "connection_security must be one of none, starttls_optional, starttls_required, tls";
            return false;
         }

         name.Trim();
         serverAddress.Trim();

         if (name.IsEmpty())
         {
            error = "name is required";
            return false;
         }
         if (name.GetLength() > 255 || serverAddress.GetLength() > 255 || username.GetLength() > 255 || headers.GetLength() > 255)
         {
            error = "name, server_address, username and mime_recipient_headers are at most 255 characters";
            return false;
         }
         if (serverAddress.IsEmpty())
         {
            error = "server_address is required";
            return false;
         }
         if (port == 0)
         {
            error = "port is required";
            return false;
         }

         fetchAccount->SetName(name);
         fetchAccount->SetServerAddress(serverAddress);
         fetchAccount->SetPort((int) port);
         fetchAccount->SetServerType(type);
         fetchAccount->SetUsername(username);
         fetchAccount->SetPassword(password);
         fetchAccount->SetActive(enabled);
         fetchAccount->SetMinutesBetweenTry((int) minutes);
         fetchAccount->SetDaysToKeep((int) days);
         fetchAccount->SetConnectionSecurity(connectionSecurity);
         fetchAccount->SetProcessMIMERecipients(processRecipients);
         fetchAccount->SetProcessMIMEDate(processDate);
         fetchAccount->SetUseAntiSpam(antiSpam);
         fetchAccount->SetUseAntiVirus(antiVirus);
         fetchAccount->SetEnableRouteRecipients(routeRecipients);
         fetchAccount->SetMIMERecipientHeaders(headers);
         fetchAccount->SetMirrorFolders(mirror);
         fetchAccount->SetMirrorSkip(mirrorSkip);
         return true;
      }

      // The account named in the path, or null. Read as HandleUpdateAccount_
      // reads it: by address, through PersistentAccount.
      std::shared_ptr<Account> OwningAccount(const String &address)
      {
         std::shared_ptr<Account> account = std::shared_ptr<Account>(new Account());
         if (!PersistentAccount::ReadObject(account, address) || account->GetID() == 0)
            return std::shared_ptr<Account>();
         return account;
      }

      bool ParseBody(const AnsiString &requestBody, JsonValue &body)
      {
         std::string parseError;
         std::string text(requestBody.c_str(), (size_t) requestBody.GetLength());
         return JsonValue::Parse(text, body, parseError) && body.IsObject();
      }
   }

   AnsiString
   RestApiServer::OpenApiFetchAccountsPaths_()
   {
      static const char *paths =
         ",\"/api/v1/accounts/{address}/fetch-accounts\":{"
         "\"get\":{\"summary\":\"The account's external (fetch) accounts\",\"description\":\"The remote POP3 or IMAP mailboxes the server collects into this account - Account.FetchAccounts over COM. Each entry: id, name, server_address, port, server_type (pop3 or imap), username, enabled, minutes_between_fetch, days_to_keep_messages, connection_security, process_mime_recipients, process_mime_date, use_antispam, use_antivirus, enable_route_recipients, mime_recipient_headers, mirror_folders, locked (a collection is running now) and next_download_time. The remote password is never emitted. A key restricted to named domains reaches the accounts of those domains only.\",\"responses\":{\"200\":{\"description\":\"Array of fetch accounts\"},\"404\":{\"description\":\"No such account\"}}},"
         "\"post\":{\"summary\":\"Create an external (fetch) account\",\"description\":\"Body: name, server_address and port (required); server_type (pop3, the default, or imap), username, password (write-only), enabled (default true), minutes_between_fetch (default 30), days_to_keep_messages (default 0: delete after collecting; -1 keeps every message; for IMAP the remote INBOX is collected once by UID and left intact when this says so), connection_security (none, starttls_optional, starttls_required, tls; default none), process_mime_recipients, process_mime_date, use_antispam, use_antivirus, enable_route_recipients, mime_recipient_headers, mirror_folders. What InterfaceFetchAccounts.Add and Save do; the first collection is scheduled at once. Everything is checked before anything is saved: an unknown field, a value of the wrong type or a value out of range is a 400 naming it.\",\"requestBody\":{\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"required\":[\"name\",\"server_address\",\"port\"],\"properties\":{\"name\":{\"type\":\"string\"},\"server_address\":{\"type\":\"string\"},\"port\":{\"type\":\"integer\"},\"server_type\":{\"type\":\"string\",\"enum\":[\"pop3\",\"imap\"]},\"username\":{\"type\":\"string\"},\"password\":{\"type\":\"string\",\"writeOnly\":true},\"enabled\":{\"type\":\"boolean\"},\"minutes_between_fetch\":{\"type\":\"integer\"},\"days_to_keep_messages\":{\"type\":\"integer\"},\"connection_security\":{\"type\":\"string\",\"enum\":[\"none\",\"starttls_optional\",\"starttls_required\",\"tls\"]},\"process_mime_recipients\":{\"type\":\"boolean\"},\"process_mime_date\":{\"type\":\"boolean\"},\"use_antispam\":{\"type\":\"boolean\"},\"use_antivirus\":{\"type\":\"boolean\"},\"enable_route_recipients\":{\"type\":\"boolean\"},\"mime_recipient_headers\":{\"type\":\"string\"},\"mirror_folders\":{\"type\":\"boolean\"}}}}}},\"responses\":{\"201\":{\"description\":\"Created: the fetch account as the listing shows it, with its id\"},\"400\":{\"description\":\"A required field missing, an unknown field, or a value refused (error names it)\"},\"404\":{\"description\":\"No such account\"}}}},"
         "\"/api/v1/accounts/{address}/fetch-accounts/{id}\":{"
         "\"get\":{\"summary\":\"One external (fetch) account\",\"description\":\"As the listing shows it. A fetch account that belongs to another account is not found here.\",\"responses\":{\"200\":{\"description\":\"The fetch account\"},\"404\":{\"description\":\"No such account, or no fetch account with that id in it\"}}},"
         "\"put\":{\"summary\":\"Change an external (fetch) account\",\"description\":\"Body: any subset of the fields POST takes; a field left out keeps its value, and a password left out is kept. The same checks as POST, and nothing changes when one fails. What the setters and Save do over COM.\",\"requestBody\":{\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\"}}}},\"responses\":{\"200\":{\"description\":\"The fetch account as saved\"},\"400\":{\"description\":\"A field refused; nothing changed\"},\"404\":{\"description\":\"No such account, or no fetch account with that id in it\"}}},"
         "\"delete\":{\"summary\":\"Delete an external (fetch) account\",\"description\":\"Through the account's collection as InterfaceFetchAccount.Delete goes: the row and the UIDs it remembered go together. Mail already collected stays.\",\"responses\":{\"200\":{\"description\":\"Deleted\"},\"404\":{\"description\":\"No such account, or no fetch account with that id in it\"}}}},"
         "\"/api/v1/accounts/{address}/fetch-accounts/{id}/download\":{\"post\":{\"summary\":\"Collect from the remote mailbox now\",\"description\":\"What DownloadNow does over COM: the next collection is set to now and the fetcher woken. Answers before the collection runs, because it runs on the fetcher's thread: watch locked in GET until it is false again.\",\"responses\":{\"202\":{\"description\":\"Queued\"},\"404\":{\"description\":\"No such account, or no fetch account with that id in it\"},\"503\":{\"description\":\"The external fetcher is not running\"}}}}";

      return AnsiString(paths);
   }

   HttpResponse
   RestApiServer::HandleListFetchAccounts_(const String &address)
   {
      Quote quote = [](const String &value) { return JsonEscape_(Utf8_(value)); };

      std::shared_ptr<Account> account = OwningAccount(address);
      if (!account)
         return BuildResponse_(404, "{\"error\":\"account not found\"}");

      FetchAccounts fetchAccounts(account->GetID());
      fetchAccounts.Refresh();

      AnsiString body = "[";
      int count = 0;
      for (std::shared_ptr<FetchAccount> fetchAccount : fetchAccounts.GetSnapshot())
      {
         if (!fetchAccount)
            continue;
         if (count > 0)
            body += ",";
         body += FetchAccountJson(fetchAccount, quote);
         count++;
      }
      body += "]";

      return BuildResponse_(200, body);
   }

   HttpResponse
   RestApiServer::HandleCreateFetchAccount_(const String &address, const AnsiString &requestBody)
   {
      Quote quote = [](const String &value) { return JsonEscape_(Utf8_(value)); };

      std::shared_ptr<Account> account = OwningAccount(address);
      if (!account)
         return BuildResponse_(404, "{\"error\":\"account not found\"}");

      JsonValue body;
      if (!ParseBody(requestBody, body))
         return BuildResponse_(400, "{\"error\":\"the body must be a JSON object\"}");

      // As InterfaceFetchAccounts::Add makes one: the constructor's defaults,
      // owned by this account.
      std::shared_ptr<FetchAccount> fetchAccount = std::shared_ptr<FetchAccount>(new FetchAccount());
      fetchAccount->SetAccountID(account->GetID());

      AnsiString error;
      if (!ApplyBody(body, fetchAccount, error))
         return BuildResponse_(400, ErrorBody(quote, String(error)));

      if (!PersistentFetchAccount::SaveObject(fetchAccount))
         return BuildResponse_(500, "{\"error\":\"failed to save the fetch account; see the error log\"}");

      LOG_APPLICATION("RestApi: Fetch account " + fetchAccount->GetName() + " created for " + address + ".");

      return BuildResponse_(201, FetchAccountJson(fetchAccount, quote));
   }

   HttpResponse
   RestApiServer::HandleGetFetchAccount_(const String &address, __int64 fetchAccountId)
   {
      Quote quote = [](const String &value) { return JsonEscape_(Utf8_(value)); };

      std::shared_ptr<Account> account = OwningAccount(address);
      if (!account)
         return BuildResponse_(404, "{\"error\":\"account not found\"}");

      FetchAccounts fetchAccounts(account->GetID());
      fetchAccounts.Refresh();

      std::shared_ptr<FetchAccount> fetchAccount = fetchAccounts.GetItemByDBID(fetchAccountId);
      if (!fetchAccount)
         return BuildResponse_(404, "{\"error\":\"fetch account not found\"}");

      return BuildResponse_(200, FetchAccountJson(fetchAccount, quote));
   }

   HttpResponse
   RestApiServer::HandleUpdateFetchAccount_(const String &address, __int64 fetchAccountId, const AnsiString &requestBody)
   {
      Quote quote = [](const String &value) { return JsonEscape_(Utf8_(value)); };

      std::shared_ptr<Account> account = OwningAccount(address);
      if (!account)
         return BuildResponse_(404, "{\"error\":\"account not found\"}");

      JsonValue body;
      if (!ParseBody(requestBody, body))
         return BuildResponse_(400, "{\"error\":\"the body must be a JSON object\"}");

      FetchAccounts fetchAccounts(account->GetID());
      fetchAccounts.Refresh();

      std::shared_ptr<FetchAccount> fetchAccount = fetchAccounts.GetItemByDBID(fetchAccountId);
      if (!fetchAccount)
         return BuildResponse_(404, "{\"error\":\"fetch account not found\"}");

      AnsiString error;
      if (!ApplyBody(body, fetchAccount, error))
         return BuildResponse_(400, ErrorBody(quote, String(error)));

      if (!PersistentFetchAccount::SaveObject(fetchAccount))
         return BuildResponse_(500, "{\"error\":\"failed to save the fetch account; see the error log\"}");

      LOG_APPLICATION("RestApi: Fetch account " + fetchAccount->GetName() + " of " + address + " updated.");

      return BuildResponse_(200, FetchAccountJson(fetchAccount, quote));
   }

   HttpResponse
   RestApiServer::HandleDeleteFetchAccount_(const String &address, __int64 fetchAccountId)
   {
      std::shared_ptr<Account> account = OwningAccount(address);
      if (!account)
         return BuildResponse_(404, "{\"error\":\"account not found\"}");

      FetchAccounts fetchAccounts(account->GetID());
      fetchAccounts.Refresh();

      std::shared_ptr<FetchAccount> fetchAccount = fetchAccounts.GetItemByDBID(fetchAccountId);
      if (!fetchAccount)
         return BuildResponse_(404, "{\"error\":\"fetch account not found\"}");

      String name = fetchAccount->GetName();

      if (!fetchAccounts.DeleteItemByDBID(fetchAccountId))
         return BuildResponse_(500, "{\"error\":\"failed to delete the fetch account\"}");

      LOG_APPLICATION("RestApi: Fetch account " + name + " of " + address + " deleted.");

      return BuildResponse_(200, "{\"deleted\":true}");
   }

   HttpResponse
   RestApiServer::HandleDownloadFetchAccount_(const String &address, __int64 fetchAccountId)
   {
      std::shared_ptr<Account> account = OwningAccount(address);
      if (!account)
         return BuildResponse_(404, "{\"error\":\"account not found\"}");

      FetchAccounts fetchAccounts(account->GetID());
      fetchAccounts.Refresh();

      std::shared_ptr<FetchAccount> fetchAccount = fetchAccounts.GetItemByDBID(fetchAccountId);
      if (!fetchAccount)
         return BuildResponse_(404, "{\"error\":\"fetch account not found\"}");

      std::shared_ptr<ExternalFetchManager> fetcher = Application::Instance()->GetExternalFetchManager();
      if (!fetcher)
         return BuildResponse_(503, "{\"error\":\"the external fetcher is not running\"}");

      PersistentFetchAccount::SetRetryNow(fetchAccount->GetID());
      fetcher->SetCheckNow();

      LOG_APPLICATION("RestApi: Fetch account " + fetchAccount->GetName() + " of " + address + " asked to collect now.");

      return BuildResponse_(202, "{\"queued\":true}");
   }

   namespace
   {
      // What a reader may set, of the fields the administrator's route takes.
      const char *const ReaderKeys[] =
      {
         "name", "server_address", "port", "server_type", "username", "password", "enabled",
         "minutes_between_fetch", "days_to_keep_messages", "connection_security", "mirror_folders", "mirror_skip"
      };

      // What an account signed in with a provider keeps: its grant is for that
      // server and that address, and a change of either would carry the reader's
      // token somewhere else.
      const char *const SignedInKeys[] =
      {
         "server_address", "port", "server_type", "username", "password", "connection_security"
      };

      // The rest of the body, fixed: the filters on, nothing read from the
      // message to decide who it is for.
      const char *const ReaderForced = "\"use_antispam\":true,\"use_antivirus\":true,\"process_mime_recipients\":false,\"enable_route_recipients\":false,\"mime_recipient_headers\":\"\"";

      // A fetch account an administrator set up with what a reader cannot set -
      // recipients read from the message, routing by them, a filter off. The
      // reader sees it and can collect from it now; changing or removing it is
      // the administrator's, since a reader's save would put those back.
      bool AdministratorsOwn(__int64 accountId, __int64 fetchAccountId)
      {
         FetchAccounts fetchAccounts(accountId);
         fetchAccounts.Refresh();
         std::shared_ptr<FetchAccount> fetchAccount = fetchAccounts.GetItemByDBID(fetchAccountId);
         if (!fetchAccount)
            return false;
         return fetchAccount->GetProcessMIMERecipients() || fetchAccount->GetEnableRouteRecipients() ||
                !fetchAccount->GetUseAntiSpam() || !fetchAccount->GetUseAntiVirus() ||
                !fetchAccount->GetMIMERecipientHeaders().IsEmpty();
      }

      // The body a reader sent, checked for its keys and its interval and host,
      // with the fixed fields added. Empty, with the refusal in problem, when it
      // is refused.
      AnsiString ReaderBody(const AnsiString &requestBody, bool creating, AnsiString &problem)
      {
         // The same narrowing for a new one and a change: a change names only
         // what it changes, and whatever it names is judged as a new one would be.
         (void) creating;

         JsonValue body;
         if (!ParseBody(requestBody, body))
         {
            problem = "the body must be a JSON object";
            return "";
         }

         for (const std::pair<std::string, JsonValue> &member : body.Members())
         {
            bool allowed = false;
            for (size_t i = 0; i < sizeof(ReaderKeys) / sizeof(ReaderKeys[0]); i++)
               allowed = allowed || member.first == ReaderKeys[i];
            if (!allowed)
            {
               problem = AnsiString("not a field a reader sets: ") + AnsiString(member.first.substr(0, 48).c_str());
               return "";
            }
         }

         const JsonValue *minutes = body.Get("minutes_between_fetch");
         if (minutes && minutes->IsNumber() && minutes->AsNumber() < 15)
         {
            problem = "minutes_between_fetch is at least 15 for an account a reader adds";
            return "";
         }
         const JsonValue *server = body.Get("server_address");
         if (server && server->IsString() && !Configuration::Instance()->GetReaderFetchAccountsPrivateAddresses())
         {
            AnsiString host = AnsiString(server->AsString().c_str());
            host.Trim();
            String refusal;
            if (!host.IsEmpty() && !HttpsClient::HostIsPublic(host, refusal))
            {
               problem = "server_address: " + AnsiString(refusal);
               return "";
            }
         }

         std::string text(requestBody.c_str(), (size_t) requestBody.GetLength());
         size_t close = text.find_last_of('}');
         size_t open = text.find('{');
         if (close == std::string::npos || open == std::string::npos || close < open)
         {
            problem = "the body must be a JSON object";
            return "";
         }
         std::string inside = text.substr(open + 1, close - open - 1);
         bool empty = inside.find_first_not_of(" \t\r\n") == std::string::npos;
         return AnsiString(("{" + inside + (empty ? "" : ",") + ReaderForced + "}").c_str());
      }
   }

   AnsiString
   RestApiServer::OpenApiReaderFetchAccountsPaths_()
   {
      static const char *paths =
         ",\"/api/v1/me/fetch-accounts\":{"
         "\"get\":{\"summary\":\"The signed-in account's other accounts\",\"description\":\"The remote POP3 or IMAP mailboxes collected into this account, whoever added them - each as GET /api/v1/accounts/{address}/fetch-accounts shows it, the password never. 403 when ReaderFetchAccountsMax is 0.\",\"responses\":{\"200\":{\"description\":\"Array of fetch accounts\"},\"403\":{\"description\":\"Not offered on this server\"}}},"
         "\"post\":{\"summary\":\"Add another account to collect from\",\"description\":\"Body: name, server_address and port (required); server_type (pop3 or imap), username, password (write-only), enabled, minutes_between_fetch (at least 15, default 30), days_to_keep_messages (0 deletes from the remote after collecting, -1 keeps every message there), connection_security (none, starttls_optional, starttls_required, tls) and mirror_folders (IMAP: bring the remote folders over as folders here). Any other field is refused: the spam and virus filters are on and the recipients are not re-read from the message, whatever is sent. The host has to resolve to public addresses only unless reader_fetch_accounts_private_addresses is on - checked when it is saved - and an account has at most reader_fetch_accounts_max. The first collection is scheduled at once. A mutating route: a browser session carries X-Requested-With.\",\"responses\":{\"201\":{\"description\":\"The fetch account\"},\"400\":{\"description\":\"A field refused (error names it), a host that is not public, or an interval under 15 minutes\"},\"403\":{\"description\":\"Not offered on this server, or the account has as many as it may\"}}}},"
         "\"/api/v1/me/fetch-accounts/{id}\":{"
         "\"get\":{\"summary\":\"One of the signed-in account's other accounts\",\"responses\":{\"200\":{\"description\":\"The fetch account\"},\"404\":{\"description\":\"Not one of this account's\"}}},"
         "\"put\":{\"summary\":\"Change one of the signed-in account's other accounts\",\"description\":\"Any subset of the fields POST takes, with the same refusals; a password left out is kept.\",\"responses\":{\"200\":{\"description\":\"The fetch account as saved\"},\"400\":{\"description\":\"A field refused; nothing changed\"},\"404\":{\"description\":\"Not one of this account's\"}}},"
         "\"delete\":{\"summary\":\"Stop collecting from one of the signed-in account's other accounts\",\"description\":\"The fetch account and the UIDs it remembered go; mail already collected stays.\",\"responses\":{\"200\":{\"description\":\"Deleted\"},\"404\":{\"description\":\"Not one of this account's\"}}}},"
         "\"/api/v1/me/fetch-accounts/{id}/download\":{\"post\":{\"summary\":\"Collect from one of the signed-in account's other accounts now\",\"description\":\"The next collection is set to now; watch locked in GET until it is false.\",\"responses\":{\"202\":{\"description\":\"Queued\"},\"404\":{\"description\":\"Not one of this account's\"}}}}";
      return AnsiString(paths);
   }

   HttpResponse
   RestApiServer::HandleMeFetchAccounts_(const Caller &caller, RouteKind kind, __int64 fetchAccountId, const AnsiString &requestBody)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      // ReaderFetchAccountsMax is a setting with levels: the account's own
      // ceiling, its domain's, or the server's (docs/SettingLevels.md).
      const int ceiling = EffectiveSetting::ResolveInt(_T("reader_fetch_accounts_max"), std::shared_ptr<const Domain>(), account);
      if (ceiling <= 0)
         return BuildResponse_(403, "{\"error\":\"adding other accounts is not offered on this server\"}");

      const String address = account->GetAddress();

      switch (kind)
      {
      case RouteMeFetchAccounts:
         return HandleListFetchAccounts_(address);

      case RouteMeFetchAccount:
         return HandleGetFetchAccount_(address, fetchAccountId);

      case RouteMeFetchAccountDelete:
         if (AdministratorsOwn(account->GetID(), fetchAccountId))
            return BuildResponse_(403, "{\"error\":\"this account was set up by an administrator, who changes or removes it\"}");
         return HandleDeleteFetchAccount_(address, fetchAccountId);

      case RouteMeFetchAccountDownload:
         return HandleDownloadFetchAccount_(address, fetchAccountId);

      case RouteMeFetchAccountCreate:
      case RouteMeFetchAccountUpdate:
      {
         const bool creating = kind == RouteMeFetchAccountCreate;
         if (creating)
         {
            FetchAccounts existing(account->GetID());
            existing.Refresh();
            if ((int) existing.GetSnapshot().size() >= ceiling)
            {
               AnsiString body;
               body.Format("{\"error\":\"an account may collect from at most %d other accounts\"}", ceiling);
               return BuildResponse_(403, body);
            }
         }

         if (!creating && AdministratorsOwn(account->GetID(), fetchAccountId))
            return BuildResponse_(403, "{\"error\":\"this account was set up by an administrator, who changes or removes it\"}");

         if (!creating)
         {
            FetchAccounts existing(account->GetID());
            existing.Refresh();
            std::shared_ptr<FetchAccount> current = existing.GetItemByDBID(fetchAccountId);
            JsonValue asked;
            if (current && !current->GetOAuthProvider().IsEmpty() && ParseBody(requestBody, asked))
            {
               for (const char *key : SignedInKeys)
               {
                  if (asked.Get(key))
                     return BuildResponse_(400, AnsiString("{\"error\":\"an account signed in with a provider keeps its server and address; remove it and add another (") + key + ")\"}");
               }
            }
         }

         AnsiString problem;
         const AnsiString narrowed = ReaderBody(requestBody, creating, problem);
         if (narrowed.IsEmpty())
            return BuildResponse_(400, "{\"error\":\"" + JsonEscape_(problem) + "\"}");

         return creating ? HandleCreateFetchAccount_(address, narrowed)
                         : HandleUpdateFetchAccount_(address, fetchAccountId, narrowed);
      }

      default:
         return BuildResponse_(404, "{\"error\":\"not found\"}");
      }
   }

   AnsiString
   RestApiServer::OpenApiReaderFetchImportPaths_()
   {
      static const char *paths =
         ",\"/api/v1/me/fetch-accounts/import\":{"
         "\"get\":{\"summary\":\"The providers the signed-in account can import from with a sign-in\",\"description\":\"{\\\"google\\\":bool,\\\"microsoft\\\":bool}: true where an administrator has registered this server with the provider (ImportOAuth*ClientId, the secret Google requires, both endpoints) and set ImportOAuthRedirectBaseUrl. 403 when ReaderFetchAccountsMax is 0.\",\"responses\":{\"200\":{\"description\":\"The providers\"},\"403\":{\"description\":\"Not offered on this server\"}}},"
         "\"post\":{\"summary\":\"Start signing in to Gmail or Outlook.com to import from it\",\"description\":\"Body: provider (google or microsoft), mirror_folders (default true: bring the folders over, the reader choosing which once the other server has listed them) and days_to_keep_messages (-1, the default, leaves the mail there). Answers the provider's authorize address as url, for the page to send the browser to, and sets the hmimport cookie (HttpOnly, SameSite=Lax, ten minutes) the callback needs. The authorization code flow with PKCE (S256); the provider sends the browser back to <ImportOAuthRedirectBaseUrl>/portal/oauth/callback, which exchanges the code, reads the address from the id_token and makes an IMAP fetch account that logs in with XOAUTH2 - or, when the reader already has one for that provider and address, gives it the new grant. Then to /portal#/accounts?imported=<id>, or ?import_error=<sentence>.\",\"responses\":{\"200\":{\"description\":\"url\"},\"400\":{\"description\":\"No such provider, or a field refused\"},\"403\":{\"description\":\"Not offered, not set up for that provider, or the account has as many other accounts as it may\"}}}}";
      return AnsiString(paths);
   }

   HttpResponse
   RestApiServer::HandleMeFetchImport_(const Caller &caller, RouteKind kind, const AnsiString &requestBody)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      // ReaderFetchAccountsMax is a setting with levels: the account's own
      // ceiling, its domain's, or the server's (docs/SettingLevels.md).
      const int ceiling = EffectiveSetting::ResolveInt(_T("reader_fetch_accounts_max"), std::shared_ptr<const Domain>(), account);
      if (ceiling <= 0)
         return BuildResponse_(403, "{\"error\":\"adding other accounts is not offered on this server\"}");

      if (kind == RouteMeFetchImport)
      {
         AnsiString body;
         body.Format("{\"google\":%hs,\"microsoft\":%hs}",
            ReaderOAuth::Configured(*ReaderOAuth::Find("google")) ? "true" : "false",
            ReaderOAuth::Configured(*ReaderOAuth::Find("microsoft")) ? "true" : "false");
         return BuildResponse_(200, body);
      }

      JsonValue body;
      if (!ParseBody(requestBody, body))
         return BuildResponse_(400, "{\"error\":\"the body must be a JSON object\"}");

      for (const std::pair<std::string, JsonValue> &member : body.Members())
      {
         if (member.first != "provider" && member.first != "mirror_folders" && member.first != "days_to_keep_messages")
            return BuildResponse_(400, "{\"error\":\"not a field an import takes: " + JsonEscape_(AnsiString(member.first.substr(0, 48).c_str())) + "\"}");
      }

      const ReaderOAuth::Provider *provider = ReaderOAuth::Find(AnsiString(body.GetString("provider").c_str()));
      if (!provider)
         return BuildResponse_(400, "{\"error\":\"provider must be google or microsoft\"}");

      bool mirror = true;
      long days = -1;
      AnsiString error;
      if (!ReadBool(body, "mirror_folders", mirror, error) || !ReadInteger(body, "days_to_keep_messages", -1, 100000, days, error))
         return BuildResponse_(400, "{\"error\":\"" + JsonEscape_(error) + "\"}");

      if (!ReaderOAuth::Configured(*provider))
         return BuildResponse_(403, AnsiString("{\"error\":\"signing in with ") + provider->label + " is not set up on this server\"}");

      FetchAccounts existing(account->GetID());
      existing.Refresh();
      if ((int) existing.GetSnapshot().size() >= ceiling)
      {
         AnsiString refusal;
         refusal.Format("{\"error\":\"an account may collect from at most %d other accounts\"}", ceiling);
         return BuildResponse_(403, refusal);
      }

      AnsiString url, nonce, problem;
      if (!ReaderOAuth::Begin(account->GetID(), *provider, mirror, (int) days, url, nonce, problem))
         return BuildResponse_(503, "{\"error\":\"" + JsonEscape_(problem) + "\"}");

      // SameSite=Lax, not Strict: the browser comes back from the provider's
      // site, a top-level navigation Lax lets the cookie ride and Strict does not.
      // Path=/portal/oauth: no other request carries it.
      AnsiString cookie;
      cookie.Format("Set-Cookie: hmimport=%hs; Path=/portal/oauth; HttpOnly; SameSite=Lax; Max-Age=600%hs\r\n",
         nonce.c_str(), use_tls_ ? "; Secure" : "");

      LOG_APPLICATION("RestApi: " + account->GetAddress() + " started signing in to " + String(provider->label) + " to import from it.");

      return BuildResponse_(200, "{\"url\":\"" + JsonEscape_(url) + "\"}", cookie);
   }

   HttpResponse
   RestApiServer::HandlePortalOAuthCallback_(const AnsiString &request, const AnsiString &query)
   {
      AnsiString headers;
      headers.Format("Set-Cookie: hmimport=; Path=/portal/oauth; HttpOnly; SameSite=Lax; Max-Age=0%hs\r\nCache-Control: no-store\r\nReferrer-Policy: no-referrer\r\n",
         use_tls_ ? "; Secure" : "");

      auto back = [&headers](const AnsiString &fragment)
      {
         return BuildResponse_(302, "{}", "Location: /portal#/accounts" + fragment + "\r\n" + headers);
      };

      AnsiString nonce;
      std::vector<AnsiString> cookies = StringParser::SplitString(GetHeader_(request, "cookie"), ";");
      for (AnsiString part : cookies)
      {
         part.Trim();
         if (part.StartsWith("hmimport="))
            nonce = part.Mid(9);
      }

      const AnsiString state = QueryParameter_(query, "state");
      const AnsiString code = QueryParameter_(query, "code");
      const AnsiString providerError = QueryParameter_(query, "error");

      ReaderOAuth::Grant grant;
      AnsiString problem;
      if (!ReaderOAuth::Complete(state, nonce, providerError.IsEmpty() ? code : AnsiString(), grant, problem))
      {
         if (!providerError.IsEmpty())
            problem = "the provider did not complete the sign-in (" + providerError.Mid(0, 60) + ")";
         return back("?import_error=" + HttpsClient::FormEncode(problem));
      }

      // A migration job's sign-in (RestApiMigrations.cpp): the grant is the
      // job's, for a mailbox that is not an account here, and the answer is a
      // page for its owner rather than the webmail's accounts page.
      if (grant.migration_job_id > 0)
      {
         HttpResponse signedIn = CompleteMigrationSignIn_(grant.migration_job_id, grant.migration_link, grant.address, grant.refresh_token, grant.provider);
         signedIn.extra_headers += headers;
         return signedIn;
      }

      const ReaderOAuth::Provider *provider = ReaderOAuth::Find(grant.provider);
      std::shared_ptr<Account> account = std::shared_ptr<Account>(new Account());
      if (!provider || !PersistentAccount::ReadObject(account, grant.account_id) || account->GetID() == 0)
         return back("?import_error=" + HttpsClient::FormEncode("the account this sign-in was for is gone"));

      FetchAccounts fetchAccounts(account->GetID());
      fetchAccounts.Refresh();

      // Signed in again to an account already here - after a revoked grant, or
      // a changed password there: it gets the new grant, and nothing is added.
      for (std::shared_ptr<FetchAccount> known : fetchAccounts.GetSnapshot())
      {
         if (!known || known->GetOAuthProvider() != String(grant.provider) || known->GetUsername().CompareNoCase(grant.address) != 0)
            continue;

         known->SetOAuthToken(String(grant.refresh_token));
         known->SetActive(true);
         if (!PersistentFetchAccount::SaveObject(known))
            return back("?import_error=" + HttpsClient::FormEncode("the account could not be saved; see the error log"));
         ReaderOAuth::Forget(known->GetID());

         LOG_APPLICATION("RestApi: " + account->GetAddress() + " signed in to " + String(provider->label) + " again as " + grant.address + ".");

         AnsiString again;
         again.Format("?imported=%I64d", known->GetID());
         return back(again);
      }

      if ((int) fetchAccounts.GetSnapshot().size() >= EffectiveSetting::ResolveInt(_T("reader_fetch_accounts_max"), std::shared_ptr<const Domain>(), account))
         return back("?import_error=" + HttpsClient::FormEncode("this account collects from as many other accounts as it may"));

      std::shared_ptr<FetchAccount> fetchAccount = std::shared_ptr<FetchAccount>(new FetchAccount());
      fetchAccount->SetAccountID(account->GetID());
      fetchAccount->SetName(String(provider->label) + _T(" - ") + grant.address);
      fetchAccount->SetServerAddress(String(provider->imap_host));
      fetchAccount->SetPort(provider->imap_port);
      fetchAccount->SetServerType(FetchAccount::IMAP);
      fetchAccount->SetConnectionSecurity(CSSSL);
      fetchAccount->SetUsername(grant.address);
      fetchAccount->SetPassword(String());
      fetchAccount->SetActive(true);
      fetchAccount->SetMinutesBetweenTry(30);
      fetchAccount->SetDaysToKeep(grant.days_to_keep);
      fetchAccount->SetUseAntiSpam(true);
      fetchAccount->SetUseAntiVirus(true);
      fetchAccount->SetProcessMIMERecipients(false);
      fetchAccount->SetProcessMIMEDate(false);
      fetchAccount->SetEnableRouteRecipients(false);
      fetchAccount->SetMIMERecipientHeaders(String());
      fetchAccount->SetMirrorFolders(grant.mirror);
      fetchAccount->SetMirrorSkip(grant.mirror ? String(_T("*")) : String());
      fetchAccount->SetOAuthProvider(String(grant.provider));
      fetchAccount->SetOAuthToken(String(grant.refresh_token));

      if (!PersistentFetchAccount::SaveObject(fetchAccount))
         return back("?import_error=" + HttpsClient::FormEncode("the account could not be saved; see the error log"));

      if (std::shared_ptr<ExternalFetchManager> fetcher = Application::Instance()->GetExternalFetchManager())
         fetcher->SetCheckNow();

      LOG_APPLICATION("RestApi: " + account->GetAddress() + " signed in to " + String(provider->label) + " as " + grant.address + " and collects from it.");

      AnsiString made;
      made.Format("?imported=%I64d", fetchAccount->GetID());
      return back(made);
   }
}
