// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The sender list as a resource, at all three of its scopes, and the quarantine
// digest's own two routes. See RestApiServer.h.
//
//    /api/v1/sender-list                     the server's, for an administrator
//    /api/v1/domains/{domain}/sender-list     one domain's
//    /api/v1/me/sender-list                   the signed-in account's own
//    /api/v1/me/quarantine-digest             whether the digest reaches this
//                                             account, read and written by its
//                                             owner
//
// All three list routes answer the same object and take the same body, because
// they are the same list at three distances - the only thing that differs is who
// may write it and what `scope` says. That is on purpose: one shape means one
// editor component in the Deck, one in the webmail, one set of translations, and
// one place to look when an entry does not do what somebody expected.
//
// Written in the shape RestApiAntiSpamLists.cpp established, with the same
// Bridge: the body is read whole and checked before anything is applied, so a
// wrong type, an unknown field or a value the owner refuses is a 400 naming it
// and nothing changes.
//
// THE ONE RULE THAT IS NOT OBVIOUS FROM THE ROUTES
//
// A create whose value this owner has already listed does not make a second row;
// it rewrites the first. "example.com is allowed" and "example.com is blocked"
// cannot both be true of one list, and two rows that disagree are a question the
// reader never meant to ask. The answer is 200 with the row, rather than 201,
// and the response says which id it wrote.

#include "StdAfx.h"
#include "RestApiServer.h"
#include "JsonDocument.h"
#include "Unicode.h"

#include "../AntiSpam/QuarantineDigest.h"
#include "../AntiSpam/SenderListCache.h"
#include "../AntiSpam/SenderPattern.h"
#include "../BO/Account.h"
#include "../BO/Domain.h"
#include "../BO/Domains.h"
#include "../BO/SenderListEntries.h"
#include "../BO/SenderListEntry.h"
#include "../Cache/CacheContainer.h"
#include "../Persistence/PersistentSenderListEntry.h"
#include "../Util/Time.h"
#include "../Util/VariantDateTime.h"

