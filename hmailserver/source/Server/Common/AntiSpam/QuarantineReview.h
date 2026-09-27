// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The quarantine's review actions over many held messages at once (GitLab
// issue 1): release, delete, allow the sender and release, block the sender
// and delete. One request names the messages and the action, and the answer
// says what became of each one, so that a group of forty held messages from
// one bulk sender is one request and not forty.
//
// Allow and block write the lists that already exist, and no third one:
//
//   list address   the sender's address, on the server's allow and block list
//                  (hm_senderlist, docs/SenderListsAndDigests.md). Read at
//                  delivery for every recipient: an allowed sender's copy is
//                  delivered unmarked and the account's own thresholds do not
//                  run; a blocked sender's is held for the recipient, or marked
//                  where the quarantine is off. The default.
//   list domain    the organization's domain - of the sender's address, or of
//                  the HELO name - on the domain white and black list
//                  (hm_domainpolicylist, docs/SmtpPolicy.md), as .example.com,
//                  which covers that domain and everything beneath it. Read at
//                  MAIL FROM: white is not refused or held on the spam score,
//                  black is refused with 550 before anything else runs.
//
// The organization's domain is the registrable domain by the Public Suffix
// List, which is also what the review pages group by, so an entry names what
// its group showed. A name that is itself a public suffix - co.uk, a HELO of
// localhost - would cover every domain registered beneath it, so such an entry
// is written only when the request says it knows (confirm_public_suffix). No
// list of free mail providers exists in this server to warn about gmail.com
// and its like; the default, the sender's address, is the answer for those.
//
// Several held messages naming the same sender, or the same domain, make one
// entry. An entry is written first and the messages are acted on after, and a
// message whose entry could not be written is left where it is: allowing a
// sender and then not releasing is recoverable, releasing and then failing to
// allow is a sender who will be held again with nobody told.

#pragma once

namespace HM
{
   struct QuarantinedMessage;

   class QuarantineReview
   {
   public:
      enum Action
      {
         ActionRelease,
         ActionDelete,
         ActionAllow,
         ActionBlock
      };

      enum ListKind
      {
         ListAddress,
         ListDomain
      };

      enum DomainSource
      {
         DomainOfSender,
         DomainOfHelo
      };

      struct Request
      {
         std::vector<__int64> ids;
         Action action = ActionRelease;
         ListKind list = ListAddress;
         DomainSource source = DomainOfSender;

         // Answer what would be written and done, and write and do nothing.
         bool dry_run = false;

         // Write an entry for a name that is itself a public suffix.
         bool confirm_public_suffix = false;
      };

      enum Outcome
      {
         OutcomeDone,
         OutcomeBadRequest,
         OutcomeNeedsConfirmation
      };

      // The most messages one request names. The list pages show at most a
      // thousand, so a select-all is one request.
      static constexpr int MaxIds = 1000;

      // The body POST /api/v1/quarantine/bulk and Quarantine.Bulk take:
      //   ids                    1 to 1000 message ids (required)
      //   action                 release, delete, allow or block (required)
      //   list                   address (default) or domain; allow and block only
      //   domain_from            sender (default) or helo; with list domain only
      //   dry_run                default false
      //   confirm_public_suffix  default false
      // False with the sentence when it is not that; nothing has been done.
      static bool ParseRequest(const AnsiString &utf8Json, Request &out, String &error);

      // Does it. out_json is the answer in every outcome:
      //   action, list, domain_from, dry_run,
      //   entries  [{value, list, match_on, public_suffix, outcome, error, ids}]
      //            outcome is added, changed or unchanged (add, change or
      //            unchanged for a dry run); error names why it was not written
      //   results  [{id, ok, outcome, error}]
      //            outcome is released or deleted (release or delete for a dry
      //            run); error names why that message was left where it was
      //   succeeded, failed
      // OutcomeNeedsConfirmation writes and does nothing, and out_error says
      // which names are public suffixes; out_json then carries public_suffix,
      // those names, beside error.
      static Outcome Run(const Request &request, AnsiString &out_json, String &out_error);

      static bool WritesAList(Action action) { return action == ActionAllow || action == ActionBlock; }

      // The organization's domain the review pages group a held message by:
      // of the sender's address, and of the HELO name. Empty for the null
      // sender, for no HELO, and for a HELO that is an address literal.
      static String SenderDomain(const String &sender);
      static String HeloDomain(const String &helo);

      // Whether a lower-cased domain is itself a public suffix: com, co.uk,
      // a name of the list's private section such as github.io, and a name of
      // one label, which the list's implicit rule makes one.
      static bool IsPublicSuffix(const String &domain);

      static String ActionWord(Action action);

   private:
      static String OrganizationOf_(const String &normalisedName);
      static bool EntryFor_(const Request &request, const QuarantinedMessage &message, String &value, String &problem);
   };

   class QuarantineReviewTester
   {
   public:
      void Test();
   };
}
