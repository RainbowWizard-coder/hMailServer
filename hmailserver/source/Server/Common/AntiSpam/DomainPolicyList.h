// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "../BO/DomainPolicyEntry.h"

namespace HM
{
   // The domain white and black list (discussion #285): a sending organisation
   // named by its domain, when its outbound addresses change every week and an
   // IP list would be dead by the second move.
   //
   //   White  "I know who they are. Their mail compliance is a mess and will stay
   //          a mess. I still want the mail." The session skips greylisting and
   //          the hard refusals the spam score would otherwise make - the 550 at
   //          RCPT TO and the 554 after DATA at the delete threshold, the
   //          quarantine, and the 451 SPFTempErrorDefer would answer - while
   //          every test still runs, still logs, and still writes its headers.
   //          The mark threshold still marks: scoring continues, it is just not
   //          the accept gate.
   //   Black  "I know who they are. Never accept." A 550 at MAIL FROM, before
   //          greylisting, before scoring and before a single lookup is paid for
   //          - a fresh address or a passing SPF record copied this morning
   //          changes nothing, because the key is the name.
   //
   // Keyed by the envelope sender's domain, the HELO/EHLO name, or both, as each
   // row says. Consulted for an unauthenticated inbound session only, and never
   // used to override authentication: one of our own people who authenticated
   // is not "inbound". Distinct from the three per-recipient allow and block
   // lists (SenderListsAndDigests.md), which act at delivery and decide for one
   // recipient; this decides for the connection, before there is a recipient.
   //
   // A whole-table cache, reloaded when a row is saved or deleted, as
   // SpamTrapMonitor keeps its traps: the list is a handful of rows an
   // administrator typed, and one read costs less than a query for every MAIL
   // FROM. An empty list costs one flag test and changes nothing.
   class DomainPolicyList
   {
   public:
      enum Verdict
      {
         VerdictNone = 0,
         VerdictWhite = 1,
         VerdictBlack = 2
      };

      struct Decision
      {
         Decision() : verdict(VerdictNone), key(DomainPolicyEntry::MatchBoth) { }

         Verdict verdict;

         // Which row decided, and what it matched: for the log line and the
         // refusal text.
         String pattern;
         String matchedName;
         DomainPolicyEntry::MatchOn key;
      };

      // The cache is stale; the next Decide reloads it. Called by the persister
      // on every save and delete.
      static void SetNeedRefresh();

      // What the list says about a session whose envelope sender is
      // envelopeSender (empty for the null sender) and whose HELO name is
      // heloHost. When several rows match, the most specific pattern wins - a
      // longer pattern is more specific, and a whole domain is more specific
      // than the suffix of the same length - and black wins a tie.
      static Decision Decide(const String &envelopeSender, const String &heloHost);

      // The list as a JSON document for COM and the Control Panel: items, each
      // with id, pattern, match_on, action, note and created. readable is false
      // when the table could not be read, and the document then has no items.
      static AnsiString Document(bool &readable);

      // Adds a row. matchOnWord is envelope, helo or both; actionWord is white
      // or black. False with refusal when the pattern is not a domain, a word is
      // not one of those, or the pair is already listed.
      static bool Add(const String &pattern, const String &matchOnWord, const String &actionWord,
                      const String &note, __int64 &id, String &refusal);

      // Removes the row with that id, answering whether there was one.
      static bool Remove(__int64 id, bool &found);

      // The domain part of an address, or the HELO name, as the list compares
      // it: trimmed, lower-cased, no trailing dot, and empty for an address
      // literal such as [192.0.2.1], which no domain pattern covers.
      static String NormaliseName(const String &name);

   private:

      static bool Refresh_();
   };
}
