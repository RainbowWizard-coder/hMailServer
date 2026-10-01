// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The journaling and dual-delivery rules as a resource. See RestApiServer.h and
// Common/Util/Journaling.h.
//
//    /api/v1/journal-rules        the rules, and a new one
//    /api/v1/journal-rules/{id}   one rule, changed or removed
//
// In the shape of RestApiBlockedAttachments.cpp: a listing and a create at the
// collection's path, an update and a delete at /{id}, one entry that every route
// answers with, and a body read whole before anything is applied, so that a wrong
// type, an unknown field or a value the owner refuses is a 400 naming it and
// nothing changes.
//
// The difference from that file is where the decisions are. A blocked attachment
// is an object of a live collection Configuration holds, and this file would be
// the only place that knew what a journal rule's words mean. It does not: the
// three small vocabularies, the write and the delete are Journaling's
// (Journaling::SaveRule, DeleteRule, RuleJson), so that this file, COM's three
// Utilities methods and the Control Panel page cannot come to disagree about what
// "outbound" or "copy" means. What is left here is HTTP: the body, the status
// codes, and the rule that a field left out of a PUT keeps its value.
//
// Server-wide, and refused to a credential confined to named domains even though
// a rule names one: a rule says that somebody's mail is copied to an address
// outside their sight, which is the server administrator's decision to make.

#include "StdAfx.h"
#include "RestApiServer.h"
#include "JsonDocument.h"
#include "Journaling.h"
#include "Unicode.h"

#include "../BO/JournalRule.h"
#include "../BO/JournalRules.h"

