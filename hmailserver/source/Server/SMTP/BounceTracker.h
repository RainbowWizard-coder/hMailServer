// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Bounces read, and a suppression list.
//
// The server has always retried and given up; what it never did was read what
// came back. This is the reading: a bounce classified against RFC 3463
// (BounceClassifier), counted per address and per sending domain, and an address
// that has hard-bounced past a threshold put on that domain's suppression list,
// which the next send is checked against. Per domain, visible, removable by hand,
// and off by default.
//
// TWO WAYS A BOUNCE ARRIVES, and one choke point for each, both in SMTPDeliverer:
//
//   1. A remote server refuses while this server delivers. Every outcome is read
//      in RecordDeliveryPass, after ExternalDelivery has run, from the recipient
//      objects themselves - which are the message's own, and keep their result,
//      their enhanced code and the remote's words. Nothing in ExternalDelivery or
//      SMTPClientConnection is changed to make that possible.
//
//   2. An RFC 3464 report arrives afterwards for one of this installation's
//      senders. InspectIncomingReport reads it before local delivery, and believes
//      it only when it passes the checks below AND names a message this server
//      really sent: when a remote accepted a message, RecordDeliveryPass wrote a
//      SHA-256 of its Message-ID, the recipient and the envelope sender into
//      hm_bouncereportkeys, and a report that does not reproduce one of those
//      keys is ignored. The key is consumed when it matches, so the same report
//      twice counts once. The report itself is always delivered, unchanged.
//      Because the report for a message follows its acceptance, the acceptance
//      does not end the address's run of hard bounces: it sets the run aside in
//      the same register row, and the report gives it back (ReportRunIncrease),
//      so a threshold above one is reachable through reports as well.
//
// FORGED BACKSCATTER must not suppress an address. A report is considered only
// when its envelope sender is null, it is addressed to one of this server's
// addresses and no more, it was not classified as spam, it arrived over an
// unauthenticated SMTP session this server recorded in its own topmost Received
// field (which excludes this server's own non-delivery reports, which carry none,
// and anything a local user submitted), the Authentication-Results
// this server wrote - where it writes them - do not say SPF or DMARC failed, and,
// when BATV is on, it was addressed to a prvs return path that verifies. What
// that does not stop is stated in docs/BounceSuppression.md: a co-recipient who
// saw the message's Message-ID can forge a report for another visible recipient.
//
// THE LIST IS ENFORCED twice. Synchronously, for an SMTP submission and for the
// webmail's send route, where a refusal can still be shown to the person who
// typed the address (RefuseAtSubmission). And at delivery, for everything else -
// an alias that expands to a listed address, a forward, a rule, a scheduled send
// - in ApplyAtDelivery, which is the one decision every path meets.
//
// With BounceProcessingEnabled off, every entry point reads one in-memory setting
// and returns: nothing is read, queried or written.

#pragma once

#include "BounceClassifier.h"
#include "DeliveryFailure.h"

namespace HM
{
   class Message;
   struct BounceSuppressionEntry;
   struct BounceReportKeyRow;
   struct DeliveryReport;

   // The facts a report is judged on, gathered once and judged as values so that
   // the judgement is testable without a delivery (BounceReportGateTester).
   struct BounceReportFacts
   {
      bool envelope_sender_empty = false;
      bool spam_flagged = false;
      bool received_by_this_server = false;
      bool received_authenticated = false;
      bool auth_results_spf_fail = false;
      bool auth_results_dmarc_fail = false;
      bool batv_enabled = false;
      bool recipient_is_verified_prvs = false;
   };

   class BounceTracker
   {
   public:
      // BounceSuppressionAction.
      enum Action
      {
         ActionRefuse = 0,
         ActionDrop = 1,
         ActionObserve = 2
      };

      static const char *ActionName(int action);

      // -------------------------------------------------------- the hooks

      // SMTPDeliverer, before local delivery.
      static void InspectIncomingReport(std::shared_ptr<Message> message);

      // SMTPDeliverer, after local delivery and before external delivery. Removes
      // listed recipients from the message as BounceSuppressionAction says, adding
      // a failure record for each that is refused so the sender's non-delivery
      // report names it.
      static void ApplyAtDelivery(std::shared_ptr<Message> message, const String &sendersIP,
                                  std::vector<DeliveryFailure> &failures);

      // SMTPDeliverer, straight after external delivery.
      static void RecordDeliveryPass(std::shared_ptr<Message> message, bool messageRescheduled);

      // SMTP RCPT TO on a submission, and the webmail's send route. True when the
      // recipient is listed for the sender's domain and the action is refuse; code
      // and text are then the reply to give, and source the entry's (bounce,
      // report or manual). False on any database failure, which sends the mail as
      // if the address were not listed.
      static bool RefuseAtSubmission(const String &sender, const String &recipient, String &enhancedCode, String &text);
      static bool RefuseAtSubmission(const String &sender, const String &recipient, String &enhancedCode, String &text, String &source);

