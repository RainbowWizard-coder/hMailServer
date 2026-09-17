// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The reports our own domains receive, read (DomainReports.h), over REST:
//
//   GET  /api/v1/domains/{domain}/dmarc-reports/summary?days=30   who sends as the domain, with what result
//   GET  /api/v1/domains/{domain}/dmarc-reports?limit&offset       the aggregate reports, newest first
//   GET  /api/v1/domains/{domain}/dmarc-reports/{id}               one report with its records
//   GET  /api/v1/domains/{domain}/tls-reports?days&limit&offset    the TLS-RPT reports over the window
//   POST /api/v1/domain-reports/expire                             the retention sweep, now
//
// The four under a domain are scoped to it in Authorize_ as the complaints
// are; the sweep prunes every domain's rows and is server-wide. The mailbox
// the reports arrive at is the domain's report_mailbox, a field of the domain
// itself, and is in these documents because it is the address the domain
// publishes in DNS for the world to send to - a report is not a secret and
// neither is where it goes.

#include "StdAfx.h"

#include "RestApiServer.h"
#include "DomainReports.h"
#include "JsonDocument.h"
#include "Unicode.h"
#include "../BO/Domain.h"
#include "../Cache/CacheContainer.h"

#include <climits>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
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

      __int64 DomainIdOf(const AnsiString &identifier)
      {
         String domainName;
         Unicode::MultiByteToWide(identifier, domainName);
         domainName.Trim();

         // Not a host name is not a domain: the cache asserts on an address
         // where a name should be, and the answer for one is the same 404.
         if (domainName.IsEmpty() || !DomainReports::IsHostName(AnsiString(identifier)))
            return 0;

         std::shared_ptr<const Domain> domain = CacheContainer::Instance()->GetDomain(domainName);
         return domain ? domain->GetID() : 0;
      }
   }

   HttpResponse
   RestApiServer::HandleDomainReports_(const Route &route, const AnsiString &requestBody)
   {
      if (route.kind == RouteDomainReportsExpire)
      {
         // The setting's retention, or the body's older_than_days when it names
         // one - 0 removes every report, which is what a test of the sweep and
         // an operator clearing a domain's history both need.
         int olderThanDays = Configuration::Instance()->GetDomainReportRetentionDays();
         bool explicitDays = false;

         if (!requestBody.IsEmpty())
         {
            JsonValue body;
            std::string error;

            if (!JsonValue::Parse(std::string(requestBody), body, error) || !body.IsObject())
               return BuildResponse_(400, "{\"error\":\"the body must be a JSON object\"}");

            const JsonValue *days = body.Get("older_than_days");
            if (days && !days->IsNull())
            {
               if (!days->IsNumber() || days->AsNumber() < 0 || days->AsNumber() > 3650 || days->AsNumber() != (double) days->AsInt64())
                  return BuildResponse_(400, "{\"error\":\"older_than_days is a whole number of days, 0 to 3650\"}");

               olderThanDays = (int) days->AsInt64();
               explicitDays = true;
            }
         }

         if (!explicitDays && olderThanDays <= 0)
            return BuildResponse_(200, "{\"dmarc_reports_pruned\":0,\"tls_reports_pruned\":0,\"kept_for_ever\":true}");

         int dmarcPruned = 0, tlsPruned = 0;
         bool complete = true;
         if (DomainReports::Prune(olderThanDays, dmarcPruned, tlsPruned, complete) < 0)
            return BuildResponse_(500, "{\"error\":\"the sweep failed part way; what it removed before failing has been removed\"}");

         LOG_APPLICATION(Formatter::Format(_T("RestApi: the domain reports' sweep removed {0} DMARC aggregate report(s) and {1} TLS-RPT report(s) read more than {2} days ago{3}."),
            dmarcPruned, tlsPruned, olderThanDays, String(complete ? _T("") : _T("; more remain, and another run removes them"))));

         AnsiString body;
         body.Format("{\"dmarc_reports_pruned\":%d,\"tls_reports_pruned\":%d,\"older_than_days\":%d,\"complete\":%s}",
            dmarcPruned, tlsPruned, olderThanDays, complete ? "true" : "false");
         return BuildResponse_(200, body);
      }

      const __int64 domainID = DomainIdOf(route.identifier);
      if (domainID <= 0)
         return BuildResponse_(404, "{\"error\":\"domain not found\"}");

      AnsiString body;

      switch (route.kind)
      {
      case RouteDomainReportsSummary:
         {
            const int days = ParseCount(QueryParameter_(route.query, "days"), 30, 1, 3650);

            if (!DomainReports::SummaryJson(domainID, days, body))
               return BuildResponse_(500, "{\"error\":\"the reports could not be read\"}");

            return BuildResponse_(200, body);
         }

      case RouteDomainReportsList:
         {
            const int limit = ParseCount(QueryParameter_(route.query, "limit"), 50, 1, 1000);
            const int offset = ParseCount(QueryParameter_(route.query, "offset"), 0, 0, INT_MAX);

            if (!DomainReports::ReportsJson(domainID, limit, offset, body))
               return BuildResponse_(500, "{\"error\":\"the reports could not be read\"}");

            return BuildResponse_(200, body);
         }

      case RouteDomainReportGet:
         {
            bool found = false;

            if (!DomainReports::ReportJson(domainID, route.record_id, found, body))
               return BuildResponse_(500, "{\"error\":\"the report could not be read\"}");

            if (!found)
               return BuildResponse_(404, "{\"error\":\"not one of this domain's reports\"}");

            return BuildResponse_(200, body);
         }

      case RouteDomainTlsReportsList:
         {
            const int days = ParseCount(QueryParameter_(route.query, "days"), 30, 0, 3650);
            const int limit = ParseCount(QueryParameter_(route.query, "limit"), 50, 1, 1000);
            const int offset = ParseCount(QueryParameter_(route.query, "offset"), 0, 0, INT_MAX);

            if (!DomainReports::TlsReportsJson(domainID, days, limit, offset, body))
               return BuildResponse_(500, "{\"error\":\"the reports could not be read\"}");

            return BuildResponse_(200, body);
         }

      default:
         break;
      }

      return BuildResponse_(404, "{\"error\":\"not found\"}");
   }

   AnsiString
   RestApiServer::OpenApiDomainReportsPaths_()
   {
      static const char *paths =
         ",\"/api/v1/domains/{domain}/dmarc-reports/summary\":{"
         "\"get\":{\"summary\":\"Who sends as a domain, and with what result: the DMARC aggregate reports read from its report mailbox, charted\","
         "\"description\":\"What the paid DMARC services draw, from the aggregate reports (RFC 7489, RFC 9990) that the receivers of the domain's mail posted to its report_mailbox. Over the last days days (default 30, at most 3650): messages, passed (DMARC passed: aligned DKIM or aligned SPF), failed, dkim_passed, spf_passed, quarantined and rejected (what the receivers did), reports (how many were read), reporters (org, reports, messages, and checked and authenticated - how many of its reports arrived in a message this server evaluated for DMARC, and how many of those passed - who sent them), by_day (day, messages, passed, failed; every day of the window when it is 400 days or fewer, so a day nothing arrived on is a gap in the chart and not a missing bar) and sources (ip, messages, passed, failed, dkim_passed, spf_passed, quarantined, rejected, header_from, reporters - the addresses that sent as the domain, most messages first, at most 200; sources_total says how many there were). A report counts when any of its range is inside the window and its records are charted on the day it begins, which for the daily reports the large receivers send is the day. report_mailbox and mailbox_configured say where the reports arrive and whether anything is being read: with no mailbox named, the page has nothing to draw and says so. Nothing here is a secret - the mailbox is what the domain publishes in DNS - and nothing here acts: no list is fed and no mail is refused. Scoped to the domain for a domain-restricted key.\","
         "\"parameters\":[{\"name\":\"domain\",\"in\":\"path\",\"required\":true,\"schema\":{\"type\":\"string\"}},"
         "{\"name\":\"days\",\"in\":\"query\",\"schema\":{\"type\":\"integer\"},\"description\":\"The window, in days back from now. 1 to 3650; default 30.\"}],"
         "\"responses\":{\"200\":{\"description\":\"domain, days, since, until, report_mailbox, mailbox_configured, reports, messages, passed, failed, dkim_passed, spf_passed, quarantined, rejected, reporters, by_day, sources and sources_total\"},\"404\":{\"description\":\"Unknown domain\"}}}},"
         "\"/api/v1/domains/{domain}/dmarc-reports\":{"
         "\"get\":{\"summary\":\"The DMARC aggregate reports read for a domain\","
         "\"description\":\"Newest first, a page at a time. Each item: id, org (the reporter), email (its contact), report_id, begin and end (the range the report covers, seconds since 1970), policy_domain, adkim, aspf, p, sp and pct (the policy the reporter saw published), messages and passed (the records added up), records (how many were kept; a report holds at most 2,000 and the rest are counted in the log), received (when this server read it), source (the attachment it was read from), and message_dmarc and message_from - what this server's own inbound path made of the message that carried the report, the dmarc= verdict and the header.from= domain of the Authentication-Results field it wrote, empty unless AuthenticationResultsEnabled is on - so a reporter whose messages fail DMARC can be told from one whose pass. The same report delivered twice is one item. Scoped to the domain for a domain-restricted key.\","
         "\"parameters\":[{\"name\":\"domain\",\"in\":\"path\",\"required\":true,\"schema\":{\"type\":\"string\"}},"
         "{\"name\":\"limit\",\"in\":\"query\",\"schema\":{\"type\":\"integer\"},\"description\":\"Items to return, 1 to 1000. Default 50.\"},"
         "{\"name\":\"offset\",\"in\":\"query\",\"schema\":{\"type\":\"integer\"},\"description\":\"Items to skip.\"}],"
         "\"responses\":{\"200\":{\"description\":\"domain, report_mailbox, mailbox_configured, total, offset and items\"},\"404\":{\"description\":\"Unknown domain\"}}}},"
         "\"/api/v1/domains/{domain}/dmarc-reports/{id}\":{"
         "\"get\":{\"summary\":\"One DMARC aggregate report with its records\","
         "\"description\":\"The item as the listing shows it, and records: one per source and verdict the reporter counted - source_ip, count, disposition (none, quarantine, reject, or pass in the RFC 9990 form), dkim and spf (the aligned verdicts), header_from, envelope_from, and dkim_auth and spf_auth (the raw authentication results, as d=domain s=selector result=pass pairs). Scoped to the domain for a domain-restricted key.\","
         "\"parameters\":[{\"name\":\"domain\",\"in\":\"path\",\"required\":true,\"schema\":{\"type\":\"string\"}},{\"name\":\"id\",\"in\":\"path\",\"required\":true,\"schema\":{\"type\":\"integer\"}}],"
         "\"responses\":{\"200\":{\"description\":\"The report and its records\"},\"404\":{\"description\":\"Unknown domain, or not one of its reports\"}}}},"
         "\"/api/v1/domains/{domain}/tls-reports\":{"
         "\"get\":{\"summary\":\"The TLS-RPT reports read for a domain\","
         "\"description\":\"What the senders that tried to reach the domain over TLS reported (RFC 8460), one item per policy of each report, newest first: id, org, contact, report_id, begin, end, policy_type (sts, tlsa or no-policy-found), policy_domain, successful and failed (sessions), failures (result_type, sending_mta_ip, receiving_mx_hostname, receiving_ip, count - as far as the row holds; a report with hundreds of distinct failures is cut short at the row's size), received and source. sessions_succeeded and sessions_failed add up every item of the window, not only the page. Scoped to the domain for a domain-restricted key.\","
         "\"parameters\":[{\"name\":\"domain\",\"in\":\"path\",\"required\":true,\"schema\":{\"type\":\"string\"}},"
         "{\"name\":\"days\",\"in\":\"query\",\"schema\":{\"type\":\"integer\"},\"description\":\"The window, in days back from now. 0 is every report kept; default 30.\"},"
         "{\"name\":\"limit\",\"in\":\"query\",\"schema\":{\"type\":\"integer\"},\"description\":\"Items to return, 1 to 1000. Default 50.\"},"
         "{\"name\":\"offset\",\"in\":\"query\",\"schema\":{\"type\":\"integer\"},\"description\":\"Items to skip.\"}],"
         "\"responses\":{\"200\":{\"description\":\"domain, days, since, mailbox_configured, sessions_succeeded, sessions_failed, total, offset and items\"},\"404\":{\"description\":\"Unknown domain\"}}}},"
         "\"/api/v1/domain-reports/expire\":{\"post\":{\"summary\":\"Run the domain reports' retention sweep now\","
         "\"description\":\"What the six-hourly retention task does: DMARC aggregate and TLS-RPT reports read more than domain_report_retention_days ago are removed, a report's records with it, in passes of 5,000 of each kind until a pass finds nothing or a hundred have run; complete is false when the hundredth pass still found rows, and another call removes the rest. Body, optional: older_than_days (0 to 3650) to use instead of the setting - 0 removes every report of every domain. With the setting at 0 and no body, nothing is removed and kept_for_ever says so. Server-wide; refused for domain-restricted and read-only keys.\","
         "\"requestBody\":{\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"properties\":{\"older_than_days\":{\"type\":\"integer\"}}}}}},"
         "\"responses\":{\"200\":{\"description\":\"dmarc_reports_pruned, tls_reports_pruned, older_than_days and complete\"},\"400\":{\"description\":\"older_than_days is not a whole number of days\"},\"500\":{\"description\":\"The sweep failed part way\"}}}}";

      return AnsiString(paths);
   }
}
