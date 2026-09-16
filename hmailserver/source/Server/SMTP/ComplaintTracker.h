// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Complaints, read: the feedback loops.
//
// The large mailbox providers run a feedback loop. When one of their readers
// presses the button that says this is spam, they post an abuse report (RFC 5965,
// a multipart/report whose report-type is feedback-report) to an address the
// sending operator registered with them. It says which of their readers
// complained, about which message, when it arrived, from which IP, and about
// which domain. Until this existed nothing here read one: they arrived in a
// mailbox and sat there.
//
// This reads them. A report that reaches the complaint mailbox is parsed
// (FeedbackReportParser), recorded against the sending domain and the sending
// account, counted where the bounces are counted, and - past a threshold - the
// reader's address is put on that domain's suppression list with its own reason,
// so this server stops mailing somebody who has said twice over that they do not
// want it. A rate per domain is exposed in the metrics and raises an alert when it
// crosses the rule's threshold, because a complaint rate is the number the
// providers themselves throttle and block on, and an operator who learns it from
// the providers learns it too late.
//
// WHICH MAILBOX, AND WHY IT IS A CONFIGURED ONE. The alternative - read any
// message anywhere on the server that carries a message/feedback-report part -
// was rejected, and the reason is in the code rather than in taste:
// BounceTracker's whole gate (AcceptReport, and the register a delivery report
// must reproduce a key from) exists because a message that looks like a report is
// trivially forged, and a report that is believed can stop this installation
// mailing an address. A feedback report has none of the bounce report's own
// evidence: its envelope sender is the provider's, not null, and there is nothing
// in it that only the provider could know - the Message-ID and the recipient of
// the message complained about are known to every other recipient of that message
// and to the reader themselves. So the one thing that distinguishes a report from
// the provider is that it arrived where the provider was told to send it. Making
// every mailbox an intake would let any stranger who can mail one user stop this
// server sending to any address of that user's domain. The mailbox is therefore
// named by the administrator (ComplaintMailbox), empty by default, and with it
// empty nothing here reads, queries or writes anything.
//
// What that does not stop is stated in docs/Complaints.md: somebody who learns
// the complaint address - and abuse@ is a published address by RFC 2142 - can
// forge a report, so the threshold, the rate and the list are all visible and
// every entry says where it came from and can be removed. A complaint counts only
// for a domain this server hosts, so no amount of forged mail can create a row
// for a domain that is not the operator's own.
//
// A COMPLAINT IS NOT A BOUNCE, and the code keeps them apart. A bounce says the
// mailbox may not be there; a complaint proves that it is, and that the person
// behind it read the message and did not want it. So a complaint touches no
// bounce counter and never the run of hard bounces: it has its own counter
// (hm_bouncecounts.bccomplaints), its own date (bclastcomplaint), its own
// threshold, and its own reason on the one suppression list
// (PersistentBounce::SourceComplaint). Everything that enforces that list - the
// refusal at submission, the removal at delivery, the page, the expiry sweep -
// applies to a complaint's entry unchanged, because it is the same list.
//
// The date is a column of its own and not bclastdelivered, which would have been
// the cheap way to write it, because bclastdelivered means "a remote accepted a
// message to this address" and is what BounceTracker::ReportRunIncrease reads to
// decide whether a delivery report arriving after an acceptance still adds to the
// run. Writing a complaint's read time there would have made a genuine hard
// bounce for an earlier message count for nothing - which is exactly the
// "complaints never touch the run" promise, broken by the line that was meant to
// keep it. It is also what the hourly sweep measures a complaint-only row's age
// by, so a row that holds nothing but complaints ages from the last of them.

#pragma once

namespace HM
{
   class Message;
   struct FeedbackReport;

   class ComplaintTracker
   {
   public:
      // -------------------------------------------------------- the hooks

      // SMTPDeliverer, before local delivery, beside the delivery report's hook.
      // The message is delivered unchanged whatever this makes of it.
      static void InspectIncomingComplaint(std::shared_ptr<Message> message);