      // -------------------------------------------------------- the pure parts

      static bool AcceptReport(const BounceReportFacts &facts, String &reason);

      // SHA-256 hex of the normalised Message-ID, the recipient and the envelope
      // sender, each lower-cased, separated by line feeds.
      static String ReportKey(const String &messageId, const String &recipient, const String &sender);

      // The session-level guard. A refusal repeated word for word for more than one
      // recipient of one message, by the same host, is treated as the session being
      // refused and not those mailboxes: it is never counted as a dead address.
      // Without the stage the reply was given at, which SMTPClientConnection does
      // not record, it also demotes per-recipient refusals that happen to be worded
      // identically - Microsoft 365 answers every unknown recipient with the same
      // address-free line - so two dead recipients at one such host in one message
      // are counted as other. That errs towards not listing, which is the safe way.
      static BounceClassifier::Verdict ApplySessionGuard(const BounceClassifier::Verdict &verdict, int identicalRefusals);

      // THE RUN, when bounces come back as reports. A remote accepting a message is
      // not yet a delivery: the report that it failed may still come. So an
      // acceptance takes the address's run of hard bounces off the count row and
      // keeps it in the register row it writes, and a report that matches that row
      // gives it back. What a report adds to the run now:
      //   * a later acceptance of the same address is still in the register
      //     (laterKey 1): nothing now - what this report would add, setAside plus
      //     increment, is carried forward onto that later row, and is given back
      //     only if the later message fails as well;
      //   * whether there is one could not be read (laterKey -1): nothing;
      //   * an acceptance the register does not hold - a message without a
      //     Message-ID, or one delivered while reading reports was off - was
      //     recorded after this message was sent (lastDelivered later than sent):
      //     nothing, because that message was delivered after this one;
      //   * otherwise setAside plus increment, increment being 1 for a bounce that
      //     counts towards the list and 0 for one that does not (a policy refusal
      //     reported is not a delivery either, so what was set aside comes back).
      // Every case that cannot be decided adds nothing: the run can come out short
      // - reports that arrive for two messages in the reverse of the order they
      // were sent in. It can come out long by that one report only when an address
      // with no count row yet was sent a message the register holds and then one
      // it does not (no Message-ID) before the first one's report came, since a
      // delivery to an address that has never bounced is recorded nowhere - or
      // when that second, unregistered delivery fell in the same second as the
      // first, since the times compared have one-second resolution.
      static int ReportRunIncrease(int increment, int setAside, int laterKey, const String &lastDelivered, const String &sent);

      // The failed recipients of a report that are looked up, as indexes into
      // report.recipients: Action failed, each address once, at most limit of
      // them. truncated says whether any were left out.
      static std::vector<size_t> ReportedFailures(const DeliveryReport &report, size_t limit, bool &truncated);

      // Register lookups for one report message. A genuine report names the failed
      // recipients of one message; a report naming more is either a very large
      // list, whose remaining dead addresses the next report will name, or a
      // message built to make this server work.
      static const size_t MaxReportLookups = 50;

      // -------------------------------------------------------- administration

      enum AddResult
      {
         AddAdded = 0,
         AddInvalidAddress = 1,
         AddHostedAddress = 2,
         AddAlreadyListed = 3,
         AddInvalidExpiry = 4,
         AddFailed = 5
      };

      static __int64 DomainIDByName(const String &domainName);

      static AddResult AddManual(__int64 domainID, const String &address, const String &note, int expiresDays);

      // 1 removed, 0 not listed, -1 the database failed.
      static int Remove(__int64 domainID, const String &address);

      // What the maintenance sweep removed. False when a sweep failed.
      static bool RunMaintenance(int &suppressionsExpired, int &countsPruned, int &reportKeysPruned);

      // The documents the REST routes answer and the COM methods return, built in
      // one place so the Control Panel and the Deck read the same thing. UTF-8.
      static bool SuppressionsJson(__int64 domainID, const String &search, int limit, int offset, AnsiString &json);
      static bool CountsJson(__int64 domainID, const String &search, int limit, int offset, AnsiString &json);
      static AnsiString SuppressionItemJson(__int64 domainID, const String &address);
      static AnsiString ClassifyJson(const String &status, const String &diagnostic);

   private:
      static bool Enabled_();
      static __int64 SenderDomainID_(const String &sender);
      static void Count_(__int64 domainID, const String &address, const BounceClassifier::Verdict &verdict,
                         int source, const String &diagnostic, const String &remoteMta,
                         const BounceReportKeyRow *reportKey);
      static void RecordAcceptance_(__int64 domainID, std::shared_ptr<Message> message, const String &address,
                                    const String &reportKey);
      static void ReportListUnreadable_(const String &detail);
      static String DescribeEntry_(const BounceSuppressionEntry &entry, const String &domainName, String &enhancedCode);
   };

   class BounceReportGateTester
   {
   public:
      void Test();
   };

   class BounceKeyTester
   {
   public:
      void Test();
   };
}
