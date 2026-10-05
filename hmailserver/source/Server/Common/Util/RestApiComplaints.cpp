// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Complaints, read: the feedback loops, over REST.
//
//   GET    /api/v1/domains/{domain}/complaints   the reports about a domain's mail
//   GET    /api/v1/complaints/rates              every domain's rate over the window
//   POST   /api/v1/complaints/expire             the hourly sweep, now
//
// The settings group, /api/v1/settings/complaints, is a table in RestApiSettings.cpp
// like every other group. The domain route is scoped to its domain in Authorize_,
// so a key issued for one domain reads that domain's complaints and no other's; the
// rates and the sweep are server-wide and refused to such a key.
//
// The documents are built by ComplaintTracker, which is also what the COM methods
// the desktop Control Panel calls return, so the two surfaces cannot drift apart.

#include "StdAfx.h"

#include "RestApiServer.h"

#include "Unicode.h"
#include "../../SMTP/BounceTracker.h"
#include "../../SMTP/ComplaintTracker.h"

#include <climits>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // A path segment as the client sent it, with %XX decoded, so that a domain
      // written with an escape names the same domain.
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
   RestApiServer::HandleComplaints_(const Route &route, const AnsiString &)
   {
      switch (route.kind)
      {
      case RouteComplaintRates:
         {
            const AnsiString body = ComplaintTracker::RatesJson();

            if (body.IsEmpty())
               return BuildResponse_(500, "{\"error\":\"the complaint rates could not be read\"}");

            return BuildResponse_(200, body);
         }

      case RouteComplaintExpire:
         {
            int complaintsPruned = 0;

            if (!ComplaintTracker::RunMaintenance(complaintsPruned))
               return BuildResponse_(500, "{\"error\":\"the maintenance sweep failed part way; what it removed before failing has been removed\"}");

            LOG_APPLICATION(Formatter::Format(_T("RestApi: the feedback loops' maintenance sweep removed {0} old complaint(s) and tally row(s)."),
               complaintsPruned));

            AnsiString body;
            body.Format("{\"complaints_pruned\":%d}", complaintsPruned);

            return BuildResponse_(200, body);
         }

      default:
         break;
      }

      if (route.kind != RouteComplaintList)
         return BuildResponse_(404, "{\"error\":\"not found\"}");

      String domainName;
      Unicode::MultiByteToWide(DecodePathSegment(route.identifier), domainName);

      const __int64 domainID = BounceTracker::DomainIDByName(domainName);
      if (domainID <= 0)
         return BuildResponse_(404, "{\"error\":\"domain not found\"}");

      String search;
      Unicode::MultiByteToWide(QueryParameter_(route.query, "search"), search);

      const AnsiString accountText = QueryParameter_(route.query, "account_id");
      const __int64 accountID = accountText.IsEmpty() ? 0 : _atoi64(accountText.c_str());

      const int limit = ParseCount(QueryParameter_(route.query, "limit"), 100, 1, 1000);
      const int offset = ParseCount(QueryParameter_(route.query, "offset"), 0, 0, INT_MAX);

      AnsiString body;
      if (!ComplaintTracker::ComplaintsJson(domainID, accountID > 0 ? accountID : 0, search, limit, offset, body))
         return BuildResponse_(500, "{\"error\":\"the complaints could not be read\"}");

      return BuildResponse_(200, body);
   }

   AnsiString
   RestApiServer::OpenApiComplaintsPaths_()
   {
      static const char *paths =
         ",\"/api/v1/domains/{domain}/complaints\":{"
         "\"get\":{\"summary\":\"The abuse reports recorded about a domain's mail\","
         "\"description\":\"What the large providers' feedback loops have reported about this domain: who complained, about which message, when it arrived, from which source IP and which provider reported it. Newest first. Each item: address (the reader who complained, empty when the provider redacted it), sender (the address here the message came from), account_id (0 when the sender is not an account), feedback_type (abuse, fraud, virus, not-spam, other), counts (false for not-spam, which is the reader saying the opposite and counts towards nothing), provider, source_ip, reported_domain, message_id, subject, arrival and received. The document also carries the domain's own rate over the window - sent, complaints, rate_ppm and rate_judged, which is false for a domain that has sent too little for a rate to mean anything - and processing_enabled and mailbox_configured, because a page that shows the rows without saying whether anything is being read shows half the answer. The mailbox ADDRESS is deliberately not here: a key confined to one domain reads this document, and knowing the address the feedback loops post to is the only thing that lets a report be forged, so it stays in the server-wide settings group. Scoped to the domain for a domain-restricted key.\","
         "\"parameters\":[{\"name\":\"domain\",\"in\":\"path\",\"required\":true,\"schema\":{\"type\":\"string\"}},"
         "{\"name\":\"search\",\"in\":\"query\",\"schema\":{\"type\":\"string\"},\"description\":\"A substring of the complained-of address, the sender, the subject or the provider.\"},"
         "{\"name\":\"account_id\",\"in\":\"query\",\"schema\":{\"type\":\"integer\"},\"description\":\"Only the complaints about mail from this account.\"},"
         "{\"name\":\"limit\",\"in\":\"query\",\"schema\":{\"type\":\"integer\"},\"description\":\"Items to return, 1 to 1000. Default 100.\"},"
         "{\"name\":\"offset\",\"in\":\"query\",\"schema\":{\"type\":\"integer\"},\"description\":\"Items to skip.\"}],"
         "\"responses\":{\"200\":{\"description\":\"domain, processing_enabled, mailbox_configured, window_days, sent, complaints, rate_ppm, rate_judged, total, offset and items\"},\"404\":{\"description\":\"Unknown domain\"}}}},"
         "\"/api/v1/complaints/rates\":{\"get\":{\"summary\":\"Every domain's complaint rate\","
         "\"description\":\"The complaint rate of each locally hosted domain over the last seven days, worst first: sent (messages a remote accepted), complaints, rate_ppm (complaints in a million messages) and judged, which is false for a domain that sent fewer than minimum_volume in the window and whose rate is reported as 0 because it would not mean anything. This is the number the large providers throttle and block on; 1000 parts per million is one complaint in a thousand, which they describe as too high, and is what the complaints.rate.high alert rule is seeded with. Read from the database at most once a minute and shared with the metrics endpoint. Server-wide; refused for domain-restricted keys.\","
         "\"responses\":{\"200\":{\"description\":\"processing_enabled, mailbox, window_days, minimum_volume and domains\"},\"500\":{\"description\":\"The tally could not be read\"}}}},"
         "\"/api/v1/complaints/expire\":{\"post\":{\"summary\":\"Run the feedback loops' maintenance sweep now\","
         "\"description\":\"What the hourly task does: complaints older than retention_days are removed, and so are the daily tally rows behind the rate - those on retention_days or the seven days of the rate window, whichever is longer, so a short retention cannot empty the window the rate is measured over. A suppression list entry a complaint made is not touched - it has its own expiry, and /api/v1/bounces/expire is its sweep. Server-wide; refused for domain-restricted and read-only keys.\","
         "\"responses\":{\"200\":{\"description\":\"complaints_pruned\"},\"500\":{\"description\":\"The sweep failed part way\"}}}}";

      return AnsiString(paths);
   }
}
