// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The REST API's DKIM key rotation. See RestApiServer.h and
// Common/AntiSpam/DKIM/DkimRotation.h.
//
//    GET    /api/v1/domains/{domain}/dkim/rotation          Domain.DKIMRotationStatus
//    POST   /api/v1/domains/{domain}/dkim/rotation/start    Domain.DKIMRotationStart   {"selector"?}
//    POST   /api/v1/domains/{domain}/dkim/rotation/check    Domain.DKIMRotationCheck
//    POST   /api/v1/domains/{domain}/dkim/rotation/switch   Domain.DKIMRotationSwitch  {"force"?}
//    POST   /api/v1/domains/{domain}/dkim/rotation/retire   Domain.DKIMRotationRetire
//    DELETE /api/v1/domains/{domain}/dkim/rotation/next     Domain.DKIMRotationCancel
//    POST   /api/v1/dkim/rotation/run                       Utilities.RunDkimRotation  {"seconds_ahead"?}
//
// The policy is not here: it is five keys of the domain itself, written by PUT
// /api/v1/domains/{domain} and shown by its listing, so that it travels with the
// domain everywhere the domain already goes (hmctl config, the Deck's editor).
//
// Every answer to a domain route is the status document, the same one COM returns;
// a refusal is the server's sentence in error, and a 409 carries the waiting reason
// that refused it and the status document beside it, so a client can draw what is
// in the way without asking again. The six domain routes are scoped to the key's
// domains in Authorize_ and the five that act are refused to a read-only key; the
// run route is server-wide, refused to a key restricted to domains, because it moves
// every domain's rotation and it moves the clock.

#include "StdAfx.h"
#include "RestApiServer.h"
#include "JsonDocument.h"
#include "../AntiSpam/DKIM/DkimRotation.h"
#include "../BO/Domain.h"
#include "../Persistence/PersistentDomain.h"

