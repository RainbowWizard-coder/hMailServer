// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The domain white and black list (discussion #285) as a resource. See
// RestApiServer.h and Common/AntiSpam/DomainPolicyList.h.
//
//    /api/v1/domain-policy-list        GET lists, POST adds
//    /api/v1/domain-policy-list/{id}   PUT changes, DELETE removes
//
// In the shape of the anti-spam lists in RestApiAntiSpamLists.cpp: one entry
// that every route answers with, and a body read whole before anything is
// applied, so that a wrong type, an unknown field or a value the row refuses
// is a 400 naming it and nothing changes. Each write reaches the same
// PersistentDomainPolicyEntry the COM members reach, and the persister marks
// DomainPolicyList's cache for reload, so a row is in force for the next
// session.

#include "StdAfx.h"
#include "RestApiServer.h"
#include "JsonDocument.h"
#include "Unicode.h"
#include "../Util/Time.h"
#include "../BO/DomainPolicyEntries.h"
#include "../BO/DomainPolicyEntry.h"
#include "../Persistence/PersistentDomainPolicyEntry.h"
#include "../Persistence/PersistenceMode.h"

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

   HttpResponse RefusalText(const Bridge &bridge, int status, const String &sentence)
   {
      return bridge.respond(status, "{\"error\":\"" + Quoted(bridge, sentence) + "\"}", "");
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

   AnsiString EntryJson(const Bridge &bridge, const std::shared_ptr<DomainPolicyEntry> &item)
   {
      AnsiString entry;
      entry.Format("{\"id\":%I64d,\"pattern\":\"%hs\",\"match_on\":\"%hs\",\"action\":\"%hs\",\"note\":\"%hs\",\"created\":\"%hs\"}",
         item->GetID(),
         Quoted(bridge, item->GetPattern()).c_str(),
         Quoted(bridge, DomainPolicyEntry::MatchOnWord(item->GetMatchOn())).c_str(),
         Quoted(bridge, DomainPolicyEntry::ActionWord(item->GetAction())).c_str(),
         Quoted(bridge, item->GetNote()).c_str(),
         Quoted(bridge, Time::GetTimeStampFromDateTime(item->GetCreated())).c_str());
      return entry;
   }

   // The body onto the item: everything read and checked first, applied last,
   // so a refusal leaves the item as it was. A new item matches on both keys
   // and is white unless the body says otherwise - white, because the row
   // that refuses mail is the one nobody should get by leaving a field out.
   bool ReadFields(const JsonValue &body, std::shared_ptr<DomainPolicyEntry> item, bool creating, AnsiString &error, String &refusal)
   {
      static const char *const keys[] = { "pattern", "match_on", "action", "note" };
      if (UnknownKey(body, keys, sizeof(keys) / sizeof(keys[0]), error))
         return false;

      String pattern = item->GetPattern();
      String matchOnWord = DomainPolicyEntry::MatchOnWord(item->GetMatchOn());
      String actionWord = DomainPolicyEntry::ActionWord(item->GetAction());
      String note = item->GetNote();

      if (!ReadString(body, "pattern", pattern, error) ||
          !ReadString(body, "match_on", matchOnWord, error) ||
          !ReadString(body, "action", actionWord, error) ||
          !ReadString(body, "note", note, error))
         return false;

      if (creating && !body.Get("pattern"))
      {
         error = "pattern is required";
         return false;
      }

      if (!DomainPolicyEntry::ValidatePattern(pattern, refusal))
         return false;

      DomainPolicyEntry::MatchOn matchOn = DomainPolicyEntry::MatchBoth;
      if (!DomainPolicyEntry::ReadMatchOn(matchOnWord, matchOn))
      {
         error = "match_on must be envelope, helo or both";
         return false;
      }

      DomainPolicyEntry::Action action = DomainPolicyEntry::ActionWhite;
      if (!DomainPolicyEntry::ReadAction(actionWord, action))
      {
         error = "action must be white or black";
         return false;
      }

      note.Trim();
      if (note.GetLength() > 255)
      {
         error = "note is at most 255 characters";
         return false;
      }

      item->SetPattern(pattern);
      item->SetMatchOn(matchOn);
      item->SetAction(action);
      item->SetNote(note);
      return true;
   }

   HttpResponse List(const Bridge &bridge)
   {
      DomainPolicyEntries collection;
      if (!collection.Refresh())
         return Refusal(bridge, 503, "the domain policy list could not be read; see the error log");

      AnsiString json = "[";
      int count = 0;
      for (std::shared_ptr<DomainPolicyEntry> item : collection.GetSnapshot())
      {
         if (!item)
            continue;
         if (count > 0)
            json += ",";
         json += EntryJson(bridge, item);
         count++;
      }
      json += "]";
      return bridge.respond(200, json, "");
   }

   HttpResponse Create(const Bridge &bridge, const AnsiString &requestBody)
   {
      JsonValue body;
      if (!ParseObjectBody(requestBody, body))
         return Refusal(bridge, 400, "the body must be a JSON object");

      std::shared_ptr<DomainPolicyEntry> item(new DomainPolicyEntry);
      AnsiString error;
      String refusal;
      if (!ReadFields(body, item, true, error, refusal))
         return refusal.IsEmpty() ? Refusal(bridge, 400, error) : RefusalText(bridge, 400, refusal);

      String saveError;
      if (!PersistentDomainPolicyEntry::SaveObject(item, saveError, PersistenceModeNormal))
      {
         if (saveError.IsEmpty())
            return Refusal(bridge, 500, "the domain policy list entry could not be saved; see the error log");
         return RefusalText(bridge, 400, saveError);
      }

      LOG_APPLICATION("RestApi: Domain policy list entry " + item->GetPattern() + " (" +
                      DomainPolicyEntry::MatchOnWord(item->GetMatchOn()) + ", " +
                      DomainPolicyEntry::ActionWord(item->GetAction()) + ") created.");

      return bridge.respond(201, EntryJson(bridge, item), "");
   }

   HttpResponse Update(const Bridge &bridge, __int64 id, const AnsiString &requestBody)
   {
      JsonValue body;
      if (!ParseObjectBody(requestBody, body))
         return Refusal(bridge, 400, "the body must be a JSON object");

      DomainPolicyEntries collection;
      if (!collection.Refresh())
         return Refusal(bridge, 503, "the domain policy list could not be read; see the error log");

      std::shared_ptr<DomainPolicyEntry> item = collection.GetItemByDBID(id);
      if (!item)
         return Refusal(bridge, 404, "domain policy list entry not found");

      AnsiString error;
      String refusal;
      if (!ReadFields(body, item, false, error, refusal))
         return refusal.IsEmpty() ? Refusal(bridge, 400, error) : RefusalText(bridge, 400, refusal);

      String saveError;
      if (!PersistentDomainPolicyEntry::SaveObject(item, saveError, PersistenceModeNormal))
      {
         if (saveError.IsEmpty())
            return Refusal(bridge, 500, "the domain policy list entry could not be saved; see the error log");
         return RefusalText(bridge, 400, saveError);
      }

      LOG_APPLICATION("RestApi: Domain policy list entry " + item->GetPattern() + " (" +
                      DomainPolicyEntry::MatchOnWord(item->GetMatchOn()) + ", " +
                      DomainPolicyEntry::ActionWord(item->GetAction()) + ") updated.");

      return bridge.respond(200, EntryJson(bridge, item), "");
   }

   HttpResponse Delete(const Bridge &bridge, __int64 id)
   {
      DomainPolicyEntries collection;
      if (!collection.Refresh())
         return Refusal(bridge, 503, "the domain policy list could not be read; see the error log");

      std::shared_ptr<DomainPolicyEntry> item = collection.GetItemByDBID(id);
      if (!item)
         return Refusal(bridge, 404, "domain policy list entry not found");

      const String pattern = item->GetPattern();

      if (!collection.DeleteItemByDBID(id))
         return Refusal(bridge, 500, "the domain policy list entry could not be deleted; see the error log");

      LOG_APPLICATION("RestApi: Domain policy list entry " + pattern + " deleted.");

      return bridge.respond(200, "{\"deleted\":true}", "");
   }

   const char *DomainPolicyListPaths =
      ",\"/api/v1/domain-policy-list\":{"
      "\"get\":{\"summary\":\"List the domain white and black list\",\"description\":\"The sending organisations this server has decided about by domain rather than by address (AntiSpam.DomainPolicyList over COM). Each entry: id, pattern (example.com for that domain alone, .example.com for the domain and everything beneath it), match_on (envelope: the envelope sender's domain; helo: the HELO/EHLO name; both), action (white: the session skips greylisting and the refusals the spam score would make while every test still runs and logs; black: 550 at MAIL FROM before greylisting, scoring or any lookup), note and created. Consulted for unauthenticated inbound sessions of an IP range that runs the spam tests only, and never to override authentication. Distinct from the per-recipient allow and block lists, which act at delivery. Server-wide; refused for domain-restricted keys.\",\"responses\":{\"200\":{\"description\":\"Array of domain policy list entries\"}}},"
      "\"post\":{\"summary\":\"Add a domain to the white or black list\",\"description\":\"Body: pattern (required; a domain, or .domain for the domain and everything beneath it), match_on (envelope, helo or both; both by default), action (white or black; white by default) and note. The pair of pattern and match_on is unique. In force for the next session. Server-wide; refused for domain-restricted and read-only keys.\",\"requestBody\":{\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"required\":[\"pattern\"],\"properties\":{\"pattern\":{\"type\":\"string\"},\"match_on\":{\"type\":\"string\",\"enum\":[\"envelope\",\"helo\",\"both\"]},\"action\":{\"type\":\"string\",\"enum\":[\"white\",\"black\"]},\"note\":{\"type\":\"string\"}}}}}},\"responses\":{\"201\":{\"description\":\"Created: the entry as the listing shows it\"},\"400\":{\"description\":\"pattern missing or not a domain, a word that is not one of the choices, a pair already listed, a field of the wrong type or an unknown field (error names it)\"}}}},"
      "\"/api/v1/domain-policy-list/{id}\":{"
      "\"put\":{\"summary\":\"Change a domain policy list entry\",\"description\":\"Body: any subset of the fields POST takes; a field left out keeps its value. Nothing changes when the body is refused. Server-wide; refused for domain-restricted and read-only keys.\",\"requestBody\":{\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"properties\":{\"pattern\":{\"type\":\"string\"},\"match_on\":{\"type\":\"string\",\"enum\":[\"envelope\",\"helo\",\"both\"]},\"action\":{\"type\":\"string\",\"enum\":[\"white\",\"black\"]},\"note\":{\"type\":\"string\"}}}}}},\"responses\":{\"200\":{\"description\":\"The entry as saved\"},\"400\":{\"description\":\"A field refused (error names it); nothing changed\"},\"404\":{\"description\":\"Unknown id\"}}},"
      "\"delete\":{\"summary\":\"Remove a domain policy list entry\",\"description\":\"A black row removed stops refusing at once; a white row removed puts the domain back under greylisting and the score for the next session.\",\"responses\":{\"200\":{\"description\":\"Deleted\"},\"404\":{\"description\":\"Unknown id\"}}}}";
}

namespace HM
{
   AnsiString
   RestApiServer::OpenApiDomainPolicyListPaths_()
   {
      return AnsiString(DomainPolicyListPaths);
   }

   HttpResponse
   RestApiServer::HandleDomainPolicyList_(RouteKind kind, __int64 id, const AnsiString &requestBody)
   {
      Bridge bridge = { &RestApiServer::JsonEscape_, &RestApiServer::BuildResponse_ };

      switch (kind)
      {
      case RouteDomainPolicyList:
         return List(bridge);
      case RouteDomainPolicyCreate:
         return Create(bridge, requestBody);
      case RouteDomainPolicyUpdate:
         return Update(bridge, id, requestBody);
      case RouteDomainPolicyDelete:
         return Delete(bridge, id);
      default:
         break;
      }

      return BuildResponse_(404, "{\"error\":\"not found\"}");
   }
}
