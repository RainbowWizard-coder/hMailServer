// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The per-domain greylisting bypass list as a resource. See RestApiServer.h.
//
//    /api/v1/greylisting-bypass       AntiSpam.GreyListingBypassEntries (InterfaceGreyListingBypassEntry)
//
// In the greylisting white list's shape (RestApiAntiSpamLists.cpp): a listing
// and a create at the collection's path, an update and a delete at /{id}, one
// entry that every route answers with, and a body read whole before anything
// is applied, so that a wrong type, an unknown field or a value the owner
// refuses is a 400 naming it and nothing changes. Each write reaches the same
// PersistentGreyListingBypassEntry::SaveObject or DeleteObject the COM item's
// Save and Delete reach, and each marks the copy the SMTP session reads
// (GreyListingBypass) stale, so a change is in force for the next RCPT TO.
//
// An entry: id, pattern, match (envelope, helo or both), require_spf,
// require_dkim, require_dmarc, note. The three flags rather than a number,
// because a flag is what the Deck's editor and hmctl can draw and say without a
// table of meanings beside it; the COM object has the same three.

#include "StdAfx.h"
#include "RestApiServer.h"
#include "JsonDocument.h"
#include "Unicode.h"
#include "../BO/GreyListingBypassEntries.h"
#include "../BO/GreyListingBypassEntry.h"
#include "../Persistence/PersistentGreyListingBypassEntry.h"
#include "../Persistence/PersistenceMode.h"
#include "../AntiSpam/AntiSpamConfiguration.h"

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

   AntiSpamConfiguration &AntiSpam()
   {
      return Configuration::Instance()->GetAntiSpamConfiguration();
   }

   AnsiString EntryJson(const Bridge &bridge, const std::shared_ptr<GreyListingBypassEntry> &item)
   {
      const int require = item->GetRequire();

      AnsiString entry;
      entry.Format("{\"id\":%I64d,\"pattern\":\"%hs\",\"match\":\"%hs\",\"require_spf\":%hs,\"require_dkim\":%hs,\"require_dmarc\":%hs,\"note\":\"%hs\"}",
         item->GetID(),
         Quoted(bridge, item->GetPattern()).c_str(),
         Utf8(GreyListingBypassEntry::MatchOnWord(item->GetMatchOn())).c_str(),
         (require & GreyListingBypassEntry::RequireSpf) ? "true" : "false",
         (require & GreyListingBypassEntry::RequireDkim) ? "true" : "false",
         (require & GreyListingBypassEntry::RequireDmarc) ? "true" : "false",
         Quoted(bridge, item->GetNote()).c_str());
      return entry;
   }

   // The body onto the item: everything read and checked first, applied last,
   // so a refusal leaves the item as it was. What the pattern must be, and that
   // something must be required, is the persistence layer's to say.
   bool ReadFields(const JsonValue &body, std::shared_ptr<GreyListingBypassEntry> item, AnsiString &error)
   {
      static const char *const keys[] = { "pattern", "match", "require_spf", "require_dkim", "require_dmarc", "note" };
      if (UnknownKey(body, keys, sizeof(keys) / sizeof(keys[0]), error))
         return false;

      String pattern = item->GetPattern(), note = item->GetNote();
      String matchWord = GreyListingBypassEntry::MatchOnWord(item->GetMatchOn());
      bool spf = (item->GetRequire() & GreyListingBypassEntry::RequireSpf) != 0;
      bool dkim = (item->GetRequire() & GreyListingBypassEntry::RequireDkim) != 0;
      bool dmarc = (item->GetRequire() & GreyListingBypassEntry::RequireDmarc) != 0;

      if (!ReadString(body, "pattern", pattern, error) ||
          !ReadString(body, "match", matchWord, error) ||
          !ReadBool(body, "require_spf", spf, error) ||
          !ReadBool(body, "require_dkim", dkim, error) ||
          !ReadBool(body, "require_dmarc", dmarc, error) ||
          !ReadString(body, "note", note, error))
         return false;

      int matchOn = 0;
      if (!GreyListingBypassEntry::MatchOnFromWord(matchWord, matchOn))
      {
         error = "match must be envelope, helo or both";
         return false;
      }

      item->SetPattern(pattern);
      item->SetMatchOn(matchOn);
      item->SetRequire((spf ? GreyListingBypassEntry::RequireSpf : 0) |
                       (dkim ? GreyListingBypassEntry::RequireDkim : 0) |
                       (dmarc ? GreyListingBypassEntry::RequireDmarc : 0));
      item->SetNote(note);
      return true;
   }

   HttpResponse SaveRefusal(const Bridge &bridge, const String &error)
   {
      if (error.IsEmpty())
         return Refusal(bridge, 500, "the greylisting bypass entry could not be saved; see the error log");
      return bridge.respond(400, "{\"error\":\"" + Quoted(bridge, error) + "\"}", "");
   }

   HttpResponse List(const Bridge &bridge)
   {
      std::shared_ptr<GreyListingBypassEntries> collection = AntiSpam().GetGreyListingBypassEntries();

      AnsiString json = "[";
      int count = 0;
      for (std::shared_ptr<GreyListingBypassEntry> item : collection->GetSnapshot())
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

      std::shared_ptr<GreyListingBypassEntry> item(new GreyListingBypassEntry);
      // A new entry requires nothing until the body says what; the persister
      // refuses one that still requires nothing.
      item->SetRequire(0);

      AnsiString error;
      if (!ReadFields(body, item, error))
         return Refusal(bridge, 400, error);

      String saveError;
      if (!PersistentGreyListingBypassEntry::SaveObject(item, saveError, PersistenceModeNormal))
         return SaveRefusal(bridge, saveError);

      LOG_APPLICATION("RestApi: Greylisting bypass entry " + item->GetPattern() + " created.");

      return bridge.respond(201, EntryJson(bridge, item), "");
   }

   HttpResponse Update(const Bridge &bridge, __int64 id, const AnsiString &requestBody)
   {
      JsonValue body;
      if (!ParseObjectBody(requestBody, body))
         return Refusal(bridge, 400, "the body must be a JSON object");

      std::shared_ptr<GreyListingBypassEntries> collection = AntiSpam().GetGreyListingBypassEntries();
      std::shared_ptr<GreyListingBypassEntry> item = collection->GetItemByDBID(id);
      if (!item)
         return Refusal(bridge, 404, "greylisting bypass entry not found");

      AnsiString error;
      if (!ReadFields(body, item, error))
         return Refusal(bridge, 400, error);

      String saveError;
      if (!PersistentGreyListingBypassEntry::SaveObject(item, saveError, PersistenceModeNormal))
         return SaveRefusal(bridge, saveError);

      LOG_APPLICATION("RestApi: Greylisting bypass entry " + item->GetPattern() + " updated.");

      return bridge.respond(200, EntryJson(bridge, item), "");
   }

   HttpResponse Delete(const Bridge &bridge, __int64 id)
   {
      std::shared_ptr<GreyListingBypassEntries> collection = AntiSpam().GetGreyListingBypassEntries();
      std::shared_ptr<GreyListingBypassEntry> item = collection->GetItemByDBID(id);
      if (!item)
         return Refusal(bridge, 404, "greylisting bypass entry not found");

      const String pattern = item->GetPattern();

      if (!collection->DeleteItemByDBID(id))
         return Refusal(bridge, 500, "the greylisting bypass entry could not be deleted; see the error log");

      LOG_APPLICATION("RestApi: Greylisting bypass entry " + pattern + " deleted.");

      return bridge.respond(200, "{\"deleted\":true}", "");
   }

   const char *Paths =
      ",\"/api/v1/greylisting-bypass\":{"
      "\"get\":{\"summary\":\"List the greylisting bypass entries\",\"description\":\"AntiSpam.GreyListingBypassEntries over COM: the envelope domains and HELO suffixes let past greylisting when the message carries the authentication results the entry requires. A privilege granted to a named domain, beside the server-wide bypass_greylisting_on_spf_success, which every SPF pass in the world enjoys - including a hijacked mailbox at a well-configured provider. Each entry: id, pattern (a domain matched exactly, or .suffix covering that name and every name under it), match (envelope, helo or both), require_spf, require_dkim, require_dmarc, note. Server-wide; refused for domain-restricted keys.\",\"responses\":{\"200\":{\"description\":\"Array of greylisting bypass entries\"}}},"
      "\"post\":{\"summary\":\"Add a greylisting bypass entry\",\"description\":\"Body: pattern (required; a domain such as example.com, or a suffix such as .mail.example.com), match (envelope, helo or both; default envelope), require_spf, require_dkim, require_dmarc (default false each; at least one must be true - an entry requiring nothing is refused, because a domain to be let through on no evidence belongs on the greylisting white list) and note. SPF is known at RCPT TO, so an entry requiring SPF alone decides there; DKIM and DMARC exist only after DATA, so an entry requiring either lets the recipient through for now and the message is greylisted after DATA if the result is not a pass - and only when DKIM verification or DMARC is switched on, since otherwise the entry can never be satisfied. A required result must belong to an identity this entry covers: DKIM counts only for a d= the pattern covers, DMARC only for a From domain it covers, and SPF only for the identity it was evaluated for - the envelope domain, or, where the entry matches on HELO, the HELO checked on its own (RFC 7208 section 2.3). Consulted for the next RCPT TO. Server-wide; refused for domain-restricted and read-only keys.\",\"requestBody\":{\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"required\":[\"pattern\"],\"properties\":{\"pattern\":{\"type\":\"string\"},\"match\":{\"type\":\"string\",\"enum\":[\"envelope\",\"helo\",\"both\"]},\"require_spf\":{\"type\":\"boolean\"},\"require_dkim\":{\"type\":\"boolean\"},\"require_dmarc\":{\"type\":\"boolean\"},\"note\":{\"type\":\"string\"}}}}}},\"responses\":{\"201\":{\"description\":\"Created: the entry as the listing shows it\"},\"400\":{\"description\":\"pattern empty or not a domain, no result required, a match word outside the three, a note over 255 characters, a field of the wrong type or an unknown field (error names it)\"}}}},"
      "\"/api/v1/greylisting-bypass/{id}\":{"
      "\"put\":{\"summary\":\"Change a greylisting bypass entry\",\"description\":\"Body: any subset of the fields POST takes; a field left out keeps its value. Nothing changes when the body is refused. Server-wide; refused for domain-restricted and read-only keys.\",\"requestBody\":{\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"properties\":{\"pattern\":{\"type\":\"string\"},\"match\":{\"type\":\"string\",\"enum\":[\"envelope\",\"helo\",\"both\"]},\"require_spf\":{\"type\":\"boolean\"},\"require_dkim\":{\"type\":\"boolean\"},\"require_dmarc\":{\"type\":\"boolean\"},\"note\":{\"type\":\"string\"}}}}}},\"responses\":{\"200\":{\"description\":\"The entry as saved\"},\"400\":{\"description\":\"A field refused (error names it); nothing changed\"},\"404\":{\"description\":\"Unknown id\"}}},"
      "\"delete\":{\"summary\":\"Remove a greylisting bypass entry\",\"responses\":{\"200\":{\"description\":\"Deleted\"},\"404\":{\"description\":\"Unknown id\"}}}}";
}

namespace HM
{
   AnsiString
   RestApiServer::OpenApiGreylistBypassPaths_()
   {
      return AnsiString(Paths);
   }

   HttpResponse
   RestApiServer::HandleGreylistBypass_(RouteKind kind, __int64 id, const AnsiString &requestBody)
   {
      Bridge bridge = { &RestApiServer::JsonEscape_, &RestApiServer::BuildResponse_ };

      switch (kind)
      {
      case RouteGreylistBypassList:
         return List(bridge);
      case RouteGreylistBypassCreate:
         return Create(bridge, requestBody);
      case RouteGreylistBypassUpdate:
         return Update(bridge, id, requestBody);
      case RouteGreylistBypassDelete:
         return Delete(bridge, id);
      default:
         break;
      }

      return BuildResponse_(404, "{\"error\":\"not found\"}");
   }
}