#include <cmath>
#include <string>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace
{
   using namespace HM;

   struct Bridge
   {
      AnsiString (*escape)(const AnsiString &value);
      HttpResponse (*respond)(int statusCode, const AnsiString &body, const AnsiString &extraHeaders);
   };

   AnsiString Utf8(const String &value)
   {
      AnsiString utf8;
      Unicode::WideToMultiByte(value, utf8);
      return utf8;
   }

   String Utf8ToString(const std::string &utf8)
   {
      String value;
      Unicode::MultiByteToWide(AnsiString(utf8.c_str()), value);
      return value;
   }

   AnsiString Quoted(const Bridge &bridge, const String &value)
   {
      return bridge.escape(Utf8(value));
   }

   HttpResponse Refusal(const Bridge &bridge, int status, const AnsiString &sentence)
   {
      return bridge.respond(status, "{\"error\":\"" + bridge.escape(sentence) + "\"}", "");
   }

   bool ParseObjectBody(const AnsiString &requestBody, JsonValue &body)
   {
      std::string parseError;
      std::string text(requestBody.c_str(), (size_t) requestBody.GetLength());
      return JsonValue::Parse(text, body, parseError) && body.IsObject();
   }

   bool ReadString(const JsonValue &object, const char *key, String &out, AnsiString &error)
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

   bool UnknownKey(const JsonValue &object, const char *const *known, size_t knownCount, AnsiString &error)
   {
      for (const std::pair<std::string, JsonValue> &member : object.Members())
      {
         bool found = false;
         for (size_t i = 0; i < knownCount; i++)
         {
            if (member.first == known[i])
            {
               found = true;
               break;
            }
         }

         if (!found)
         {
            error = AnsiString("unknown field: ") + AnsiString(member.first.substr(0, 48).c_str());
            return true;
         }
      }

      return false;
   }

   const char *ScopeText(int scope)
   {
      switch (scope)
      {
      case SenderListEntry::ScopeAccount:
         return "account";
      case SenderListEntry::ScopeDomain:
         return "domain";
      default:
         return "server";
      }
   }

   AnsiString EntryJson(const Bridge &bridge, const std::shared_ptr<SenderListEntry> &entry)
   {
      AnsiString json;
      json.Format("{\"id\":%I64d,\"scope\":\"%hs\",\"value\":\"%hs\",\"blocked\":%hs,\"reason\":\"%hs\",\"expires\":%hs,\"expires_time\":\"%hs\",\"expired\":%hs,\"created\":\"%hs\"}",
         entry->GetID(),
         ScopeText(entry->GetScope()),
         Quoted(bridge, entry->GetValue()).c_str(),
         entry->GetBlocked() ? "true" : "false",
         Quoted(bridge, entry->GetReason()).c_str(),
         entry->GetExpires() ? "true" : "false",
         entry->GetExpires() ? Quoted(bridge, Time::GetTimeStampFromDateTime(entry->GetExpiresTime())).c_str() : "",
         entry->HasExpired() ? "true" : "false",
         Quoted(bridge, Time::GetTimeStampFromDateTime(entry->GetCreated())).c_str());

      return json;
   }

   // The body onto the entry. Everything is read and checked before anything is
   // applied, so a refusal leaves the entry exactly as it was.
   bool ReadFields(const JsonValue &body, std::shared_ptr<SenderListEntry> entry, bool creating, AnsiString &error)
   {
      static const char *const keys[] = { "value", "blocked", "reason", "expires", "expires_time" };

      if (UnknownKey(body, keys, sizeof(keys) / sizeof(keys[0]), error))
         return false;

      String value = entry->GetValue();
      String reason = entry->GetReason();
      bool blocked = entry->GetBlocked();
      bool expires = entry->GetExpires();
      String expiresTime = entry->GetExpires() ? Time::GetTimeStampFromDateTime(entry->GetExpiresTime()) : String();

      if (!ReadString(body, "value", value, error) ||
          !ReadBool(body, "blocked", blocked, error) ||
          !ReadString(body, "reason", reason, error) ||
          !ReadBool(body, "expires", expires, error) ||
          !ReadString(body, "expires_time", expiresTime, error))
         return false;

      if (creating && value.IsEmpty())
      {
         error = "value is required: an address, or a domain that covers its subdomains";
         return false;
      }

      String problem;
      if (!SenderPattern::Validate(value, problem))
      {
         error = Utf8(problem);
         return false;
      }

      reason.Trim();
      if (reason.GetLength() > 255)
      {
         error = "reason is at most 255 characters";
         return false;
      }

      HM::DateTime expiresAt;

      if (expires)
      {
         expiresTime.Trim();

         if (expiresTime.IsEmpty())
         {
            error = "an entry that expires needs expires_time, as YYYY-MM-DD HH:MM:SS";
            return false;
         }

         expiresAt = Time::GetDateFromSystemDate(expiresTime);

         // The same refusal PersistentSecurityRange makes, for the same reason:
         // an expiry flag without a readable time is an entry the sweep removes
         // before anybody has used it, and the caller would never know why.
         if (expiresAt.GetStatus() != HM::DateTime::valid)
         {
            error = "expires_time is not a date; it is written YYYY-MM-DD HH:MM:SS";
            return false;
         }
      }

      entry->SetValue(value);
      entry->SetBlocked(blocked);
      entry->SetReason(reason);
      entry->SetExpires(expires);

      if (expires)
         entry->SetExpiresTime(expiresAt);

      return true;
   }

   HttpResponse ListEntries(const Bridge &bridge, int scope, __int64 ownerId)
   {
      SenderListEntries entries(scope, ownerId);

      if (!entries.Refresh())
         return Refusal(bridge, 503, "the sender list could not be read");

      AnsiString json = "{\"scope\":\"";
      json += ScopeText(scope);
      json += "\",\"items\":[";

      int count = 0;
      for (std::shared_ptr<SenderListEntry> entry : entries.GetSnapshot())
      {
         if (!entry)
            continue;

         if (count > 0)
            json += ",";

         json += EntryJson(bridge, entry);
         count++;
      }

      json += "]}";

      return bridge.respond(200, json, "");
   }

   HttpResponse CreateEntry(const Bridge &bridge, int scope, __int64 ownerId, const AnsiString &requestBody)
   {
      JsonValue body;
      if (!ParseObjectBody(requestBody, body))
         return Refusal(bridge, 400, "the body must be a JSON object");

      std::shared_ptr<SenderListEntry> entry = std::shared_ptr<SenderListEntry>(new SenderListEntry());
      entry->SetScope(scope);
      entry->SetOwnerID(ownerId);

      AnsiString error;
      if (!ReadFields(body, entry, true, error))
         return Refusal(bridge, 400, error);

      // One row per value per owner; see the file comment. The existing row is
      // rewritten rather than joined by a contradiction.
      __int64 existingId = 0;
      const bool replacing = PersistentSenderListEntry::Find(scope, ownerId, entry->GetValue(), existingId);

      if (replacing)
         entry->SetID(existingId);

      // A per-account list is a person's own, not a place to put ten thousand
      // rows: a ceiling keeps one account from filling the table every delivery
      // reads. Generous enough that nobody sensible meets it.
      if (!replacing)
      {
         SenderListEntries existing(scope, ownerId);

         if (existing.Refresh() && existing.GetCount() >= 1000)
            return Refusal(bridge, 400, "this list already holds a thousand entries; remove some first");
      }

      if (!PersistentSenderListEntry::SaveObject(entry))
         return Refusal(bridge, 500, "the entry could not be saved; see the error log");

      LOG_APPLICATION(Formatter::Format("RestApi: {0} sender-list entry {1} ({2}) saved.",
         String(ScopeText(scope)), entry->GetValue(), entry->GetBlocked() ? _T("blocked") : _T("allowed")));

      return bridge.respond(replacing ? 200 : 201, EntryJson(bridge, entry), "");
   }

   HttpResponse UpdateEntry(const Bridge &bridge, int scope, __int64 ownerId, __int64 id, const AnsiString &requestBody)
   {
      SenderListEntries entries(scope, ownerId);

      if (!entries.Refresh())
         return Refusal(bridge, 503, "the sender list could not be read");

      // Another owner's entry is "not found", never "forbidden": the id space is
      // shared between every scope, and a refusal that differed would confirm
      // that somebody else's entry exists.
      std::shared_ptr<SenderListEntry> entry = entries.GetItemByDBID(id);

      if (!entry)
         return Refusal(bridge, 404, "no such entry");

      JsonValue body;
      if (!ParseObjectBody(requestBody, body))
         return Refusal(bridge, 400, "the body must be a JSON object");

      AnsiString error;
      if (!ReadFields(body, entry, false, error))
         return Refusal(bridge, 400, error);

      if (!PersistentSenderListEntry::SaveObject(entry))
         return Refusal(bridge, 500, "the entry could not be saved; see the error log");

      return bridge.respond(200, EntryJson(bridge, entry), "");
   }

   HttpResponse DeleteEntry(const Bridge &bridge, int scope, __int64 ownerId, __int64 id)
   {
      SenderListEntries entries(scope, ownerId);

      if (!entries.Refresh())
         return Refusal(bridge, 503, "the sender list could not be read");

      std::shared_ptr<SenderListEntry> entry = entries.GetItemByDBID(id);

      if (!entry)
         return Refusal(bridge, 404, "no such entry");

      if (!PersistentSenderListEntry::DeleteObject(entry))
         return Refusal(bridge, 500, "the entry could not be removed; see the error log");

      return bridge.respond(200, "{\"deleted\":true}", "");
   }

   // Begins with a comma and does not end with one, as every piece that
   // HandleOpenApi_ appends does: it goes after another path, and the path after
   // it brings its own comma. The other way round the document the Deck builds
   // its forms from was not JSON, and the Deck's JSON.parse of it threw.
   const char *const SenderListPaths =
      ",\"/api/v1/sender-list\":{"
      "\"get\":{\"summary\":\"The server's allow and block list\",\"description\":\"Every address and domain the server has been told is wanted, or is not, for every account. An entry with an @ is one exact address; without one it is a whole domain including its subdomains. An entry covers a message when it matches the envelope sender or the address in the From header; a message with the null sender is never decided by any list. The account's own list is consulted first, then its domain's, then this one, and the nearest scope that says anything is the only one that decides - see the sender-list documentation. Each entry: id, scope, value, blocked, reason, expires, expires_time, expired, created.\",\"responses\":{\"200\":{\"description\":\"scope, items\"}}},"
      "\"post\":{\"summary\":\"Add an entry to the server's list\",\"description\":\"Body: value (an address, a domain, or a URL such as https://host/anything, stored as host/ and covering links to that host and its subdomains; required), blocked (true to block, false - the default - to allow), reason (a note, at most 255 characters), expires (default false) and expires_time (YYYY-MM-DD HH:MM:SS, required when expires is true). A value this list already holds is rewritten rather than duplicated, and the answer is then 200 rather than 201.\",\"requestBody\":{\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"required\":[\"value\"],\"properties\":{\"value\":{\"type\":\"string\"},\"blocked\":{\"type\":\"boolean\"},\"reason\":{\"type\":\"string\"},\"expires\":{\"type\":\"boolean\"},\"expires_time\":{\"type\":\"string\"}}}}}},\"responses\":{\"201\":{\"description\":\"The entry\"},\"200\":{\"description\":\"The entry, which replaced one with the same value\"},\"400\":{\"description\":\"A value refused, with the reason in error\"}}}},"
      "\"/api/v1/sender-list/{id}\":{"
      "\"put\":{\"summary\":\"Change an entry of the server's list\",\"description\":\"The fields the body names are applied; the others are left as they are.\",\"responses\":{\"200\":{\"description\":\"The entry\"},\"404\":{\"description\":\"No such entry\"}}},"
      "\"delete\":{\"summary\":\"Remove an entry from the server's list\",\"responses\":{\"200\":{\"description\":\"Removed\"},\"404\":{\"description\":\"No such entry\"}}}},"
      "\"/api/v1/domains/{domain}/sender-list\":{"
      "\"get\":{\"summary\":\"One domain's allow and block list\",\"description\":\"The same object as the server's list, for every account of this domain. Consulted after an account's own list and before the server's.\",\"responses\":{\"200\":{\"description\":\"scope, items\"},\"404\":{\"description\":\"No such domain\"}}},"
      "\"post\":{\"summary\":\"Add an entry to a domain's list\",\"description\":\"The same body as the server's list.\",\"responses\":{\"201\":{\"description\":\"The entry\"},\"200\":{\"description\":\"The entry, which replaced one with the same value\"},\"400\":{\"description\":\"A value refused\"},\"404\":{\"description\":\"No such domain\"}}}},"
      "\"/api/v1/domains/{domain}/sender-list/{id}\":{"
      "\"put\":{\"summary\":\"Change an entry of a domain's list\",\"responses\":{\"200\":{\"description\":\"The entry\"},\"404\":{\"description\":\"No such domain or entry\"}}},"
      "\"delete\":{\"summary\":\"Remove an entry from a domain's list\",\"responses\":{\"200\":{\"description\":\"Removed\"},\"404\":{\"description\":\"No such domain or entry\"}}}},"
      "\"/api/v1/me/sender-list\":{"
      "\"get\":{\"summary\":\"The signed-in account's own safe senders and blocked senders\",\"description\":\"The account's own list, and only its own: what the domain or the server lists is not shown here, because it is not this person's to change. An allowed sender's mail skips the spam verdict for this account alone; a blocked sender's is held for review where the server holds spam, and marked as spam where it does not - never destroyed.\",\"responses\":{\"200\":{\"description\":\"scope, items\"}}},"
      "\"post\":{\"summary\":\"Add an address or domain to the signed-in account's list\",\"description\":\"Body: value (required), blocked, reason, expires, expires_time. A value already on this list is rewritten, so moving an address from safe to blocked is one POST.\",\"responses\":{\"201\":{\"description\":\"The entry\"},\"200\":{\"description\":\"The entry, which replaced one with the same value\"},\"400\":{\"description\":\"A value refused\"}}}},"
      "\"/api/v1/me/sender-list/{id}\":{"
      "\"put\":{\"summary\":\"Change one of the signed-in account's entries\",\"responses\":{\"200\":{\"description\":\"The entry\"},\"404\":{\"description\":\"Not this account's entry\"}}},"
      "\"delete\":{\"summary\":\"Remove one of the signed-in account's entries\",\"responses\":{\"200\":{\"description\":\"Removed\"},\"404\":{\"description\":\"Not this account's entry\"}}}},"
      "\"/api/v1/me/quarantine-digest\":{"
      "\"get\":{\"summary\":\"Whether the quarantine digest reaches the signed-in account\",\"description\":\"available says whether the account's domain sends one at all; hour is when; enabled is false when this account has asked to stop. last_sent is when the last one went, empty when none has.\",\"responses\":{\"200\":{\"description\":\"available, enabled, hour, last_sent\"}}},"
      "\"put\":{\"summary\":\"Start or stop the digest for the signed-in account\",\"description\":\"Body: enabled. Only the account's own delivery is affected: mail is still held either way, and the Held mail page still shows it. An account cannot switch the digest on for a domain that does not send one.\",\"requestBody\":{\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"required\":[\"enabled\"],\"properties\":{\"enabled\":{\"type\":\"boolean\"}}}}}},\"responses\":{\"200\":{\"description\":\"The state as saved\"},\"400\":{\"description\":\"enabled missing or not a boolean\"}}}}";
}

