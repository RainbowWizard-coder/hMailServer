// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The audit trail and the alerts, over REST.
//
//   GET /api/v1/audit                who changed what, newest first, with
//                                    paging and filters.
//   GET /api/v1/audit/verify         walks the hash chain and names the first
//                                    row that does not check out.
//   POST /api/v1/audit/<id>/revert   puts a setting back to the value the row
//                                    says it had (AuditRevert.h), which is
//                                    itself recorded as a change.
//   GET /api/v1/alerts/rules         the conditions, their thresholds and what
//                                    each one does.
//   PUT /api/v1/alerts/rules/<name>  change one, or create one for a condition
//                                    nothing has shipped.
//   GET /api/v1/alerts/events        what has fired, newest first.
//   POST /api/v1/alerts/test         raise a condition on request.
//   POST /api/v1/alerts/run          run one alerting pass now.
//
// No API key reaches any of them, whatever its scope or role: see
// IsApiKeyRoute_ in RestApiServer.cpp for why a key is refused, and refused
// with a 401. They are not the administrator password's alone, though. An
// account that holds a role goes on to the permission decision like any other
// route - view_audit for the trail, which is what the auditor role is for;
// view_configuration to read the alert rules and events; manage_settings to
// change a rule or raise a test; manage_server to run a pass - and a role
// confined to named domains is refused all seven as server-wide.
//
// A webhook secret is never returned. It can be set and it can be cleared, and
// the answer says whether one is configured - which is what an administrator
// needs to know and what a reader of the page must not be able to take away.

#include "StdAfx.h"

#include "RestApiServer.h"