      // SMTPDeliverer, straight after external delivery: the denominator of the
      // rate. One row per sending domain per day, added to once per pass with the
      // recipients a remote accepted in it - not once per recipient, so a message
      // to a hundred addresses is one round trip and not a hundred.
      //
      // What it costs per message, stated plainly because the shape is easy to
      // under-read: a select of the day's row by its unique index and then an
      // update of it, so TWO statements per outbound message from a hosted sender,
      // both against the same single row for that domain and that day. On a backend
      // that locks coarsely - SQL Server Compact, which is what a small
      // installation and the bench both run - concurrent delivery for one busy
      // domain therefore serialises briefly on that row. It is not one write, and
      // caching the row's id to make it one was considered and rejected: the DAL
      // reports no rows-affected, so a cached id that had gone stale would lose the
      // day's tally silently, and a denominator that is quietly wrong makes every
      // rate above it wrong in the direction that hides a problem.
      //
      // Nothing is written at all while the feedback loops are off or no mailbox is
      // named, which is the shipped state.
      static void RecordDeliveryPass(std::shared_ptr<Message> message);

      // -------------------------------------------------------- the pure parts

      // Whether an address is one of the configured complaint mailboxes. The
      // setting is a list separated by commas, semicolons or spaces, so an operator
      // running several feedback loops into different addresses can name them all.
      static bool IsComplaintMailbox(const String &configured, const String &address);

      // Whether a feedback type is a complaint about this installation's mail.
      // not-spam is the reader saying the opposite and counts towards neither the
      // rate nor the list; every other registered type (abuse, fraud, virus, and
      // anything a provider invents) is a complaint.
      static bool CountsAsComplaint(const String &feedbackType);

      // What makes the same report twice one complaint: a SHA-256 of the message
      // complained about, the reader, the sender and the arrival time. A report
      // that identifies no message at all - no Message-ID and no arrival date,
      // which a heavily redacting provider sends - cannot be recognised as a
      // repeat, so its key takes the fallback (the local message's own id) and it
      // is counted. Losing a real complaint is worse than counting a resent one.
      static String ComplaintKey(const String &messageId, const String &address, const String &sender,
                                 const String &arrival, __int64 fallback);

      // -------------------------------------------------------- the rate

      // The window every rate is measured over. Seven days, which is the period the
      // providers' own postmaster tools report on, and not a setting: a rate
      // measured over a window the operator can change is not the number the
      // provider is looking at.
      static constexpr int RateWindowDays = 7;

      // Below this many messages in the window a domain's rate is not judged - the
      // alert does not fire on it and the metric reports the rate as 0. Three
      // complaints out of ten messages is not a 300,000 ppm problem, it is a domain
      // that has barely sent anything.
      static constexpr int RateMinimumVolume = 100;

      struct Rate
      {
         __int64 domain_id = 0;
         String domain;
         __int64 sent = 0;
         __int64 complaints = 0;
         int parts_per_million = 0;
         bool judged = false;      // sent reached RateMinimumVolume
      };

      // complaints per million messages, rounded to nearest; 0 when nothing was
      // sent. A function of two numbers, so the arithmetic is tested.
      static int PartsPerMillion(__int64 complaints, __int64 sent);

      // Every hosted domain that has sent or been complained about in the window,
      // worst rate first. Read from the database at most once a minute and shared
      // by the metrics endpoint and the alert pass, so a scrape every fifteen
      // seconds does not become a query every fifteen seconds.
      static bool Rates(std::vector<Rate> &rates);

      // The worst judged rate at or above thresholdPpm, as a sentence for the
      // alert. False when none is, which includes the database being unreadable:
      // an alert that cannot be evaluated is not raised.
      static bool WorstRate(int thresholdPpm, String &summary);

      // -------------------------------------------------------- administration

      // The documents the REST routes answer and the COM methods return, built in
      // one place so every surface reads the same thing. UTF-8.
      static bool ComplaintsJson(__int64 domainID, __int64 accountID, const String &search, int limit, int offset,
                                 AnsiString &json);
      static AnsiString RatesJson();

      // What the maintenance sweep removed. False when it failed.
      static bool RunMaintenance(int &complaintsPruned);

   private:
      static bool Enabled_();
      static String Mailbox_();
      static __int64 AccountIDFor_(const String &address);
      static void ReportTallyUnwritable_(const String &detail);
      static void InvalidateRates_();
   };

   // Run from ClassTester::DoTests, which the regression suite reaches through
   // Utilities.RunTestSuite (Infrastructure.MainOperations.TestInternals).
   class ComplaintTrackerTester
   {
   public:
      void Test();
   };
}