#include <ctime>
#include <string>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // An absent or empty body is an empty object: every member of every body here
      // is optional.
      bool ParseOptionalObject(const AnsiString &requestBody, JsonValue &body)
      {
         AnsiString trimmed = requestBody;
         trimmed.Trim();

         std::string text = trimmed.IsEmpty() ? std::string("{}") : std::string(trimmed.c_str(), (size_t) trimmed.GetLength());

         std::string parseError;
         return JsonValue::Parse(text, body, parseError) && body.IsObject();
      }

      bool OnlyKnownKeys(const JsonValue &body, const char *known, AnsiString &unknown)
      {
         for (const std::pair<std::string, JsonValue> &member : body.Members())
         {
            if (known == nullptr || member.first != known)
            {
               unknown = AnsiString(member.first.substr(0, 48).c_str());
               return false;
            }
         }
         return true;
      }

      // 3660 days: further than any period the policy can hold.
      const __int64 MaximumSecondsAhead = (__int64) 3660 * 86400;
   }

   HttpResponse
   RestApiServer::HandleDkimRotation_(const Caller &caller, const Route &route, const AnsiString &requestBody)
   {
      const __int64 now = (__int64) time(nullptr);

      JsonValue body;
      if (route.kind == RouteDkimRotationStart || route.kind == RouteDkimRotationSwitch || route.kind == RouteDkimRotationRun)
      {
         if (!ParseOptionalObject(requestBody, body))
            return BuildResponse_(400, "{\"error\":\"the body must be a JSON object\"}");
      }

      if (route.kind == RouteDkimRotationRun)
      {
         AnsiString unknown;
         if (!OnlyKnownKeys(body, "seconds_ahead", unknown))
            return BuildResponse_(400, "{\"error\":\"unknown field: " + JsonEscape_(unknown) + "\"}");

         __int64 secondsAhead = 0;
         const JsonValue *member = body.Get("seconds_ahead");
         if (member && !member->IsNull())
         {
            const double number = member->IsNumber() ? member->AsNumber() : -1.0;
            if (!member->IsNumber() || number < 0 || number > (double) MaximumSecondsAhead || (double) (__int64) number != number)
               return BuildResponse_(400, "{\"error\":\"seconds_ahead must be a whole number between 0 and 316224000\"}");

            secondsAhead = (__int64) number;
         }

         if (secondsAhead > 0)
         {
            LOG_APPLICATION(Formatter::Format("DKIM rotation: a pass was run over REST by {0} with the clock moved {1} second(s) ahead.",
                                              String(caller.identity), secondsAhead));
         }

         DkimRotation::RunSummary summary = DkimRotation::RunAll(now + secondsAhead);
         return BuildResponse_(200, summary.ToJson());
      }

      std::shared_ptr<Domain> domain = std::shared_ptr<Domain>(new Domain());
      if (route.identifier.IsEmpty() || !PersistentDomain::ReadObject(domain, String(route.identifier)) || domain->GetID() <= 0)
         return BuildResponse_(404, "{\"error\":\"domain not found\"}");

      DkimRotation::Result result;

      switch (route.kind)
      {
      case RouteDkimRotationGet:
         result = DkimRotation::Status(domain->GetID(), now);
         break;

      case RouteDkimRotationStart:
         {
            AnsiString unknown;
            if (!OnlyKnownKeys(body, "selector", unknown))
               return BuildResponse_(400, "{\"error\":\"unknown field: " + JsonEscape_(unknown) + "\"}");

            AnsiString selector;
            const JsonValue *member = body.Get("selector");
            if (member && !member->IsNull())
            {
               if (!member->IsString())
                  return BuildResponse_(400, "{\"error\":\"selector must be a string\"}");

               selector = AnsiString(member->AsString().c_str());
            }

            result = DkimRotation::Start(domain->GetID(), selector, now);
         }
         break;

      case RouteDkimRotationCheck:
         result = DkimRotation::CheckNow(domain->GetID(), now);
         break;

      case RouteDkimRotationSwitch:
         {
            AnsiString unknown;
            if (!OnlyKnownKeys(body, "force", unknown))
               return BuildResponse_(400, "{\"error\":\"unknown field: " + JsonEscape_(unknown) + "\"}");

            bool force = false;
            const JsonValue *member = body.Get("force");
            if (member && !member->IsNull())
            {
               if (!member->IsBool())
                  return BuildResponse_(400, "{\"error\":\"force must be true or false\"}");

               force = member->AsBool();
            }

            result = DkimRotation::Switch(domain->GetID(), force, now);
         }
         break;

      case RouteDkimRotationRetire:
         result = DkimRotation::Retire(domain->GetID(), now);
         break;

      case RouteDkimRotationCancel:
         result = DkimRotation::Cancel(domain->GetID(), now);
         break;

      default:
         return BuildResponse_(404, "{\"error\":\"not found\"}");
      }

      if (result.Succeeded())
         return BuildResponse_(result.http_status, result.json);

      AnsiString answer = "{\"error\":\"" + JsonEscape_(Utf8_(result.error)) + "\"";
      if (!result.reason.IsEmpty())
         answer += ",\"reason\":\"" + JsonEscape_(result.reason) + "\"";
      if (!result.json.IsEmpty())
         answer += ",\"status\":" + result.json;
      answer += "}";

      return BuildResponse_(result.http_status, answer);
   }

   AnsiString
   RestApiServer::OpenApiDkimRotationPaths_()
   {
      static const char *paths =
         ",\"/api/v1/domains/{domain}/dkim/rotation\":{\"get\":{\"summary\":\"The state of a domain's DKIM key rotation\","
         "\"description\":\"The policy (enabled, interval_days, propagation_hours, retention_days, key_bits - written through PUT /api/v1/domains/{domain} as the dkim_rotation_ keys); the phase (off, idle, awaiting_dns, propagating, ready or waiting_for_retiring_slot); current, next and retiring, each with its selector, key file, whether the rotation generated it, its times, the TXT records to publish or keep (one per name the selector signs for - the domain and, with dkim_sign_aliases, each domain alias) and the last check of them from the server's resolver (unchecked, matches, not_found, different, lookup_failed or key_unreadable); retired, with the records that may now be removed; what is waiting, the reason word and until when; and the last error. Times are seconds since the epoch. Nothing is written or looked up.\","
         "\"parameters\":[{\"name\":\"domain\",\"in\":\"path\",\"required\":true,\"schema\":{\"type\":\"string\"}}],"
         "\"responses\":{\"200\":{\"description\":\"The status document\"},\"404\":{\"description\":\"Unknown domain\"}}}}"
         ",\"/api/v1/domains/{domain}/dkim/rotation/start\":{\"post\":{\"summary\":\"Rotate now: generate and stage the next key\","
         "\"description\":\"Generates an RSA key of the policy's size under the given selector, or under s and the UTC date when none is given, writes it to the DKIM directory under the data directory readable only by the server and the administrators, and stages it as the domain's secondary. The answer names the TXT record to publish. Signing does not move until the record resolves with this key for the propagation period, whatever the schedule.\","
         "\"parameters\":[{\"name\":\"domain\",\"in\":\"path\",\"required\":true,\"schema\":{\"type\":\"string\"}}],"
         "\"requestBody\":{\"required\":false,\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"properties\":{\"selector\":{\"type\":\"string\",\"description\":\"Lower-case letters, digits and hyphens in dot-separated labels, at most 63 characters.\"}}}}}},"
         "\"responses\":{\"201\":{\"description\":\"Staged; the status document\"},\"400\":{\"description\":\"The selector is not one\"},\"404\":{\"description\":\"Unknown domain\"},\"409\":{\"description\":\"A next key is already staged, or the selector is in use; error, reason and status\"}}}}"
         ",\"/api/v1/domains/{domain}/dkim/rotation/check\":{\"post\":{\"summary\":\"Check the rotation's records now\","
         "\"description\":\"Looks up the next, current and retiring TXT records from the server's own resolver now, rather than at the next scheduled pass, and records what was found.\","
         "\"parameters\":[{\"name\":\"domain\",\"in\":\"path\",\"required\":true,\"schema\":{\"type\":\"string\"}}],"
         "\"responses\":{\"200\":{\"description\":\"The status document\"},\"404\":{\"description\":\"Unknown domain\"}}}}"
         ",\"/api/v1/domains/{domain}/dkim/rotation/switch\":{\"post\":{\"summary\":\"Move signing to the next key now\","
         "\"description\":\"Refused unless the next key's record matches in a check made by this request, and - without force - unless it has resolved for the whole propagation period. force skips the period and never the check. Refused while an old selector is still retiring.\","
         "\"parameters\":[{\"name\":\"domain\",\"in\":\"path\",\"required\":true,\"schema\":{\"type\":\"string\"}}],"
         "\"requestBody\":{\"required\":false,\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"properties\":{\"force\":{\"type\":\"boolean\"}}}}}},"
         "\"responses\":{\"200\":{\"description\":\"Switched; the status document\"},\"404\":{\"description\":\"Unknown domain\"},\"409\":{\"description\":\"Refused: nothing_staged, record_not_found, record_differs, lookup_failed, key_unreadable, propagation, retiring_slot or changed in reason, with error and status\"}}}}"
         ",\"/api/v1/domains/{domain}/dkim/rotation/retire\":{\"post\":{\"summary\":\"Retire the old selector now\","
         "\"description\":\"Ends the retention period early: the old selector's record may be removed, and its key file is deleted if the rotation generated it and nothing else names it.\","
         "\"parameters\":[{\"name\":\"domain\",\"in\":\"path\",\"required\":true,\"schema\":{\"type\":\"string\"}}],"
         "\"responses\":{\"200\":{\"description\":\"Retired; the status document\"},\"404\":{\"description\":\"Unknown domain\"},\"409\":{\"description\":\"Nothing is retiring\"}}}}"
         ",\"/api/v1/domains/{domain}/dkim/rotation/next\":{\"delete\":{\"summary\":\"Cancel the next key\","
         "\"description\":\"Empties the domain's secondary selector and deletes the key file the rotation generated for it. Signing is unchanged.\","
         "\"parameters\":[{\"name\":\"domain\",\"in\":\"path\",\"required\":true,\"schema\":{\"type\":\"string\"}}],"
         "\"responses\":{\"200\":{\"description\":\"Cancelled; the status document\"},\"404\":{\"description\":\"Unknown domain\"},\"409\":{\"description\":\"Nothing is staged\"}}}}"
         ",\"/api/v1/dkim/rotation/run\":{\"post\":{\"summary\":\"Run one pass of the DKIM key rotation now\","
         "\"description\":\"The pass the scheduled task makes every fifteen minutes, over every domain whose schedule is on or whose rotation is in flight. seconds_ahead moves the clock forward, 0 to 316224000 (3660 days), which is how the regression suite tests a propagation or retention period without waiting. The offset is this request's, but what the pass does is not: a switch or retire it makes happens now, early, and the times it records are the moved clock's and stay recorded, so later passes on the real clock wait that much longer. Not for a production server. A non-zero value is written to the application log. Refused to a key restricted to domains.\","
         "\"requestBody\":{\"required\":false,\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"properties\":{\"seconds_ahead\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":316224000}}}}}},"
         "\"responses\":{\"200\":{\"description\":\"as_of, domains, generated, checked, switched, retired, errors\"},\"400\":{\"description\":\"seconds_ahead out of range\"}}}}";

      return AnsiString(paths);
   }
}