#include "AuditRevert.h"
#include "AuditTrail.h"
#include "../Application/IniSettingStore.h"
#include "AlertManager.h"
#include "JsonDocument.h"
#include "Unicode.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // Every text field as UTF-8. AnsiString(String) is no encoding of the text:
      // on Windows it wrote NUL bytes from the first character above U+00FF to
      // the end of the field, and on Linux a '?' for each, so a row naming a
      // Cyrillic account, an IDN address or a setting's value outside Latin-1
      // was listed as something else than the row the chain verifies. Through
      // JsonEscapeText, so that a field holding its UTF-8 widened byte by byte -
      // an identity taken from a REST Basic credential, an address read back
      // from an AnsiString member - is written as the text, as it was before.
      AnsiString AuditEntryJson(const AuditTrail::Entry &entry)
      {
         AnsiString (*escape)(const String &) = &RestApiServer::JsonEscapeText;

         AnsiString json;
         AnsiString numbers;

         numbers.Format("{\"id\":%I64d,\"time\":%I64d,", entry.id, entry.time);
         json += numbers;

         json += "\"actor\":\"" + escape(entry.actor) + "\",";
         json += "\"actor_type\":\"" + escape(entry.actor_kind) + "\",";
         json += "\"interface\":\"" + escape(entry.interface_name) + "\",";
         json += "\"address\":\"" + escape(entry.address) + "\",";
         json += "\"object_type\":\"" + escape(entry.object_type) + "\",";
         json += "\"object\":\"" + escape(entry.object_name) + "\",";
         json += "\"action\":\"" + escape(entry.action) + "\",";
         json += "\"detail\":\"" + escape(entry.detail) + "\",";
         json += "\"hash\":\"" + escape(entry.hash) + "\",";
         json += "\"previous_hash\":\"" + escape(entry.previous_hash) + "\"";
         json += "}";

         return json;
      }

      String QueryText(const AnsiString &bytes)
      {
         String text;
         if (!Unicode::MultiByteToWide(bytes, text))
            text = String(bytes);
         return text;
      }

      // The text of an alert's rule and events: in from the JSON body as UTF-8,
      // out to it as UTF-8. It went in widened byte by byte and out through
      // AnsiString(String), which gave the same bytes back for text that came in
      // over REST and so looked right, while the rule was stored as something
      // else than its text and a condition's length was counted in bytes; and
      // an alert the server raised about text outside Latin-1 was listed with
      // NUL bytes on Windows and '?' on Linux.
      bool JsonText(const std::string &utf8, String &text)
      {
         return Unicode::MultiByteToWide(AnsiString(utf8.data(), utf8.size()), text);
      }

      AnsiString AlertRuleJson(const AlertManager::Rule &rule)
      {
         AnsiString (*escape)(const String &) = &RestApiServer::JsonEscapeText;

         AnsiString json;

         json += "{\"condition\":\"" + escape(rule.condition) + "\",";
         json += AnsiString("\"enabled\":") + (rule.enabled ? "true" : "false") + ",";

         AnsiString numbers;
         numbers.Format("\"threshold\":%d,\"actions\":%d,\"cooldown_minutes\":%d,", rule.threshold, rule.actions, rule.cooldown);
         json += numbers;

         json += AnsiString("\"digest\":") + (rule.digest ? "true" : "false") + ",";
         json += "\"address\":\"" + escape(rule.address) + "\",";
         json += "\"webhook\":\"" + escape(rule.webhook) + "\",";

         // The secret itself, never. Whether there is one, always: an
         // administrator asking "is this webhook signed" gets an answer, and a
         // reader of the page gets nothing they could sign with.
         json += AnsiString("\"webhook_secret_set\":") + (rule.secret.IsEmpty() ? "false" : "true");
         json += "}";

         return json;
      }

      AnsiString AlertEventJson(const AlertManager::Event &event)
      {
         AnsiString (*escape)(const String &) = &RestApiServer::JsonEscapeText;

         const char *hookState = "none";
         switch (event.hook_state)
         {
         case AlertManager::HookPending:      hookState = "pending"; break;
         case AlertManager::HookDelivered:    hookState = "delivered"; break;
         case AlertManager::HookDeadLettered: hookState = "dead-lettered"; break;
         default: break;
         }

         AnsiString json;
         AnsiString numbers;

         numbers.Format("{\"id\":%I64d,\"time\":%I64d,\"severity\":%d,", event.id, event.time, event.severity);
         json += numbers;

         json += "\"condition\":\"" + escape(event.condition) + "\",";
         json += AnsiString("\"state\":\"") + (event.state == AlertManager::StateCleared ? "cleared" : "raised") + "\",";
         json += "\"summary\":\"" + escape(event.summary) + "\",";
         json += "\"detail\":\"" + escape(event.detail) + "\",";
         json += AnsiString("\"notified\":") + (event.notified ? "true" : "false") + ",";
         json += AnsiString("\"held_for_digest\":") + (event.digested ? "true" : "false") + ",";
         json += AnsiString("\"webhook\":\"") + hookState + "\",";

         AnsiString tries;
         tries.Format("\"webhook_attempts\":%d", event.hook_tries);
         json += tries;

         json += "}";

         return json;
      }
   }

   AnsiString
   RestApiServer::JsonEscapeText(const String &value)
   {
      return JsonEscape_(Unicode::ToUtf8(value));
   }

   HttpResponse
   RestApiServer::HandleAudit_(const Route &route, const AnsiString &requestBody, bool administrator)
   {
      AnsiString (*escape)(const AnsiString &) = &RestApiServer::JsonEscape_;

      switch (route.kind)
      {
      case RouteAuditList:
         {
            AuditTrail::Filter filter;
            // The query's percent-decoded bytes are UTF-8: widened byte by
            // byte, an actor or an object type outside ASCII matched nothing.
            // Bytes that are not UTF-8 are widened as they were, so that such a
            // filter still narrows the list - to nothing - rather than being
            // dropped and listing every row.
            filter.actor = QueryText(QueryParameter_(route.query, "actor"));
            filter.object_type = QueryText(QueryParameter_(route.query, "object_type"));
            filter.action = QueryText(QueryParameter_(route.query, "action"));

            AnsiString since = QueryParameter_(route.query, "since");
            if (!since.IsEmpty())
               filter.since = _atoi64(since.c_str());

            AnsiString until = QueryParameter_(route.query, "until");
            if (!until.IsEmpty())
               filter.until = _atoi64(until.c_str());

            AnsiString limit = QueryParameter_(route.query, "limit");
            if (!limit.IsEmpty())
               filter.limit = atoi(limit.c_str());

            AnsiString offset = QueryParameter_(route.query, "offset");
            if (!offset.IsEmpty())
               filter.offset = atoi(offset.c_str());

            std::vector<AuditTrail::Entry> entries;
            int total = 0;

            if (!AuditTrail::Instance()->Query(filter, entries, total))
               return BuildResponse_(500, "{\"error\":\"the audit trail could not be read\"}");

            AnsiString body;
            AnsiString head;
            head.Format("{\"total\":%d,\"offset\":%d,\"entries\":[", total, filter.offset < 0 ? 0 : filter.offset);
            body += head;

            for (size_t i = 0; i < entries.size(); i++)
            {
               if (i > 0)
                  body += ",";

               body += AuditEntryJson(entries[i]);
            }

            body += "]}";

            return BuildResponse_(200, body);
         }

      case RouteAuditVerify:
         {
            __int64 firstBroken = 0;
            __int64 rowsChecked = 0;
            String reason;

            bool intact = AuditTrail::Instance()->Verify(firstBroken, rowsChecked, reason);

            AnsiString body;
            body.Format("{\"intact\":%hs,\"rows_checked\":%I64d,\"first_broken_id\":%I64d,\"reason\":\"%hs\"}",
               intact ? "true" : "false", rowsChecked, firstBroken, escape(Utf8_(reason)).c_str());

            return BuildResponse_(200, body);
         }

      case RouteAuditRevert:
         {
            AuditTrail::Entry entry;
            if (!AuditTrail::Instance()->Read(route.record_id, entry))
               return BuildResponse_(404, "{\"error\":\"no audit row has that id\"}");

            // A key of the [Settings] section - AutoBanCommand, the addresses
            // trusted to name a client, the live update's checks - answers to the
            // administrator password and to nothing else, here as at
            // /api/v1/settings/ini: an account holding a role with
            // manage_settings reaches this route for the database's settings,
            // and until 6.3.6 it could put a [Settings] key back to an earlier
            // value through it. The same sentence the ini routes refuse a role
            // with; refused before anything is read or written.
            String spelling;
            if (!administrator && IniSettingStore::IsSettingTheServerReads(entry.object_name, spelling))
               return BuildResponse_(403, "{\"error\":\"the api keys and the [Settings] section of hMailServer.ini, including the list of what this server has changed from stock, answer to the administrator password only\"}");

            String restored;
            String reason;
            if (!AuditRevert::PutBack(entry, restored, reason))
               return BuildResponse_(409, "{\"error\":\"" + escape(Utf8_(reason)) + "\"}");

            LOG_APPLICATION(String(_T("The setting ")) + entry.object_name + _T(" was put back to the value audit row ") + StringParser::IntToString(entry.id) + _T(" records, over the REST API."));

            AnsiString body;
            body.Format("{\"reverted\":true,\"id\":%I64d,\"setting\":\"", entry.id);
            return BuildResponse_(200, body + escape(Utf8_(entry.object_name)) + "\",\"value\":\"" + escape(Utf8_(restored)) + "\"}");
         }

      case RouteAlertRuleList:
         {
            std::vector<AlertManager::Rule> rules;

            if (!AlertManager::Instance()->GetRules(rules))
               return BuildResponse_(500, "{\"error\":\"the alert rules could not be read\"}");

            AnsiString body = "{\"rules\":[";

            for (size_t i = 0; i < rules.size(); i++)
            {
               if (i > 0)
                  body += ",";

               body += AlertRuleJson(rules[i]);
            }

            body += "]}";

            return BuildResponse_(200, body);
         }

      case RouteAlertRuleUpdate:
         {
            // Conditions are stored lower-case; see AlertManager::SaveRule. The
            // path's percent-decoded bytes are UTF-8.
            String condition;
            if (!Unicode::MultiByteToWide(route.identifier, condition))
               return BuildResponse_(400, "{\"error\":\"the condition in the path is not UTF-8\"}");
            condition.MakeLower();

            AlertManager::Rule rule;
            bool exists = AlertManager::Instance()->GetRule(condition, rule);

            rule.condition = condition;

            JsonValue document;
            std::string parseError;
            if (!JsonValue::Parse(std::string(requestBody.c_str(), requestBody.GetLength()), document, parseError) || !document.IsObject())
               return BuildResponse_(400, "{\"error\":\"the body must be a JSON object\"}");

            // A field the body leaves out keeps the value it has, so a client
            // that wants to switch one condition on does not have to send back
            // every other field of the rule - and cannot blank a webhook secret
            // by forgetting it.
            const JsonValue *member = nullptr;

            member = document.Get("enabled");
            if (member != nullptr)
               rule.enabled = member->AsBool();

            member = document.Get("digest");
            if (member != nullptr)
               rule.digest = member->AsBool();

            member = document.Get("threshold");
            if (member != nullptr)
               rule.threshold = member->AsInt();

            member = document.Get("actions");
            if (member != nullptr)
               rule.actions = member->AsInt();

            member = document.Get("cooldown_minutes");
            if (member != nullptr)
               rule.cooldown = member->AsInt();

            member = document.Get("address");
            if (member != nullptr && !JsonText(member->AsString(), rule.address))
               return BuildResponse_(400, "{\"error\":\"address is not UTF-8\"}");

            member = document.Get("webhook");
            if (member != nullptr && !JsonText(member->AsString(), rule.webhook))
               return BuildResponse_(400, "{\"error\":\"webhook is not UTF-8\"}");

            // Write-only, as a secret should be: it can be set, it can be
            // cleared with "", and it is never handed back. The webhook is
            // signed with its UTF-8 (AlertManager::DeliverWebhook_), which is
            // what a receiver holding the same text computes.
            member = document.Get("webhook_secret");
            if (member != nullptr && !JsonText(member->AsString(), rule.secret))
               return BuildResponse_(400, "{\"error\":\"webhook_secret is not UTF-8\"}");

            String refusal;
            if (!AlertManager::Instance()->SaveRule(rule, refusal))
            {
               AnsiString error;
               error.Format("{\"error\":\"%hs\"}", escape(Utf8_(refusal)).c_str());
               return BuildResponse_(400, error);
            }

            LOG_APPLICATION("RestApi: Alert rule '" + condition + "' " + String(exists ? _T("changed") : _T("created")) + ".");

            AlertManager::Rule saved;
            AlertManager::Instance()->GetRule(condition, saved);

            return BuildResponse_(exists ? 200 : 201, AlertRuleJson(saved));
         }

      case RouteAlertEventList:
         {
            int limit = 100;

            AnsiString limitText = QueryParameter_(route.query, "limit");
            if (!limitText.IsEmpty())
               limit = atoi(limitText.c_str());

            std::vector<AlertManager::Event> events;

            if (!AlertManager::Instance()->GetEvents(limit, events))
               return BuildResponse_(500, "{\"error\":\"the alert events could not be read\"}");

            AnsiString body = "{\"events\":[";

            for (size_t i = 0; i < events.size(); i++)
            {
               if (i > 0)
                  body += ",";

               body += AlertEventJson(events[i]);
            }

            body += "]}";

            return BuildResponse_(200, body);
         }

      case RouteAlertTest:
         {
            JsonValue document;
            std::string parseError;
            if (!JsonValue::Parse(std::string(requestBody.c_str(), requestBody.GetLength()), document, parseError) || !document.IsObject())
               return BuildResponse_(400, "{\"error\":\"the body must be a JSON object\"}");

            String condition;
            if (!JsonText(document.GetString("condition"), condition))
               return BuildResponse_(400, "{\"error\":\"the condition is not UTF-8\"}");
            condition.Trim();
            condition.MakeLower();

            if (condition.IsEmpty())
               return BuildResponse_(400, "{\"error\":\"name the condition to raise\"}");

            String summary;
            if (!JsonText(document.GetString("summary", "Raised on request, to prove that alerting reaches somebody."), summary))
               return BuildResponse_(400, "{\"error\":\"the summary is not UTF-8\"}");

            // Through exactly the path a real condition takes - the same rule,
            // the same cool-down, the same ceiling - so that what arrives is
            // what would arrive. A condition with no rule, or a rule that is
            // off, raises nothing and says so rather than pretending.
            bool raised = document.GetBool("occurrence")
               ? AlertManager::Instance()->RaiseOccurrence(condition, ErrorManager::Low, summary, _T("Raised through POST /api/v1/alerts/test."))
               : AlertManager::Instance()->Raise(condition, ErrorManager::Low, summary, _T("Raised through POST /api/v1/alerts/test."));

            AnsiString body;
            body.Format("{\"raised\":%hs}", raised ? "true" : "false");

            return BuildResponse_(200, body);
         }

      case RouteAlertRun:
         {
            // One pass of AlertTask, now. The scheduled one runs every minute;
            // this is for the administrator who has just configured a recipient
            // or a webhook and wants to know before the minute is out.
            int evaluated = AlertManager::Instance()->EvaluateScheduledConditions();
            int notified = AlertManager::Instance()->SendPendingNotifications();
            int delivered = AlertManager::Instance()->RunWebhookDeliveries();
            int digested = AlertManager::Instance()->SendDigestIfDue();

            AnsiString body;
            body.Format("{\"conditions_raised\":%d,\"notifications_sent\":%d,\"webhooks_delivered\":%d,\"digest_events\":%d}",
               evaluated, notified, delivered, digested);

            return BuildResponse_(200, body);
         }

      default:
         break;
      }

      return BuildResponse_(404, "{\"error\":\"not found\"}");
   }

   AnsiString
   RestApiServer::OpenApiAuditPaths_()
   {
      static const char *paths =
         ",\"/api/v1/audit\":{\"get\":{\"summary\":\"The audit trail\","
         "\"description\":\"Every administrative change, newest first, with the administrator or key that made it, the interface it came over, the address it came from, and the hash chain each row carries. Refused to every API key. The administrator password, or an account whose role holds view_audit and reaches every domain.\","
         "\"parameters\":["
         "{\"name\":\"actor\",\"in\":\"query\",\"schema\":{\"type\":\"string\"},\"description\":\"Substring of the actor, so key: finds every API key.\"},"
         "{\"name\":\"object_type\",\"in\":\"query\",\"schema\":{\"type\":\"string\"},\"description\":\"domain, account, setting, rule and so on.\"},"
         "{\"name\":\"action\",\"in\":\"query\",\"schema\":{\"type\":\"string\"},\"description\":\"created, updated or deleted; unlocked for an account let back in; cleared for the logon-failure list.\"},"
         "{\"name\":\"since\",\"in\":\"query\",\"schema\":{\"type\":\"integer\"},\"description\":\"Seconds since the epoch, inclusive.\"},"
         "{\"name\":\"until\",\"in\":\"query\",\"schema\":{\"type\":\"integer\"},\"description\":\"Seconds since the epoch, inclusive.\"},"
         "{\"name\":\"limit\",\"in\":\"query\",\"schema\":{\"type\":\"integer\"},\"description\":\"Rows to return, 1 to 1000. Default 100.\"},"
         "{\"name\":\"offset\",\"in\":\"query\",\"schema\":{\"type\":\"integer\"},\"description\":\"Rows to skip.\"}"
         "],\"responses\":{\"200\":{\"description\":\"The page of entries and the total that match.\"}}}}"
         ",\"/api/v1/audit/{id}/revert\":{\"post\":{\"summary\":\"Put a setting back to the value an audit row says it had\","
         "\"description\":\"The row must record a setting's change - name: old -> new in its detail - and the old value is written into the column the server reads the setting from, through the setter that writes it - a switch (true -> false) as a switch, a number as a number, a text as a text - so the put-back is itself a row in the trail made by whoever asked. Refused with 409 for a row that is not a setting's, for a setting's row whose detail does not hold the two values, for a secret (the trail never holds its values), for a value the trail cut at 120 characters, for a row whose values hold the arrow themselves unless it is the setting's latest change in the trail and exactly one place to divide it leaves the value the setting has now as the new side (a [Settings] key's such row always), for a number's row that holds no number, for a [Settings] value the store cannot hold, and for a setting this server no longer has; and, with the sentence every other way of changing it is refused with, for one of the five [Settings] keys of the live update's trust (UpdateSigningIdentity, UpdateSigningIssuer, UpdateSourceRepository, UpdateTrustRootsFile, UpdateLogPublicKeyFile), which only hMailServer.INI itself changes, and for a setting this server's environment sets (hmailserver --apply-environment), each unless the value put back is the one the setting has already. A write that fails is answered with 409 and says so. A [Settings] key whose old side is (default) is returned to its default. Needs manage_settings; refused to every API key; and a [Settings] key's row is put back by the administrator password alone, as /api/v1/settings/ini is written - an account holding a role is answered 403 for one, before anything is read or written (since 6.3.6).\","
         "\"responses\":{\"200\":{\"description\":\"reverted, id, setting and value\"},\"403\":{\"description\":\"A [Settings] key's row, asked by an account holding a role rather than the administrator password\"},\"404\":{\"description\":\"No such row\"},\"409\":{\"description\":\"The row cannot be put back, with the reason\"}}}}"
         ",\"/api/v1/audit/verify\":{\"get\":{\"summary\":\"Verify the audit chain\","
         "\"description\":\"Walks the hash chain from the first row and reports the first row whose stored hash is not the hash its contents produce, or which does not carry the hash of the row before it. Refused to every API key. The administrator password, or an account whose role holds view_audit and reaches every domain.\","
         "\"responses\":{\"200\":{\"description\":\"intact, rows_checked, first_broken_id and the reason.\"}}}}"
         ",\"/api/v1/alerts/rules\":{\"get\":{\"summary\":\"The alert rules\","
         "\"description\":\"One row per condition: whether it is on, its threshold, whether it mails (1), calls a webhook (2) or both (3), its cool-down and whether it waits for the daily digest. A webhook secret is never returned; webhook_secret_set says whether one is configured. Refused to every API key. The administrator password, or an account whose role holds view_configuration and reaches every domain.\","
         "\"responses\":{\"200\":{\"description\":\"The rules.\"}}}}"
         ",\"/api/v1/alerts/rules/{condition}\":{\"put\":{\"summary\":\"Change or create an alert rule\","
         "\"description\":\"Fields left out keep the value they have. webhook_secret is write-only; an empty string clears it. A condition that does not exist yet is created, which is how a condition raised by an event script or a future check is configured without a schema change. Refused to every API key. The administrator password, or an account whose role holds manage_settings and reaches every domain.\","
         "\"parameters\":[{\"name\":\"condition\",\"in\":\"path\",\"required\":true,\"schema\":{\"type\":\"string\"}}],"
         "\"responses\":{\"200\":{\"description\":\"The rule as it now stands.\"},\"201\":{\"description\":\"The rule was created.\"},\"400\":{\"description\":\"The rule was refused, with the reason.\"}}}}"
         ",\"/api/v1/alerts/test\":{\"post\":{\"summary\":\"Raise a condition on request\","
         "\"description\":\"Raises the named condition through exactly the path a real one takes - the same rule, the same cool-down, the same ceiling - so that what arrives is what would arrive. Answers raised:false when the condition has no rule, its rule is off, or the condition is already raised. Refused to every API key. The administrator password, or an account whose role holds manage_settings and reaches every domain.\","
         "\"responses\":{\"200\":{\"description\":\"Whether an event was recorded.\"},\"400\":{\"description\":\"No condition was named.\"}}}}"
         ",\"/api/v1/alerts/run\":{\"post\":{\"summary\":\"Run one alerting pass now\","
         "\"description\":\"Evaluates the scheduled conditions, sends what is waiting, attempts the webhooks that are due and sends the digest if this is its hour - the same pass the scheduled task makes every minute. Refused to every API key. The administrator password, or an account whose role holds manage_server and reaches every domain.\","
         "\"responses\":{\"200\":{\"description\":\"What the pass did.\"}}}}"
         ",\"/api/v1/alerts/events\":{\"get\":{\"summary\":\"What has fired\","
         "\"description\":\"Alert events, newest first: the condition, whether it was raised or cleared, whether it has been notified or is held for the digest, and how the webhook delivery went. Refused to every API key. The administrator password, or an account whose role holds view_configuration and reaches every domain.\","
         "\"parameters\":[{\"name\":\"limit\",\"in\":\"query\",\"schema\":{\"type\":\"integer\"},\"description\":\"Events to return, 1 to 500. Default 100.\"}],"
         "\"responses\":{\"200\":{\"description\":\"The events.\"}}}}";

      return AnsiString(paths);
   }
}
