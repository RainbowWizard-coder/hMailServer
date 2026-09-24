// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The REST API's view of the sandbox hold. See RestApiServer.h and
// Common/AntiVirus/Sandbox.h.
//
//    GET    /api/v1/sandbox/held                    what is held for a verdict, oldest first, a page at a time
//    POST   /api/v1/sandbox/held/{queue_id}/release delivered now, without the verdict
//    DELETE /api/v1/sandbox/held/{queue_id}         gone, and nobody told
//
// The settings are not here: they are the sandbox_* keys of PUT
// /api/v1/settings, so they travel with the rest of the configuration.
//
// The held list has the delivery queue's shape - one entry names a sender and
// recipients in any number of domains - so it takes the queue's permissions
// and is refused to a credential confined to named domains, as the queue is. A
// read-only key reads it and changes nothing.
//
// A release or a discard is not a row the statement chokepoint records: the
// hold's rows are traffic, and the message's row is the queue's. Each is
// recorded with RecordEvent under the actor the dispatch installed, because a
// release delivers mail the server was waiting to judge.

#include "StdAfx.h"
#include "RestApiServer.h"
#include "HttpServer.h"
#include "AuditTrail.h"
#include "../AntiVirus/Sandbox.h"
#include "../Util/Parsing/StringParser.h"

