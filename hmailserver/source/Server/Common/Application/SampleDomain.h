// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// A sample domain that can be thrown away: example.test with three accounts, a
// distribution list of the three, one rule on one account and a handful of
// messages delivered into the mailboxes, so that every page of the Control
// Panel, the Control Deck and the webmail has something in it while somebody is
// learning - and one call that removes all of it and says what it removed.
//
// WHAT IS MADE
//
//   the domain      example.test, active, made with every default a domain made
//                   in the Control Panel gets
//   three accounts  alice, bob and carol at example.test, with names, all with
//                   ONE password, minted when the sample is made and shown once
//                   in the answer that makes it. It is not stored anywhere and
//                   no later read of the sample can tell you what it was: the
//                   accounts are ordinary mailboxes on a server whose default
//                   address range lets an authenticated session relay to the
//                   Internet, so a password printed in the manual would be
//                   three known credentials on every installation that had ever
//                   tried the sample out.
//   one list        everyone@example.test, public, with the three as members
//   one rule        on alice's account: a subject that contains "Invoice" is
//                   moved to a folder named Invoices
//   eight messages  between the three and to the list, with realistic subjects,
//                   a thread, an attachment and one invoice, sent through the
//                   server's own delivery path - the file first, the row in the
//                   queue second, the deliverer woken - so that they arrive the
//                   way mail arrives: the list expands, the rule files the
//                   invoice, every session is notified. They are in the
//                   mailboxes a few seconds after the call answers.
//
// THE MARKER
//
// The database id of the domain the sample made is written to the setting
// SampleDomainId (Configuration::SetSampleDomainId), and removal removes the
// domain that carries exactly that id and is named example.test - never an
// example.test somebody made themselves, which is refused by name. A marker
// whose domain has gone (deleted by hand, or by the regression suite, whose own
// domain is example.test) is the same as none. The first-hour checklist reads
// the marker to leave the sample out of its count: a sample domain is not the
// administrator's first domain.
//
// WHAT IS NOT DONE
//
// Nothing outside the domain: no listener, no certificate, no route, no global
// rule, no setting but the marker. The password bypasses the password policy and
// the breach check on purpose: it is twenty characters of Crockford base32 from
// the same generator an app password comes out of, so the policy has nothing to
// object to and the breach check has nothing to find, and a sample that will not
// build because the installation's policy wants a digit is a sample nobody can
// use. Removal removes the whole domain as PersistentDomain deletes one, so
// anything a learner added to it in the meantime goes with it, and the report
// counts what went - including the sample's own mail still in the delivery
// queue, which is dropped rather than left addressed to mailboxes that have
// gone.
#pragma once

namespace HM
{
   class Domain;

   class SampleDomain
   {
   public:
      // The fixed names. There is no fixed password: see the header comment.
      static const wchar_t *DomainName;
      static const wchar_t *ListAddress;

      // What the marker and the domain table say now.
      enum State
      {
         StateAbsent = 0,    // no example.test, or one the marker does not name
         StatePresent = 1,   // the example.test the sample made
         StateForeign = 2,   // an example.test the sample did not make
         StateUnknown = 3    // hm_domains could not be read, so none of the above
      };

      enum Outcome
      {
         OutcomeDone = 0,      // made or removed; json carries the report
         OutcomeRefused = 1,   // present already, absent already, or foreign; sentence says which
         OutcomeFailed = 2     // the server could not do it; sentence says what, and the error log has the rest
      };

      // The state, with the example.test that exists when one does.
      //
      // StateUnknown rather than StateAbsent when the domain table could not be
      // read. Those two used to be the same answer, and they are not: a
      // transient failure then read as "there is no example.test", which made
      // Remove clear the marker and orphan a sample that was still there -
      // foreign for ever afterwards, refused in both directions, and counted by
      // the checklist as the administrator's first domain.
      static State Examine(std::shared_ptr<Domain> &domain);

      // The id of the sample domain when it is present, and 0 when it is not or
      // cannot be told: the one question the checklist asks.
      static __int64 PresentDomainId();

      // The state as JSON: what GET /api/v1/setup/sample-domain and
      // Utilities.GetSampleDomain answer.
      static AnsiString StateJson();

      // Makes the sample and reports it as JSON; refused when example.test
      // exists, whoever made it. The deliverer is woken when there is one, and
      // left to find the queued messages on its next pass when there is not
      // (the Linux command line, run beside a server or with none running).
      //
      // The password the three mailboxes get is in the report, under "password"
      // and in the sentence, and that is the only time anything says it: it is
      // hashed into the accounts like any other and nothing keeps the clear
      // text. Every surface that offers the creation shows the sentence, so
      // every one of them shows the password once.
      static Outcome Create(AnsiString &json, String &sentence);

      // Removes the sample and reports what went as JSON; refused when there is
      // no sample, and when example.test exists but is not the sample's.
      static Outcome Remove(AnsiString &json, String &sentence);
   };
}
