// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The reports our own domains receive, read.
//
// This server has sent DMARC aggregate reports (DmarcRptReporterTask) and
// TLS-RPT reports (TlsRptReporterTask) to other people's domains for a year,
// and never read the ones that came back for its own: they landed in whatever
// mailbox the rua= tag named and sat there as gzip attachments nobody opened.
// The paid services - dmarcian, Postmark's tool, Valimail - exist to open
// them, and what they show is one chart: who sends as your domain, how much,
// and whether it passed.
//
// The intake is a mailbox per domain: Domain::GetReportMailbox, the address
// the domain's rua= tags name, and the ONE address of that domain's a report
// is read from. A message delivered to that address and to nothing else of
// this server's is opened; its attachment - gzip, zip or plain, XML or JSON -
// is unwrapped; a DMARC aggregate report (RFC 7489 Appendix C, and the RFC 9990
// form, which differs only in a namespace and a few optional elements) is
// recorded as one row in hm_dmarcreports and one in hm_dmarcreportrecords per
// <record>; a TLS-RPT report (RFC 8460) as one row in hm_tlsreports per policy.
// The report is recorded against the domain it says it is about, which must be
// one this server hosts AND one that names a report mailbox of its own (an
// organisation with several domains names the same mailbox on each); the
// message itself is delivered unchanged either way, as a complaint is, because
// a report this server cannot read is still mail.
//
// Why a configured mailbox and not any mailbox: a report is XML from a stranger.
// Nothing in it proves it came from the organisation it names, so what
// distinguishes a report from a forgery is that it arrived where the domain
// owner told the world to send reports - and that address is published in DNS,
// so it is not a secret, and a forged report can only ever add rows to a chart
// about the forger's own claims. The rows are read by an administrator and
// act on nothing: no list is fed, no mail is refused.
//
// Safety, since every byte here is hostile by construction: the message is not
// read past 32 MB, at most ten of its parts are looked at, an attachment is not
// inflated past 16 MB, a zip's central directory is what is trusted for sizes,
// the XML reader knows no entities but the five predefined ones and refuses a
// document type declaration that declares any, nesting and node counts are
// capped, a report is at most 2,000 records and a message may add no more than
// that, and a domain takes in at most 20,000 records and 1,000 TLS rows in any
// twenty-four hours - past which a report is refused with a log line, because
// the mailbox address is published and anybody may post to it, and the rows
// are what an administrator reads before tightening p=. The message is opened
// only when it is addressed to the mailbox and nothing else, and what the
// inbound path made of it - the dmarc= verdict in the Authentication-Results
// field this server wrote, when AuthenticationResultsEnabled is on - is kept
// on the report row, so a reporter whose messages fail DMARC can be seen for
// what it is.

#pragma once

#include <memory>
#include <vector>

namespace HM
{
   class Message;

   class DomainReports
   {
   public:

      // One <record> of an aggregate report: the source, how many messages,
      // what the receiver did and the two aligned verdicts, the identities,
      // and the raw authentication results as "domain:selector:result" pairs.
      struct DmarcRecord
      {
         DmarcRecord() : count(0) { }

         AnsiString source_ip;
         int count;
         AnsiString disposition;      // none / quarantine / reject / pass (9990)
         AnsiString dkim;             // aligned: pass / fail
         AnsiString spf;              // aligned: pass / fail
         AnsiString header_from;
         AnsiString envelope_from;
         AnsiString dkim_auth;        // "d=example.com s=sel result=pass; ..."
         AnsiString spf_auth;         // "d=example.com result=pass"
      };

      struct DmarcReport
      {
         DmarcReport() : pct(100), begin(0), end(0), dropped_records(0) { }

         AnsiString org_name;
         AnsiString email;
         AnsiString report_id;
         AnsiString policy_domain;
         AnsiString adkim;
         AnsiString aspf;
         AnsiString p;
         AnsiString sp;
         int pct;
         __int64 begin;
         __int64 end;
         std::vector<DmarcRecord> records;
         int dropped_records;         // past the cap, counted and not kept
      };

      // One policy of a TLS-RPT report: what the sending side found when it
      // tried to reach the policy domain under that policy.
      struct TlsPolicyResult
      {
         TlsPolicyResult() : successful(0), failed(0) { }

         AnsiString policy_type;      // tlsa / sts / no-policy-found
         AnsiString policy_domain;
         int successful;
         int failed;
         AnsiString failures_json;    // [{result_type, sending_mta_ip, receiving_mx_hostname, receiving_ip, count}]
      };

      struct TlsReport
      {
         TlsReport() : begin(0), end(0) { }

         AnsiString org_name;
         AnsiString contact;
         AnsiString report_id;
         __int64 begin;
         __int64 end;
         std::vector<TlsPolicyResult> policies;
      };

      // One thing found in a message that could be a report: its bytes as
      // decoded from the MIME part, and the name the part gave it.
      struct Candidate
      {
         AnsiString bytes;
         AnsiString name;
      };

      // --- the delivery path ------------------------------------------------

      // Reads the message when it is addressed to a hosted domain's report
      // mailbox and to nothing else, and records what it carries. Returns at
      // once for every other message. The message is never changed.
      static void InspectIncomingReport(std::shared_ptr<Message> message);