#include <ctime>
#include <limits>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // A page of GET /api/v1/sandbox/held: 100 unless asked, 1000 at most.
      // GET /api/v1/queue lists every held message, as it lists every queued
      // one; neither caps what it holds without saying so.
      const int DefaultHeldPage = 100;
      const int MaxHeldPage = 1000;

      // A count in a query: digits only, within the bounds.
      bool ParseCount(const AnsiString &text, int minimum, int maximum, int &value)
      {
         if (text.IsEmpty() || text.GetLength() > 9)
            return false;

         for (int i = 0; i < text.GetLength(); i++)
         {
            if (text[i] < '0' || text[i] > '9')
               return false;
         }

         value = atoi(text.c_str());
         return value >= minimum && value <= maximum;
      }

      AnsiString FileJson(const Sandbox::File &file)
      {
         AnsiString numbers;
         numbers.Format("{\"size\":%I64d,\"score\":%d,\"errors\":%d,\"state\":\"%hs\",",
            file.size, file.score, file.errors, Sandbox::FileStateWord(file.state));

         AnsiString json = numbers;
         json += "\"filename\":\"" + RestApiServer::JsonEscapeUtf8(file.filename) + "\"";
         json += ",\"sha256\":\"" + RestApiServer::JsonEscapeUtf8(String(file.sha256)) + "\"";
         json += ",\"submission\":\"" + RestApiServer::JsonEscapeUtf8(file.submission) + "\"";
         json += ",\"verdict\":\"" + RestApiServer::JsonEscapeUtf8(file.verdict) + "\"";
         json += ",\"detail\":\"" + RestApiServer::JsonEscapeUtf8(file.detail) + "\"}";
         return json;
      }

      AnsiString HeldJson(const Sandbox::Held &held)
      {
         const __int64 now = (__int64) time(nullptr);
         const __int64 seconds = held.held > 0 && now > held.held ? now - held.held : 0;

         AnsiString numbers;
         numbers.Format("{\"queue_id\":%I64d,\"held\":%I64d,\"held_seconds\":%I64d,\"decision\":\"%hs\",",
            held.message_id, held.held, seconds, Sandbox::DecisionWord(held.decision));

         AnsiString json = numbers;
         json += "\"from\":\"" + RestApiServer::JsonEscapeUtf8(held.from) + "\"";
         json += ",\"recipients\":\"" + RestApiServer::JsonEscapeUtf8(held.recipients) + "\"";
         json += ",\"created\":\"" + RestApiServer::JsonEscapeUtf8(held.created) + "\"";
         json += ",\"files\":[";
         for (size_t i = 0; i < held.files.size(); i++)
            json += (i ? "," : "") + FileJson(held.files[i]);
         json += "]}";
         return json;
      }

   }

   AnsiString
   RestApiServer::SandboxQueueItems_(int &count)
   {
      count = 0;

      std::vector<Sandbox::Held> held;
      int total = 0;
      if (!Sandbox::ListHeld(0, (std::numeric_limits<int>::max)(), held, total))
         return AnsiString();

      AnsiString items;
      for (const Sandbox::Held &entry : held)
      {
         AnsiString numbers;
         numbers.Format("{\"id\":%I64d,\"locked\":%hs,\"tries\":%d,\"held\":\"sandbox\",\"held_since\":%I64d,",
            entry.message_id, entry.locked ? "true" : "false", entry.tries, entry.held);

         AnsiString item = numbers;
         item += "\"created\":\"" + JsonEscape_(Utf8_(entry.created)) + "\"";
         item += ",\"from\":\"" + JsonEscape_(Utf8_(entry.from)) + "\"";
         item += ",\"recipients\":\"" + JsonEscape_(Utf8_(entry.recipients)) + "\"";
         item += ",\"next_try\":\"\",\"submissions\":[";

         bool first = true;
         for (const Sandbox::File &file : entry.files)
         {
            if (file.submission.IsEmpty())
               continue;

            item += (first ? "" : ",");
            item += "\"" + JsonEscape_(Utf8_(file.submission)) + "\"";
            first = false;
         }

         item += "]}";

         if (count > 0)
            items += ",";

         items += item;
         count++;
      }

      return items;
   }

   HttpResponse
   RestApiServer::HandleSandboxHeld_(const Caller &caller, const Route &route)
   {
      if (route.kind == RouteSandboxHeldList)
      {
         int offset = 0;
         int limit = DefaultHeldPage;

         const AnsiString offsetText = QueryParameter_(route.query, "offset");
         if (!offsetText.IsEmpty() && !ParseCount(offsetText, 0, 999999999, offset))
            return BuildResponse_(400, "{\"error\":\"offset is a whole number, 0 or more\"}");

         const AnsiString limitText = QueryParameter_(route.query, "limit");
         if (!limitText.IsEmpty() && !ParseCount(limitText, 1, MaxHeldPage, limit))
            return BuildResponse_(400, "{\"error\":\"limit is a whole number from 1 to 1000\"}");

         std::vector<Sandbox::Held> held;
         int total = 0;
         if (!Sandbox::ListHeld(offset, limit, held, total))
            return BuildResponse_(500, "{\"error\":\"the held messages could not be read\"}");

         Configuration *configuration = Configuration::Instance();
         const bool enabled = configuration->GetSandboxEnabled();
         const bool active = enabled && !configuration->GetSandboxSubmitUrl().IsEmpty() && !configuration->GetSandboxVerdictUrl().IsEmpty();

         AnsiString body;
         body.Format("{\"enabled\":%hs,\"active\":%hs,\"timeout_minutes\":%d,\"timeout_action\":\"%hs\",\"poll_seconds\":%d,\"max_files\":%d,\"total\":%d,\"offset\":%d,\"count\":%d,\"messages\":[",
            enabled ? "true" : "false",
            active ? "true" : "false",
            configuration->GetSandboxTimeoutMinutes(),
            configuration->GetSandboxTimeoutAction() == 1 ? "bounce" : "deliver",
            configuration->GetSandboxPollSeconds(),
            configuration->GetSandboxMaxFiles(),
            total,
            offset,
            (int) held.size());

         for (size_t i = 0; i < held.size(); i++)
            body += (i ? "," : "") + HeldJson(held[i]);

         body += "]}";
         return BuildResponse_(200, body);
      }

      if (route.kind == RouteSandboxHeldRelease || route.kind == RouteSandboxHeldDiscard)
      {
         Sandbox::Held held;
         if (!Sandbox::ReadHeld(route.message_id, held))
            return BuildResponse_(404, "{\"error\":\"no message with that id is held for the sandbox\"}");

         const bool release = route.kind == RouteSandboxHeldRelease;

         String problem;
         const bool done = release ? Sandbox::Release(route.message_id, problem) : Sandbox::Discard(route.message_id, problem);
         if (!done)
         {
            if (problem == _T("not held"))
               return BuildResponse_(404, "{\"error\":\"no message with that id is held for the sandbox\"}");

            // A verdict, the timeout or the switch decided the hold between the
            // read above and the release: that decision stands, and nothing is
            // recorded as released that was not.
            if (problem == _T("decided"))
               return BuildResponse_(409, "{\"error\":\"the sandbox decided this message while it was being released; it goes on as that decision says\"}");

            return BuildResponse_(500, "{\"error\":\"" + JsonEscape_(Utf8_(problem)) + "\"}");
         }

         const String detail = Sandbox::Describe(held);
         if (release)
            AuditTrail::Instance()->RecordEvent(_T("sandbox hold"), StringParser::IntToString(held.message_id), _T("released"), detail);
         else
            AuditTrail::Instance()->RecordEvent(_T("sandbox hold"), StringParser::IntToString(held.message_id), _T("discarded"), detail);

         LOG_APPLICATION("RestApi: the message " + StringParser::IntToString(held.message_id) + " held for the sandbox's verdict" +
                         (release ? " was released to the delivery queue, without the verdict, by " : " was discarded by ") + String(caller.identity) + ".");

         return BuildResponse_(200, release ? "{\"released\":true}" : "{\"discarded\":true}");
      }

      return BuildResponse_(404, "{\"error\":\"not found\"}");
   }

   AnsiString
   RestApiServer::OpenApiSandboxPaths_()
   {
      static const char *paths =
         ",\"/api/v1/sandbox/held\":{\"get\":{\"summary\":\"Messages held for a sandbox's verdict on their attachments\","
         "\"parameters\":[{\"name\":\"offset\",\"in\":\"query\",\"schema\":{\"type\":\"integer\",\"minimum\":0},\"description\":\"How many held messages to skip, oldest first; 0 by default\"},"
         "{\"name\":\"limit\",\"in\":\"query\",\"schema\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":1000},\"description\":\"How many to answer; 100 by default\"}],"
         "\"description\":\"{enabled,active,timeout_minutes,timeout_action,poll_seconds,max_files,total,offset,count,messages:[{queue_id,from,recipients,created,held,held_seconds,decision,files:[{filename,size,sha256,submission,state,verdict,score,detail,errors}]}]}, oldest first, a page at a time: total is how many are held in all and count how many this page has. A message that no signed-in user of this server submitted, with an attachment of a submitted type (sandbox_types), waits here, in no mailbox and not in the quarantine, while the poller submits each such file to sandbox_submit_url and asks sandbox_verdict_url, until the sandbox has answered for every file, one answer is malicious, or sandbox_timeout_minutes has passed since held (seconds since 1970). active is false while sandbox_enabled is off or an address is empty, when nothing is held. state is not_submitted (not yet accepted by the sandbox; the poller submits it), submitted (waiting for the verdict), answered, or not_submittable (larger than sandbox_max_size_kb, or unreadable; not submitted, and said in the header). submission is the id the sandbox answered; errors counts the requests for the file that failed. decision is pending while the message waits. Server-wide, like the delivery queue, and refused to a credential restricted to named domains for the same reason.\","
         "\"responses\":{\"200\":{\"description\":\"A page of the held messages\"},\"400\":{\"description\":\"offset or limit is not a whole number in range\"}}}}"
         ",\"/api/v1/sandbox/held/{queue_id}/release\":{\"post\":{\"summary\":\"Deliver a held message without the sandbox's verdict\","
         "\"description\":\"An administrator's decision: the message goes back into the delivery queue now and is delivered with X-hMailServer-Sandbox: released by administrator. Nothing is submitted again. Recorded on the audit trail, naming the sender, the recipients and the files. When a verdict, the timeout or the switch decided the hold first, that decision stands, the answer is 409 and nothing is recorded as released. Refused to a read-only key.\","
         "\"responses\":{\"200\":{\"description\":\"Released\"},\"404\":{\"description\":\"No message with that id is held for the sandbox\"},\"409\":{\"description\":\"The hold was decided while the release was being made; the message goes on as that decision says\"}}}}"
         ",\"/api/v1/sandbox/held/{queue_id}\":{\"delete\":{\"summary\":\"Discard a held message\","
         "\"description\":\"The message is removed from the queue and its file deleted; nothing is sent and nobody is told. Recorded on the audit trail. Refused to a read-only key.\","
         "\"responses\":{\"200\":{\"description\":\"Discarded\"},\"404\":{\"description\":\"No message with that id is held for the sandbox\"}}}}";
      return AnsiString(paths);
   }
}