namespace HM
{
   AnsiString
   RestApiServer::OpenApiSenderListPaths_()
   {
      return AnsiString(SenderListPaths);
   }

   HttpResponse
   RestApiServer::HandleSenderLists_(const Route &route, const Caller &caller, const AnsiString &requestBody)
   {
      Bridge bridge = { &RestApiServer::JsonEscape_, &RestApiServer::BuildResponse_ };

      int scope = SenderListEntry::ScopeServer;
      __int64 ownerId = 0;

      switch (route.kind)
      {
      case RouteSenderListDomainList:
      case RouteSenderListDomainCreate:
      case RouteSenderListDomainUpdate:
      case RouteSenderListDomainDelete:
      {
         scope = SenderListEntry::ScopeDomain;

         std::shared_ptr<const Domain> domain = CacheContainer::Instance()->GetDomain(String(route.identifier));

         if (!domain)
            return Refusal(bridge, 404, "no such domain");

         ownerId = domain->GetID();
         break;
      }

      case RouteSenderListMeList:
      case RouteSenderListMeCreate:
      case RouteSenderListMeUpdate:
      case RouteSenderListMeDelete:
      {
         scope = SenderListEntry::ScopeAccount;

         if (!caller.account)
            return Refusal(bridge, 500, "internal error");

         ownerId = caller.account->GetID();
         break;
      }

      default:
         break;
      }

      switch (route.kind)
      {
      case RouteSenderListList:
      case RouteSenderListDomainList:
      case RouteSenderListMeList:
         return ListEntries(bridge, scope, ownerId);

      case RouteSenderListCreate:
      case RouteSenderListDomainCreate:
      case RouteSenderListMeCreate:
         return CreateEntry(bridge, scope, ownerId, requestBody);

      case RouteSenderListUpdate:
      case RouteSenderListDomainUpdate:
      case RouteSenderListMeUpdate:
         return UpdateEntry(bridge, scope, ownerId, route.record_id, requestBody);

      case RouteSenderListDelete:
      case RouteSenderListDomainDelete:
      case RouteSenderListMeDelete:
         return DeleteEntry(bridge, scope, ownerId, route.record_id);

      default:
         break;
      }

      return BuildResponse_(404, "{\"error\":\"not found\"}");
   }