#include <string>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace
{
   using namespace HM;

   // The two private helpers of RestApiServer these functions need, handed in by
   // the member function that owns the right to name them.
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

   HttpResponse Refusal(const Bridge &bridge, int status, const String &sentence)
   {
      return bridge.respond(status, "{\"error\":\"" + bridge.escape(Utf8(sentence)) + "\"}", "");
   }

   bool ParseObjectBody(const AnsiString &requestBody, JsonValue &body)
   {
      std::string parseError;
      std::string text(requestBody.c_str(), (size_t) requestBody.GetLength());
      return JsonValue::Parse(text, body, parseError) && body.IsObject();
   }

   // The typed reads: untouched when the member is absent or null; false, with
   // error naming the member, when it is present with another type.
   bool ReadString(const JsonValue &object, const char *key, String &out, String &error)
   {
      const JsonValue *member = object.Get(key);
      if (!member || member->IsNull())
         return true;

      if (!member->IsString())
      {
         error = String(key) + " must be a string";
         return false;
      }

      out = Utf8ToString(member->AsString());
      return true;
   }

   bool ReadBool(const JsonValue &object, const char *key, bool &out, String &error)
   {
      const JsonValue *member = object.Get(key);
      if (!member || member->IsNull())
         return true;

      if (!member->IsBool())
      {
         error = String(key) + " must be true or false";
         return false;
      }

      out = member->AsBool();
      return true;
   }

   bool UnknownKey(const JsonValue &object, String &error)
   {
      static const char *const known[] =
         { "scope", "target", "journal_address", "direction", "mode", "enabled", "note" };

      for (const std::pair<std::string, JsonValue> &member : object.Members())
      {
         bool found = false;
         for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++)
         {
            if (member.first == known[i])
            {
               found = true;
               break;
            }
         }
         if (!found)
         {
            error = String("unknown field: ") + Utf8ToString(member.first.substr(0, 48));
            return true;
         }
      }
      return false;
   }

   // Everything a rule is, as words. Filled from the rule being changed - or from
   // a new rule's defaults - and then overlaid with whatever the body carries, so
   // that a field left out of a PUT keeps its value.
   struct Wanted
   {
      String scope;
      String target;
      String journal_address;
      String direction;
      String mode;
      String note;
      bool enabled = false;
   };

   Wanted FromRule(std::shared_ptr<JournalRule> rule)
   {
      Wanted wanted;
      wanted.scope = Journaling::ScopeName(rule->GetScope());
      wanted.target = rule->GetTarget();
      wanted.journal_address = rule->GetJournalAddress();
      wanted.direction = Journaling::DirectionName(rule->GetDirection());
      wanted.mode = Journaling::ModeName(rule->GetMode());
      wanted.note = rule->GetNote();
      wanted.enabled = rule->GetEnabled();
      return wanted;
   }

   bool ReadFields(const JsonValue &body, Wanted &wanted, String &error)
   {
      if (UnknownKey(body, error))
         return false;

      return ReadString(body, "scope", wanted.scope, error) &&
             ReadString(body, "target", wanted.target, error) &&
             ReadString(body, "journal_address", wanted.journal_address, error) &&
             ReadString(body, "direction", wanted.direction, error) &&
             ReadString(body, "mode", wanted.mode, error) &&
             ReadString(body, "note", wanted.note, error) &&
             ReadBool(body, "enabled", wanted.enabled, error);
   }

   HttpResponse Saved(const Bridge &bridge, int createdStatus, __int64 id, const Wanted &wanted)
   {
      __int64 savedId = 0;
      String error;

      if (!Journaling::SaveRule(id, wanted.scope, wanted.target, wanted.journal_address,
                                wanted.direction, wanted.mode, wanted.enabled, wanted.note, savedId, error))
         return Refusal(bridge, 400, error);

      std::shared_ptr<JournalRule> rule = Journaling::ReadRule(savedId);

      if (!rule)
         return Refusal(bridge, 500, "the journaling rule was saved but could not be read back; see the error log");

      LOG_APPLICATION("RestApi: Journaling rule for " + rule->GetTarget() + " to " + rule->GetJournalAddress() + " saved.");

      return bridge.respond(createdStatus, Journaling::RuleJson(rule), "");
   }

   HttpResponse List(const Bridge &bridge)
   {
      JournalRules all;

      if (!all.Refresh())
         return Refusal(bridge, 500, "the journaling rules could not be read; see the error log");

      // An array, as the other small collections of this API answer with; the
      // Control Panel's COM listing wraps the same entries in {"items":[...]}
      // because that is the shape its other JSON readers take. Both are built
      // from Journaling::RuleJson, so the entries themselves cannot differ.
      AnsiString json = "[";
      int count = 0;

      for (std::shared_ptr<JournalRule> rule : all.GetSnapshot())
      {
         if (!rule)
            continue;

         if (count > 0)
            json += ",";

         json += Journaling::RuleJson(rule);
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

      std::shared_ptr<JournalRule> blank(new JournalRule);
      Wanted wanted = FromRule(blank);

      String error;
      if (!ReadFields(body, wanted, error))
         return Refusal(bridge, 400, error);

      return Saved(bridge, 201, 0, wanted);
   }

   HttpResponse Update(const Bridge &bridge, __int64 id, const AnsiString &requestBody)
   {
      JsonValue body;
      if (!ParseObjectBody(requestBody, body))
         return Refusal(bridge, 400, "the body must be a JSON object");

      std::shared_ptr<JournalRule> rule = Journaling::ReadRule(id);
      if (!rule)
         return Refusal(bridge, 404, "journaling rule not found");

      Wanted wanted = FromRule(rule);

      String error;
      if (!ReadFields(body, wanted, error))
         return Refusal(bridge, 400, error);

      return Saved(bridge, 200, id, wanted);
   }

   HttpResponse Delete(const Bridge &bridge, __int64 id)
   {
      bool found = false;

      if (!Journaling::DeleteRule(id, found))
         return Refusal(bridge, 500, "the journaling rule could not be deleted; see the error log");

      if (!found)
         return Refusal(bridge, 404, "journaling rule not found");

      LOG_APPLICATION("RestApi: A journaling rule was deleted.");

      return bridge.respond(200, "{\"deleted\":true}", "");
   }

   const char *JournalRulePaths =
      ",\"/api/v1/journal-rules\":{"
      "\"get\":{\"summary\":\"List the journaling and dual-delivery rules\",\"description\":\"A copy of a domain's mail, or of one account's, sent to an address: Exchange journaling and Google dual delivery. Each entry: id, scope (domain or account), target (the domain name or the account address), journal_address, direction (inbound, outbound or both), mode (report or copy), enabled and note. With no rule in the table nothing is journaled, and the evaluation costs one comparison per accepted message. Server-wide; refused for domain-restricted keys.\",\"responses\":{\"200\":{\"description\":\"Array of journaling rules\"}}},"
      "\"post\":{\"summary\":\"Add a journaling rule\",\"description\":\"Body: scope (domain or account; domain by default), target (a domain this server hosts, or an account address in one), journal_address (where the copy goes - local or anywhere else), direction (inbound, outbound or both; both by default), mode (report, the Exchange envelope report with the original attached as message/rfc822, or copy, the message itself with one header added; report by default), enabled (true by default) and note. Inbound means a message with a local recipient the rule covers; outbound means a message from an authenticated sender it covers. In force for the next accepted message. Server-wide; refused for domain-restricted and read-only keys.\",\"requestBody\":{\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"required\":[\"target\",\"journal_address\"],\"properties\":{\"scope\":{\"type\":\"string\"},\"target\":{\"type\":\"string\"},\"journal_address\":{\"type\":\"string\"},\"direction\":{\"type\":\"string\"},\"mode\":{\"type\":\"string\"},\"enabled\":{\"type\":\"boolean\"},\"note\":{\"type\":\"string\"}}}}}},\"responses\":{\"201\":{\"description\":\"Created: the rule as the listing shows it\"},\"400\":{\"description\":\"A field of the wrong type or value, an unknown field, or a target this server does not host (error names it)\"}}}},"
      "\"/api/v1/journal-rules/{id}\":{"
      "\"put\":{\"summary\":\"Change a journaling rule\",\"description\":\"Body: any subset of the fields POST takes; a field left out keeps its value. Nothing changes when the body is refused. Server-wide; refused for domain-restricted and read-only keys.\",\"requestBody\":{\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"properties\":{\"scope\":{\"type\":\"string\"},\"target\":{\"type\":\"string\"},\"journal_address\":{\"type\":\"string\"},\"direction\":{\"type\":\"string\"},\"mode\":{\"type\":\"string\"},\"enabled\":{\"type\":\"boolean\"},\"note\":{\"type\":\"string\"}}}}}},\"responses\":{\"200\":{\"description\":\"The rule as saved\"},\"400\":{\"description\":\"A field refused (error names it); nothing changed\"},\"404\":{\"description\":\"Unknown id\"}}},"
      "\"delete\":{\"summary\":\"Remove a journaling rule\",\"description\":\"The rule stops copying at once. What it already sent is wherever it was sent: this server keeps no second copy and withdraws nothing.\",\"responses\":{\"200\":{\"description\":\"Deleted\"},\"404\":{\"description\":\"Unknown id\"}}}}";
}

namespace HM
{
   AnsiString
   RestApiServer::OpenApiJournalRulePaths_()
   {
      return AnsiString(JournalRulePaths);
   }

   HttpResponse
   RestApiServer::HandleJournalRules_(RouteKind kind, __int64 id, const AnsiString &requestBody)
   {
      Bridge bridge = { &RestApiServer::JsonEscape_, &RestApiServer::BuildResponse_ };

      switch (kind)
      {
      case RouteJournalRuleList:
         return List(bridge);
      case RouteJournalRuleCreate:
         return Create(bridge, requestBody);
      case RouteJournalRuleUpdate:
         return Update(bridge, id, requestBody);
      case RouteJournalRuleDelete:
         return Delete(bridge, id);
      default:
         break;
      }

      return BuildResponse_(404, "{\"error\":\"not found\"}");
   }
}