      // The set of report mailboxes is cached; a domain save calls this so
      // that a mailbox named now reads the next message.
      static void ForgetIntakes();

      // --- what the routes answer -------------------------------------------

      // The chart's data: who sent as the domain over the last `days` days,
      // per source and per day, with the totals and the reporters.
      static bool SummaryJson(__int64 domainID, int days, AnsiString &json);

      // The reports themselves, newest first, a page at a time.
      static bool ReportsJson(__int64 domainID, int limit, int offset, AnsiString &json);

      // One report with its records. Answers false with found=false when the
      // id is not one of this domain's.
      static bool ReportJson(__int64 domainID, __int64 reportID, bool &found, AnsiString &json);

      // The TLS-RPT reports over the window, with the session totals.
      static bool TlsReportsJson(__int64 domainID, int days, int limit, int offset, AnsiString &json);

      // Removes reports read more than olderThanDays ago (0 removes every one),
      // in passes of MaxPrunePerPass until a pass finds nothing or MaxPrunePasses
      // have run; complete says which. Returns how many reports of each kind
      // went, or -1 when a delete failed.
      static int Prune(int olderThanDays, int &dmarcPruned, int &tlsPruned, bool &complete);

      // The sweep the retention task runs, on DomainReportRetentionDays.
      static void Sweep();

      // --- the readers, public for the class tester --------------------------

      static bool ParseDmarcXml(const AnsiString &xml, DmarcReport &report, AnsiString &problem);
      static bool ParseTlsJson(const AnsiString &json, TlsReport &report, AnsiString &problem);

      // gzip, zip or neither, told from the bytes rather than the name.
      static bool Unwrap(const AnsiString &bytes, AnsiString &content, AnsiString &problem);

      // Every part of the message that could be a report, in order.
      static bool FindCandidates(const AnsiString &rawMessage, std::vector<Candidate> &candidates);

      // yyyy-mm-dd, UTC, of a moment in seconds since 1970.
      static AnsiString DayOf(__int64 seconds);

      // Seconds since 1970 of an RFC 3339 / ISO 8601 date-time such as
      // 2026-09-17T00:00:00Z; -1 when it cannot be read.
      static __int64 ParseDateTime(const AnsiString &text);

      // The key that makes the same report read twice one row.
      static String DmarcKey(const AnsiString &orgName, const AnsiString &reportId, const AnsiString &policyDomain);
      static String TlsKey(const AnsiString &orgName, const AnsiString &reportId, const AnsiString &policyDomain, const AnsiString &policyType);

      // Whether a policy domain a reporter named is a host name - letters,
      // digits, dots and hyphens, at most 253 of them, no empty label - and so
      // safe to look up. A report naming anything else is refused before any
      // lookup, because the cache asserts on an address where a name should be.
      static bool IsHostName(const AnsiString &name);

      // What this server's own inbound path made of the message that carried a
      // report: the dmarc= verdict and the header.from= domain of the first
      // Authentication-Results field bearing authservId, from the raw message's
      // header block. Both come back empty when the field is not there - which
      // is the case unless AuthenticationResultsEnabled is on - or carries no
      // dmarc= method. Public for the class tester, which hands it the id.
      static void ReadMessageDmarc(const AnsiString &rawMessage, const AnsiString &authservId, AnsiString &result, AnsiString &fromDomain);

      // A report keeps at most this many records; the rest are counted and
      // logged. A message may add no more than this across every report it
      // carries, and a domain no more than the daily ceilings in any twenty-four
      // hours; a report past either is refused whole and the log says so.
      static const int MaxRecordsPerReport = 2000;
      static const int MaxRecordsPerMessage = 2000;
      static const int MaxRecordsPerDomainPerDay = 20000;
      static const int MaxTlsRowsPerDomainPerDay = 1000;
      static const int MaxCandidatesPerMessage = 10;
      static const int MaxPrunePerPass = 5000;
      static const int MaxPrunePasses = 100;

   private:

      static bool IntakeDomainFor_(const String &address, __int64 &domainID);
      static bool ReadIntakes_(std::vector<std::pair<String, __int64> > &intakes);
      static bool ReadFile_(const String &fileName, AnsiString &raw);

      static int RecordDmarc_(__int64 domainID, const DmarcReport &report, const AnsiString &source,
                              const AnsiString &messageDmarc, const AnsiString &messageFrom, __int64 &reportRowID);
      static int RecordTls_(__int64 domainID, const TlsReport &report, const TlsPolicyResult &policy, const AnsiString &source);
      static __int64 HostedDomainID_(const AnsiString &domainName);

      // What the domain has taken in over the last twenty-four hours, for the
      // ceilings: records of aggregate reports and rows of TLS reports. False
      // when the database did not answer, in which case nothing is recorded.
      static bool TakenInLastDay_(__int64 domainID, __int64 &dmarcRecords, __int64 &tlsRows);

      // Whether to say that a domain's ceiling was hit: once an hour per domain,
      // so a flood is one line an hour and not one a message.
      static bool ShouldLogCeiling_(__int64 domainID);
   };

   class DomainReportsTester
   {
   public:
      void Test();
   };
}
