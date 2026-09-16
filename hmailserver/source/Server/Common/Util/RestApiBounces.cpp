// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Bounces read, and the suppression list, over REST.
//
//   GET    /api/v1/domains/{domain}/suppressions            the list, newest first
//   POST   /api/v1/domains/{domain}/suppressions            put an address on it by hand
//   DELETE /api/v1/domains/{domain}/suppressions/{address}  take one off
//   GET    /api/v1/domains/{domain}/bounces                 the per-address counts
//   POST   /api/v1/bounces/classify                         how a status would be read
//   POST   /api/v1/bounces/expire                           the hourly sweep, now
//
// The settings group, /api/v1/settings/bounces, is a table in RestApiSettings.cpp
// like every other group. The four domain routes are scoped to their domain in
// Authorize_, so a key issued for one domain reads and changes that domain's list
// and no other; the sweep is server-wide and refused to such a key; classify reads
// and writes nothing and is allowed to any credential that reaches the API.
//
// The documents are built by BounceTracker, which is also what the COM methods the
// Control Panel calls return, so the two surfaces cannot drift apart.

#include "StdAfx.h"

#include "RestApiServer.h"

#include "JsonDocument.h"
#include "Unicode.h"
#include "../../SMTP/BounceTracker.h"

#include <climits>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // A path segment as the client sent it, with %XX decoded, so that an address
      // written as gone%40remote.test names the same entry as gone@remote.test.
      AnsiString DecodePathSegment(const AnsiString &raw)
      {
         AnsiString value;

         for (int i = 0; i < raw.GetLength(); i++)
         {
            char c = raw.GetAt(i);

            if (c == '%' && i + 2 < raw.GetLength() && isxdigit((unsigned char) raw.GetAt(i + 1)) && isxdigit((unsigned char) raw.GetAt(i + 2)))
            {
               char hex[3] = { raw.GetAt(i + 1), raw.GetAt(i + 2), 0 };
               value += (char) strtol(hex, nullptr, 16);
               i += 2;
            }
            else
            {
               value += c;
            }
         }

         return value;
      }

      // One line in the log, whatever the caller put in the path.
      String OneLineForLog(const String &address)
      {
         String value = address.Left(255);

         for (int i = 0; i < value.GetLength(); i++)
         {
            if (value.GetAt(i) < 32)
               value.SetAt(i, '?');
         }

         return value;
      }

      int ParseCount(const AnsiString &text, int fallback, int minimum, int maximum)
      {
         if (text.IsEmpty())
            return fallback;

         int value = atoi(text.c_str());

         if (value < minimum)
            return minimum;
         if (value > maximum)
            return maximum;

         return value;
      }
   }

   HttpResponse
   RestApiServer::HandleBounces_(const Route &route, const AnsiString &requestBody)
   {
      switch (route.kind)
      {
      case RouteBounceClassify:
         {
            JsonValue document;
            std::string parseError;
            if (!JsonValue::Parse(std::string(requestBody.c_str(), requestBody.GetLength()), document, parseError) || !document.IsObject())
               return BuildResponse_(400, "{\"error\":\"the body must be a JSON object\"}");

            String status;
            String diagnostic;
            Unicode::MultiByteToWide(AnsiString(document.GetString("status").c_str()), status);
            Unicode::MultiByteToWide(AnsiString(document.GetString("diagnostic").c_str()), diagnostic);
            status.Trim();

            if (status.IsEmpty())
               return BuildResponse_(400, "{\"error\":\"status is required: an enhanced status code such as 5.1.1, or a reply such as 550 5.1.1 User unknown\"}");

            return BuildResponse_(200, BounceTracker::ClassifyJson(status, diagnostic));
         }

      case RouteBounceExpire:
         {
            int suppressionsExpired = 0;
            int countsPruned = 0;
            int reportKeysPruned = 0;

            if (!BounceTracker::RunMaintenance(suppressionsExpired, countsPruned, reportKeysPruned))
               return BuildResponse_(500, "{\"error\":\"the maintenance sweep failed part way; what it removed before failing has been removed\"}");

            LOG_APPLICATION(Formatter::Format(_T("RestApi: the suppression list's maintenance sweep removed {0} entr(ies), {1} count(s) and {2} report key(s)."),
               suppressionsExpired, countsPruned, reportKeysPruned));

            AnsiString body;
            body.Format("{\"suppressions_expired\":%d,\"counts_pruned\":%d,\"report_keys_pruned\":%d}",
               suppressionsExpired, countsPruned, reportKeysPruned);

            return BuildResponse_(200, body);
         }

      default:
         break;
      }

      // The four routes under a domain.
      String domainName;
      Unicode::MultiByteToWide(DecodePathSegment(route.identifier), domainName);

      const __int64 domainID = BounceTracker::DomainIDByName(domainName);
      if (domainID <= 0)
         return BuildResponse_(404, "{\"error\":\"domain not found\"}");

      switch (route.kind)
      {
      case RouteSuppressionList:
      case RouteBounceCountList:
         {
            String search;
            Unicode::MultiByteToWide(QueryParameter_(route.query, "search"), search);

            const int limit = ParseCount(QueryParameter_(route.query, "limit"), 100, 1, 1000);
            const int offset = ParseCount(QueryParameter_(route.query, "offset"), 0, 0, INT_MAX);

            AnsiString body;
            const bool read = route.kind == RouteSuppressionList
               ? BounceTracker::SuppressionsJson(domainID, search, limit, offset, body)
               : BounceTracker::CountsJson(domainID, search, limit, offset, body);

            if (!read)
               return BuildResponse_(500, "{\"error\":\"the suppression list could not be read\"}");

            return BuildResponse_(200, body);
         }

      case RouteSuppressionCreate:
         {
            JsonValue document;
            std::string parseError;
            if (!JsonValue::Parse(std::string(requestBody.c_str(), requestBody.GetLength()), document, parseError) || !document.IsObject())
               return BuildResponse_(400, "{\"error\":\"the body must be a JSON object\"}");

            const JsonValue *addressMember = document.Get("address");
            if (!addressMember || !addressMember->IsString())
               return BuildResponse_(400, "{\"error\":\"address is required\"}");

            String address;
            String note;
            Unicode::MultiByteToWide(AnsiString(addressMember->AsString().c_str()), address);
            Unicode::MultiByteToWide(AnsiString(document.GetString("note").c_str()), note);
            address.Trim();

            __int64 expiresDays = 0;
            const JsonValue *expiresMember = document.Get("expires_days");
            if (expiresMember)
            {
               if (!expiresMember->IsNumber() || (double) expiresMember->AsInt64() != expiresMember->AsNumber())
                  return BuildResponse_(400, "{\"error\":\"expires_days must be a whole number of days from 0 to 3650\"}");

               expiresDays = expiresMember->AsInt64();
            }

            if (expiresDays < 0 || expiresDays > 3650)
               return BuildResponse_(400, "{\"error\":\"expires_days must be a whole number of days from 0 to 3650\"}");

            switch (BounceTracker::AddManual(domainID, address, note, (int) expiresDays))
            {
            case BounceTracker::AddAdded:
               LOG_APPLICATION(_T("RestApi: ") + OneLineForLog(address) + _T(" was put on the suppression list of ") + domainName + _T(" by hand."));
               return BuildResponse_(201, BounceTracker::SuppressionItemJson(domainID, address));
            case BounceTracker::AddAlreadyListed:
               return BuildResponse_(409, "{\"error\":\"that address is already on the suppression list of this domain\"}");
            case BounceTracker::AddInvalidAddress:
               return BuildResponse_(400, "{\"error\":\"address is not a valid email address\"}");
            case BounceTracker::AddHostedAddress:
               return BuildResponse_(400, "{\"error\":\"that address is hosted on this server; the suppression list only applies to addresses outside it\"}");
            case BounceTracker::AddInvalidExpiry:
               return BuildResponse_(400, "{\"error\":\"expires_days must be a whole number of days from 0 to 3650\"}");
            default:
               return BuildResponse_(500, "{\"error\":\"the entry could not be written\"}");
            }
         }

      case RouteSuppressionDelete:
         {
            String address;
            Unicode::MultiByteToWide(DecodePathSegment(route.name), address);
            address.Trim();

            const int removed = BounceTracker::Remove(domainID, address);

            if (removed < 0)
               return BuildResponse_(500, "{\"error\":\"the entry could not be removed\"}");

            if (removed == 0)
               return BuildResponse_(404, "{\"error\":\"that address is not on the suppression list of this domain\"}");

            LOG_APPLICATION(_T("RestApi: ") + OneLineForLog(address) + _T(" was removed from the suppression list of ") + domainName + _T("."));

            return BuildResponse_(200, "{\"removed\":true}");
         }

      default:
         break;
      }

      return BuildResponse_(404, "{\"error\":\"not found\"}");
   }

   AnsiString
   RestApiServer::OpenApiBouncesPaths_()
   {
      static const char *paths =
         ",\"/api/v1/domains/{domain}/suppressions\":{"
         "\"get\":{\"summary\":\"The suppression list of a domain\","
         "\"description\":\"Addresses this domain no longer sends to because mail to them bounced, or because an administrator put them there. Newest first. Each item: address, source (bounce: refused while delivering; report: an RFC 3464 delivery report that matched a message this server sent; manual: an administrator), category (hard, mailbox_full, policy, other, or none for a manual entry), status, diagnostic (the remote's own words, or the administrator's note), remote_mta, created, expires (empty for none) and expired (true for an entry that no longer applies and has not been swept yet). Also answers processing_enabled (whether bounces are read and counted), list_applies (whether the entries below are enforced, which is true whenever bounce processing OR complaint processing is on, because the feedback loops fill this same list) and action, because a list that is not being applied is worth saying so. Scoped to the domain for a domain-restricted key.\","
         "\"parameters\":[{\"name\":\"domain\",\"in\":\"path\",\"required\":true,\"schema\":{\"type\":\"string\"}},"
         "{\"name\":\"search\",\"in\":\"query\",\"schema\":{\"type\":\"string\"},\"description\":\"A substring of the address.\"},"
         "{\"name\":\"limit\",\"in\":\"query\",\"schema\":{\"type\":\"integer\"},\"description\":\"Items to return, 1 to 1000. Default 100.\"},"
         "{\"name\":\"offset\",\"in\":\"query\",\"schema\":{\"type\":\"integer\"},\"description\":\"Items to skip.\"}],"
         "\"responses\":{\"200\":{\"description\":\"domain, processing_enabled, list_applies, action, total, offset and items\"},\"404\":{\"description\":\"Unknown domain\"}}},"
         "\"post\":{\"summary\":\"Put an address on a domain's suppression list by hand\","
         "\"description\":\"Body: address (required), note (kept as the entry's diagnostic) and expires_days (0, the default, for an entry that stays until it is removed; at most 3650). An address this server hosts is refused, because the list only ever applies to mail leaving it. Recorded in the audit trail. Scoped to the domain for a domain-restricted key; refused for a read-only key.\","
         "\"parameters\":[{\"name\":\"domain\",\"in\":\"path\",\"required\":true,\"schema\":{\"type\":\"string\"}}],"
         "\"requestBody\":{\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"required\":[\"address\"],\"properties\":{\"address\":{\"type\":\"string\"},\"note\":{\"type\":\"string\"},\"expires_days\":{\"type\":\"integer\"}}}}}},"
         "\"responses\":{\"201\":{\"description\":\"The entry as stored\"},\"400\":{\"description\":\"The address is not valid, is hosted here, or expires_days is out of range\"},\"404\":{\"description\":\"Unknown domain\"},\"409\":{\"description\":\"Already listed\"}}}},"
         "\"/api/v1/domains/{domain}/suppressions/{address}\":{\"delete\":{\"summary\":\"Take an address off a domain's suppression list\","
         "\"description\":\"The next message to it is sent, and its run of hard bounces starts again at none, so one more hard bounce is one and not the threshold plus one. Recorded in the audit trail. Scoped to the domain for a domain-restricted key; refused for a read-only key.\","
         "\"parameters\":[{\"name\":\"domain\",\"in\":\"path\",\"required\":true,\"schema\":{\"type\":\"string\"}},{\"name\":\"address\",\"in\":\"path\",\"required\":true,\"schema\":{\"type\":\"string\"}}],"
         "\"responses\":{\"200\":{\"description\":\"{removed: true}\"},\"404\":{\"description\":\"Unknown domain, or the address is not listed\"}}}},"
         "\"/api/v1/domains/{domain}/bounces\":{\"get\":{\"summary\":\"The bounce counts of a domain's recipients\","
         "\"description\":\"Per address this domain has sent to and had bounce: hard, soft, mailbox_full, policy and other counts, consecutive_hard (hard bounces with no delivery between them, which is what the threshold is measured against; a message a remote accepted sets the run aside until the report that it failed after all arrives, so it reads 0 while that report may still come), complaints (abuse reports the feedback loops sent about it, which count towards a threshold of their own and never towards the run), last_status, last_category, last_bounce, last_delivered, last_complaint and whether the address is on the list now. Newest first. Scoped to the domain for a domain-restricted key.\","
         "\"parameters\":[{\"name\":\"domain\",\"in\":\"path\",\"required\":true,\"schema\":{\"type\":\"string\"}},"
         "{\"name\":\"search\",\"in\":\"query\",\"schema\":{\"type\":\"string\"},\"description\":\"A substring of the address.\"},"
         "{\"name\":\"limit\",\"in\":\"query\",\"schema\":{\"type\":\"integer\"},\"description\":\"Items to return, 1 to 1000. Default 100.\"},"
         "{\"name\":\"offset\",\"in\":\"query\",\"schema\":{\"type\":\"integer\"},\"description\":\"Items to skip.\"}],"
         "\"responses\":{\"200\":{\"description\":\"domain, total, offset and items\"},\"404\":{\"description\":\"Unknown domain\"}}}},"
         "\"/api/v1/bounces/classify\":{\"post\":{\"summary\":\"How a bounce would be classified\","
         "\"description\":\"Body: status and diagnostic. A bare enhanced status code (5.1.1) is read as a delivery report's Status field, with diagnostic as its Diagnostic-Code (its smtp; type may be left on) - and a diagnostic that is an SMTP reply is judged as that refusal would be, so a Status a reporting server derived from a bare 550 is not believed either; a reply that begins with its three digits (550 5.1.1 User unknown) is read as a refusal met while delivering, under the rule that a code the remote did not send itself is never believed as a dead mailbox. Answers category (none, hard, soft, mailbox_full, policy, other), suppresses (whether it counts towards the list under the current settings), trusted and status. Reads and writes nothing.\","
         "\"requestBody\":{\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"required\":[\"status\"],\"properties\":{\"status\":{\"type\":\"string\"},\"diagnostic\":{\"type\":\"string\"}}}}}},"
         "\"responses\":{\"200\":{\"description\":\"category, suppresses, trusted, status\"},\"400\":{\"description\":\"No status\"}}}},"
         "\"/api/v1/bounces/expire\":{\"post\":{\"summary\":\"Run the suppression list's maintenance sweep now\","
         "\"description\":\"What the hourly task does: entries whose own expiry has passed or that are older than suppression_expiry_days, counts older than count_retention_days with no entry behind them, and report keys older than report_window_days are removed. Server-wide; refused for domain-restricted and read-only keys.\","
         "\"responses\":{\"200\":{\"description\":\"suppressions_expired, counts_pruned, report_keys_pruned\"},\"500\":{\"description\":\"A sweep failed part way\"}}}}";

      return AnsiString(paths);
   }
}