   HttpResponse
   RestApiServer::HandleMeQuarantineDigest_(const Caller &caller, const AnsiString &method, const AnsiString &requestBody)
   {
      Bridge bridge = { &RestApiServer::JsonEscape_, &RestApiServer::BuildResponse_ };

      std::shared_ptr<const Account> account = caller.account;

      if (!account)
         return Refusal(bridge, 500, "internal error");

      std::shared_ptr<const Domain> domain = CacheContainer::Instance()->GetDomain(account->GetDomainID());

      const bool available = domain && domain->GetQuarantineDigest() && !QuarantineDigest::BaseUrl().IsEmpty();
      const int hour = domain ? domain->GetQuarantineDigestHour() : 0;

      QuarantineDigest::State state;

      if (!QuarantineDigest::GetState(account->GetID(), state))
         return Refusal(bridge, 503, "the digest settings could not be read");

      if (method == "PUT")
      {
         JsonValue body;
         if (!ParseObjectBody(requestBody, body))
            return Refusal(bridge, 400, "the body must be a JSON object");

         static const char *const keys[] = { "enabled" };
         AnsiString error;

         if (UnknownKey(body, keys, 1, error))
            return Refusal(bridge, 400, error);

         const JsonValue *member = body.Get("enabled");

         if (!member || !member->IsBool())
            return Refusal(bridge, 400, "enabled is required, as true or false");

         const bool wanted = member->AsBool();

         if (!QuarantineDigest::SetOptOut(account->GetID(), !wanted))
            return Refusal(bridge, 500, "that could not be saved; see the error log");

         state.opted_out = !wanted;
      }

      AnsiString json;
      json.Format("{\"available\":%hs,\"enabled\":%hs,\"hour\":%d,\"last_sent\":\"%hs\"}",
         available ? "true" : "false",
         (available && !state.opted_out) ? "true" : "false",
         hour,
         state.last_sent.GetStatus() == HM::DateTime::valid
            ? Quoted(bridge, Time::GetTimeStampFromDateTime(state.last_sent)).c_str()
            : "");

      return bridge.respond(200, json, "");
   }
}
