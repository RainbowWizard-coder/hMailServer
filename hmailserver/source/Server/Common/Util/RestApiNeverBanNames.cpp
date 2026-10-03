// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The never-ban list by reverse name (GitLab work item #56) as a resource. See
// RestApiServer.h and Common/Util/NeverBanNameList.h.
//
//    /api/v1/never-ban-names         GET lists, POST adds
//    /api/v1/never-ban-names/{id}    PUT changes, DELETE removes
//    /api/v1/never-ban-names/check   POST: what the list would decide about an address
//
// In the shape of the domain white and black list (RestApiDomainPolicyList.cpp):
// one entry every route answers with, and a body read whole before anything is
// applied, so that a wrong type, an unknown field or a pattern the row refuses
// is a 400 naming it and nothing changes. Each write reaches the same
// PersistentNeverBanName the COM members reach, which tells NeverBanNameList,
// so a row is in force for the next ban.

#include "StdAfx.h"
#include "RestApiServer.h"
#include "JsonDocument.h"
#include "HttpsClient.h"
#include "NeverBanNameList.h"
#include "Unicode.h"
#include "../TCPIP/IPAddress.h"
#include "../Util/Time.h"
#include "../BO/NeverBanName.h"
#include "../BO/NeverBanNames.h"
#include "../Persistence/PersistentNeverBanName.h"
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

   AnsiString EntryJson(const Bridge &bridge, const std::shared_ptr<NeverBanName> &item)
   {
      AnsiString entry;
      entry.Format("{\"id\":%I64d,\"pattern\":\"%hs\",\"note\":\"%hs\",\"created\":\"%hs\"}",
         item->GetID(),
         Quoted(bridge, item->GetPattern()).c_str(),
         Quoted(bridge, item->GetNote()).c_str(),
         Quoted(bridge, Time::GetTimeStampFromDateTime(item->GetCreated())).c_str());
      return entry;
   }

   // The body onto the item: everything read and checked first, applied last,
   // so a refusal leaves the item as it was.
   bool ReadFields(const JsonValue &body, std::shared_ptr<NeverBanName> item, bool creating, AnsiString &error, String &refusal)
   {
      static const char *const keys[] = { "pattern", "note" };
      if (UnknownKey(body, keys, sizeof(keys) / sizeof(keys[0]), error))
         return false;

      String pattern = item->GetPattern();
      String note = item->GetNote();

      if (!ReadString(body, "pattern", pattern, error) ||
          !ReadString(body, "note", note, error))
         return false;

      if (creating && !body.Get("pattern"))
      {
         error = "pattern is required";
         return false;
      }

      if (!NeverBanName::ValidatePattern(pattern, refusal))
         return false;

      note.Trim();
      if (note.GetLength() > 255)
      {
         error = "note is at most 255 characters";
         return false;
      }

      item->SetPattern(pattern);
      item->SetNote(note);
      return true;
   }

   HttpResponse List(const Bridge &bridge)
   {
      NeverBanNames collection;
      if (!collection.Refresh())
         return Refusal(bridge, 503, "the never-ban list could not be read; see the error log");

      AnsiString json = "[";
      int count = 0;
      for (std::shared_ptr<NeverBanName> item : collection.GetSnapshot())
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

      std::shared_ptr<NeverBanName> item(new NeverBanName);
      AnsiString error;
      String refusal;
      if (!ReadFields(body, item, true, error, refusal))
         return refusal.IsEmpty() ? Refusal(bridge, 400, error) : RefusalText(bridge, 400, refusal);

      String saveError;
      if (!PersistentNeverBanName::SaveObject(item, saveError, PersistenceModeNormal))
      {
         if (saveError.IsEmpty())
            return Refusal(bridge, 500, "the never-ban list entry could not be saved; see the error log");
         return RefusalText(bridge, 400, saveError);
      }

      LOG_APPLICATION("RestApi: Never-ban list entry " + item->GetPattern() + " created.");

      return bridge.respond(201, EntryJson(bridge, item), "");
   }

   HttpResponse Update(const Bridge &bridge, __int64 id, const AnsiString &requestBody)
   {
      JsonValue body;
      if (!ParseObjectBody(requestBody, body))
         return Refusal(bridge, 400, "the body must be a JSON object");

      NeverBanNames collection;
      if (!collection.Refresh())
         return Refusal(bridge, 503, "the never-ban list could not be read; see the error log");

      std::shared_ptr<NeverBanName> item = collection.GetItemByDBID(id);
      if (!item)
         return Refusal(bridge, 404, "never-ban list entry not found");

      AnsiString error;
      String refusal;
      if (!ReadFields(body, item, false, error, refusal))
         return refusal.IsEmpty() ? Refusal(bridge, 400, error) : RefusalText(bridge, 400, refusal);

      String saveError;
      if (!PersistentNeverBanName::SaveObject(item, saveError, PersistenceModeNormal))
      {
         if (saveError.IsEmpty())
            return Refusal(bridge, 500, "the never-ban list entry could not be saved; see the error log");
         return RefusalText(bridge, 400, saveError);
      }

      LOG_APPLICATION("RestApi: Never-ban list entry " + item->GetPattern() + " updated.");

      return bridge.respond(200, EntryJson(bridge, item), "");
   }

   HttpResponse Delete(const Bridge &bridge, __int64 id)
   {
      NeverBanNames collection;
      if (!collection.Refresh())
         return Refusal(bridge, 503, "the never-ban list could not be read; see the error log");

      std::shared_ptr<NeverBanName> item = collection.GetItemByDBID(id);
      if (!item)
         return Refusal(bridge, 404, "never-ban list entry not found");

      const String pattern = item->GetPattern();

      if (!PersistentNeverBanName::DeleteObject(item))
         return Refusal(bridge, 500, "the never-ban list entry could not be deleted; see the error log");

      LOG_APPLICATION("RestApi: Never-ban list entry " + pattern + " deleted.");

      return bridge.respond(200, "{\"deleted\":true}", "");
   }

   HttpResponse Check(const Bridge &bridge, const AnsiString &requestBody, bool administrator)
   {
      JsonValue body;
      if (!ParseObjectBody(requestBody, body))
         return Refusal(bridge, 400, "the body must be a JSON object");

      static const char *const keys[] = { "address" };
      AnsiString error;
      if (UnknownKey(body, keys, sizeof(keys) / sizeof(keys[0]), error))
         return Refusal(bridge, 400, error);

      String address;
      if (!ReadString(body, "address", address, error))
         return Refusal(bridge, 400, error);

      if (!body.Get("address"))
         return Refusal(bridge, 400, "address is required");

      // An address that is not public - private, loopback, link-local, carrier
      // NAT, documentation - is checked for the administrator password alone.
      // Its reverse names, and whether they resolve back, are this server's
      // view of its own network, which split-horizon DNS answers with the names
      // of the hosts inside it; an API key and an account holding a role check a
      // public address, as the breach check's endpoint test reaches one (Roles.md,
      // What manage_settings reaches). Until the second security review of 6.3.6
      // they checked any address. Text that is not an address is left to the
      // check's own refusal below.
      if (!administrator)
      {
         String text = address;
         text.Trim();
         IPAddress parsed;
         if (!text.IsEmpty() && parsed.TryParse(AnsiString(text), false) && !HttpsClient::AddressIsPublic(AnsiString(text)))
            return Refusal(bridge, 403, "an address that is not public - on this server's own network, loopback, link-local or reserved - is checked with the administrator password alone; an API key and an account holding a role check a public address");
      }

      AnsiString document;
      String refusal;
      if (!NeverBanNameList::Check(address, document, refusal))
         return RefusalText(bridge, 400, refusal);

      return bridge.respond(200, document, "");
   }

   const char *NeverBanNamePaths =
      ",\"/api/v1/never-ban-names\":{"
      "\"get\":{\"summary\":\"List the never-ban list by reverse name\",\"description\":\"Providers this server never auto-bans, named by the reverse names of their hosts (Settings.AutoBanNeverBanNames over COM; GitLab work item #56). Each entry: id, pattern (mail.example.net for that name alone, .example.net for the name and every name beneath it, at a label boundary), note and created. An address is spared - no IP range, no firewall rule, no line for fail2ban - when one of its reverse names is covered by a pattern AND resolves back to the address (A for IPv4, AAAA for IPv6), since anybody who holds an address can write its reverse name. Consulted beside AutoBanNeverBan by every auto-ban: failed logons, spam traps and the multi-recipient null sender. Never looked up on a session's thread: the SMTP session's own reverse lookup is used where it has one, and otherwise the ban is written, resolved on a queue of its own, every lookup given up after five seconds, and lifted if the address is spared, before anything below the server is told; until then no firewall reconciliation mirrors it. Skips nothing else: greylisting, SPF, DKIM, DMARC and the domain list still apply. Empty by default. Server-wide; refused for domain-restricted keys.\",\"responses\":{\"200\":{\"description\":\"Array of never-ban list entries\"}}},"
      "\"post\":{\"summary\":\"Add a name to the never-ban list\",\"description\":\"Body: pattern (required; a host name, or .domain for the domain and every name beneath it) and note. Refused: an address or a network (those belong in AutoBanNeverBan), a * wildcard (write .example.net), and a public suffix such as .com, .co.uk or .github.io, under which anybody can register a name. The pattern is unique. In force for the next ban. Server-wide; refused for domain-restricted and read-only keys.\",\"requestBody\":{\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"required\":[\"pattern\"],\"properties\":{\"pattern\":{\"type\":\"string\"},\"note\":{\"type\":\"string\"}}}}}},\"responses\":{\"201\":{\"description\":\"Created: the entry as the listing shows it\"},\"400\":{\"description\":\"pattern missing or not a name the list may hold, a pattern already listed, a field of the wrong type or an unknown field (error names it)\"}}}},"
      "\"/api/v1/never-ban-names/{id}\":{"
      "\"put\":{\"summary\":\"Change a never-ban list entry\",\"description\":\"Body: any subset of the fields POST takes; a field left out keeps its value. Nothing changes when the body is refused. Server-wide; refused for domain-restricted and read-only keys.\",\"requestBody\":{\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"properties\":{\"pattern\":{\"type\":\"string\"},\"note\":{\"type\":\"string\"}}}}}},\"responses\":{\"200\":{\"description\":\"The entry as saved\"},\"400\":{\"description\":\"A field refused (error names it); nothing changed\"},\"404\":{\"description\":\"Unknown id\"}}},"
      "\"delete\":{\"summary\":\"Remove a never-ban list entry\",\"description\":\"The pattern spares nobody from the next ban on.\",\"responses\":{\"200\":{\"description\":\"Deleted\"},\"404\":{\"description\":\"Unknown id\"}}}},"
      "\"/api/v1/never-ban-names/check\":{"
      "\"post\":{\"summary\":\"What the never-ban list would decide about an address\",\"description\":\"Body: address (required; IPv4 or IPv6). The server looks up the address's reverse names now and answers {address, reverse_names: [{name, usable, pattern, resolves_back}], answered, spared, name, pattern, reason}: usable is false for an address written as a name or a public suffix, pattern is the row covering the name or null, and resolves_back says whether the name's forward lookup gives the address (null when it was not asked or not answered) - asked whether or not a row covers the name, so that it shows whether a row WOULD work. Nothing is kept and nothing is banned or lifted. Sends DNS queries, so it is refused for read-only keys. An address that is not public - private, loopback, link-local, carrier NAT, reserved or documentation - is checked for the administrator password alone, since its reverse names are the server's view of its own network: an API key and an account holding a role get 403 for one (since 6.3.6; before, they checked any address). Server-wide; refused for domain-restricted keys.\",\"requestBody\":{\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"required\":[\"address\"],\"properties\":{\"address\":{\"type\":\"string\"}}}}}},\"responses\":{\"200\":{\"description\":\"The verdict and every reverse name\"},\"400\":{\"description\":\"address missing or not an address, or an unknown field\"},\"403\":{\"description\":\"An address that is not public, asked by anybody but the administrator password\"}}}}";
}

namespace HM
{
   AnsiString
   RestApiServer::OpenApiNeverBanNamePaths_()
   {
      return AnsiString(NeverBanNamePaths);
   }

   HttpResponse
   RestApiServer::HandleNeverBanNames_(RouteKind kind, __int64 id, const AnsiString &requestBody, bool administrator)
   {
      Bridge bridge = { &RestApiServer::JsonEscape_, &RestApiServer::BuildResponse_ };

      switch (kind)
      {
      case RouteNeverBanNameList:
         return List(bridge);
      case RouteNeverBanNameCreate:
         return Create(bridge, requestBody);
      case RouteNeverBanNameUpdate:
         return Update(bridge, id, requestBody);
      case RouteNeverBanNameDelete:
         return Delete(bridge, id);
      case RouteNeverBanNameCheck:
         return Check(bridge, requestBody, administrator);
      default:
         break;
      }

      return BuildResponse_(404, "{\"error\":\"not found\"}");
   }
}
