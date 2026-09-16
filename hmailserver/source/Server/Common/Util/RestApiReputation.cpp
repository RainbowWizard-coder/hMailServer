// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The REST API's own-reputation watch. See RestApiServer.h and
// Common/Util/ReputationWatch.h.
//
//    GET  /api/v1/reputation          what the last check found
//    POST /api/v1/reputation/check    check now
//
// What is checked - the addresses and the lists - is not here: it is three keys
// of PUT /api/v1/settings (reputation_watch_enabled, reputation_watch_lists and
// reputation_watch_addresses), so it travels with the rest of the configuration
// through hmctl config and appears on the Deck's Settings page like any other
// setting. This file is the report and the check.
//
// Both routes are the server's rather than a domain's: the addresses this server
// sends from are the same addresses whichever domain's mail is leaving, so a
// credential confined to named domains is refused both. Reading needs
// view_configuration; the check needs manage_server, because it makes this
// server send queries to third parties and rewrites what the report says - the
// same shape as running the alert pass or the DKIM rotation pass now.

#include "StdAfx.h"
#include "RestApiServer.h"
#include "JsonDocument.h"
#include "ReputationWatch.h"

#include <ctime>
#include <string>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   HttpResponse
   RestApiServer::HandleReputation_(const Caller &caller, const Route &route, const AnsiString &requestBody)
   {
      const __int64 now = (__int64) time(nullptr);

      if (route.kind == RouteReputationGet)
         return BuildResponse_(200, ReputationWatch::ReportJson(now));

      if (route.kind != RouteReputationCheck)
         return BuildResponse_(404, "{\"error\":\"not found\"}");

      // The check takes no arguments at all. An absent or empty body is the empty
      // object; a field is refused by name rather than ignored, so a client that
      // thinks it is asking for something narrower finds out.
      AnsiString trimmed = requestBody;
      trimmed.Trim();

      if (!trimmed.IsEmpty())
      {
         JsonValue body;
         std::string parseError;

         if (!JsonValue::Parse(std::string(trimmed.c_str(), (size_t) trimmed.GetLength()), body, parseError) || !body.IsObject())
            return BuildResponse_(400, "{\"error\":\"the body must be a JSON object\"}");

         for (const std::pair<std::string, JsonValue> &member : body.Members())
         {
            const AnsiString unknown = AnsiString(member.first.substr(0, 48).c_str());
            return BuildResponse_(400, "{\"error\":\"unknown field: " + JsonEscape_(unknown) + "\"}");
         }
      }

      // A check is a handful of DNS queries made while this request waits, so who
      // asked for it is worth a line: on a server whose resolver is not answering
      // it is also the slowest request the API has.
      LOG_APPLICATION(Formatter::Format("Reputation watch: a check was run over REST by {0}.", String(caller.identity)));

      // A check that is already running is answered rather than queued behind.
      // This request would otherwise wait out the whole of another one, and a
      // check makes a lookup per address per list.
      ReputationWatch::RunSummary summary;

      if (!ReputationWatch::TryRun(now, summary))
         return BuildResponse_(409, "{\"error\":\"a check is already running; ask again when it has finished\"}");

      return BuildResponse_(200, ReputationWatch::RunJson(summary, now));
   }

   AnsiString
   RestApiServer::OpenApiReputationPaths_()
   {
      static const char *paths =
         ",\"/api/v1/reputation\":{\"get\":{\"summary\":\"What the last check of this server's own addresses found\","
         "\"description\":\"The addresses this server is seen as sending from - the delivery bind address, the addresses its active global rules bind, and reputation_watch_addresses - each with what every block list in reputation_watch_lists last answered about it. Per list: status (listed, not_listed, refused, lookup_failed, unexpected_answer, list_not_answering, list_lists_everything, not_applicable or not_checked), listed as true, false or null (null is 'the list did not say', which is never the same as not listed), the answer in code, the list's own TXT or the sentence saying what went wrong in reason, when it was checked, when a list last said something definite about it in confirmed, and since when the address has been listed there. A listing no check has confirmed for a week stops counting as one: the pair reads as unknown rather than as listed, and never as clean. skipped names the addresses that will not be looked up at all - private, loopback, link-local, shared, multicast, reserved, unspecified, an IPv4 address written as IPv6, or one past the number of addresses this watch checks - with the reason. Nothing is looked up or written; last_checked and next_check are seconds since the epoch.\","
         "\"responses\":{\"200\":{\"description\":\"The report\"}}}}"
         ",\"/api/v1/reputation/check\":{\"post\":{\"summary\":\"Check this server's outbound addresses against the block lists now\","
         "\"description\":\"The check the server makes once a day, made now: two queries per list for the RFC 5782 test entries 127.0.0.2 and 127.0.0.1 - which is how a list that refuses this resolver, or is not answering it, or answers for every address, is told apart from a list that says nothing is listed - and then one query per address per list, with a TXT lookup for the reason where there is one. A lookup that fails or is refused is reported as such and never as clean, and it does not clear a listing an earlier check found. The answer is the report with a run member holding as_of, addresses, lists, lookups, listed, unknown, stored and complete. A check is bounded in time: complete is false when it reached that bound, and the pairs it did not reach are in the report as failed lookups rather than as clean. Takes no fields; the addresses and the lists are settings. Refused to a key restricted to domains, and the slowest request in this API when a resolver is not answering.\","
         "\"requestBody\":{\"required\":false,\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"additionalProperties\":false}}}},"
         "\"responses\":{\"200\":{\"description\":\"The report, with what this check did\"},\"400\":{\"description\":\"The body carried a field\"},\"409\":{\"description\":\"A check is already running\"}}}}";

      return AnsiString(paths);
   }
}
