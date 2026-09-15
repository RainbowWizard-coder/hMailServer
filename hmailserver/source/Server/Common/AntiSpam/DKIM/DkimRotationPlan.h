// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <set>
#include <vector>

namespace HM
{
   // The rules of a DKIM key rotation, as functions of values.
   //
   // Nothing here reads a file, the database, the clock or DNS: the time is a
   // parameter and the state is a structure, so DkimRotationTester can walk every
   // boundary - one second short of the interval and one second past it, the
   // propagation period, the retention period, a record that stops matching - in
   // process, without a delivery, a resolver or a wait. DkimRotation is the part
   // that has I/O; it builds a State, asks Decide what is owed, and does it.
   //
   // THE WALK. A rotation is three slots, which are the ordinary practice rather
   // than an invention of this server:
   //
   //   current    the domain's primary selector and key - the only one that signs.
   //   next       the domain's secondary selector and key. Generated, its record
   //              shown to the administrator, and checked from the server's own
   //              resolver until it answers with exactly this key's public half.
   //              The first pass that sees it matching starts the propagation
   //              clock; a pass that does not see it matching stops the clock and
   //              sends it back to zero, because a pass that has stopped being true
   //              is not evidence of anything.
   //   retiring   the selector that signed before the last switch, kept published
   //              for the retention period for the verifiers and the reports that
   //              arrive late, then retired: its record may go, and a key file the
   //              rotation generated is removed.
   //
   // Signing moves from current to next when next's record has matched for the whole
   // propagation period and the retiring slot is free. A second rotation that becomes
   // ready while the first one's old selector is still being kept waits for it; two
   // retiring selectors at once would be two records the administrator has to track.
   class DkimRotationPlan
   {
   public:
      // The outcome of the last check of one slot's record(s).
      enum CheckStatus
      {
         StatusUnchecked = 0,
         StatusMatches = 1,
         StatusNotFound = 2,
         StatusDifferent = 3,
         StatusLookupFailed = 4,
         StatusKeyUnreadable = 5
      };

      static const int SecondsPerHour = 3600;
      static const int SecondsPerDay = 86400;

      struct Policy
      {
         Policy() : enabled(false), interval_days(180), propagation_hours(72), retention_days(7), key_bits(2048) {}

         bool enabled;
         int interval_days;       // 1 .. 3650
         int propagation_hours;   // 1 .. 720
         int retention_days;      // 1 .. 365
         int key_bits;            // 2048, 3072 or 4096

         // The one validation every interface applies. field names the member in
         // the REST vocabulary (dkim_rotation_interval_days and so on), error is the
         // sentence.
         static bool Validate(const Policy &policy, AnsiString &field, AnsiString &error);
      };

      struct Slot
      {
         Slot() : generated(false), since(0), status(StatusUnchecked), checked(0) {}

         AnsiString selector;
         String key_file;
         bool generated;   // the rotation wrote this key file; nothing else may make it deletable
         __int64 since;    // current: became current; next: created; retiring: started retiring
         int status;
         __int64 checked;
         String detail;    // the host that did not match, and what it held

         bool IsEmpty() const { return selector.IsEmpty(); }
      };

      struct State
      {
         State() : domain_active(true), next_first_seen(0), retired_at(0), last_error_time(0) {}

         bool domain_active;
         Slot current;
         Slot next;
         __int64 next_first_seen;   // 0 until a pass has seen next's record matching
         Slot retiring;

         AnsiString retired_selector;
         __int64 retired_at;
         String retired_key_file;   // a key file retiring left on disk, because the rotation had not generated it

         String last_error;
         __int64 last_error_time;
      };

      struct Waiting
      {
         Waiting() : until(0) {}
         Waiting(const char *step_, const char *reason_, __int64 until_, const String &detail_)
            : step(step_), reason(reason_), until(until_), detail(detail_) {}

         AnsiString step;     // start | switch | retire
         AnsiString reason;   // see Decide
         __int64 until;       // seconds since the epoch, or 0 when nothing but an event ends the wait
         String detail;
      };

      struct Decision
      {
         Decision() : generate(false), switch_now(false), retire_now(false) {}

         bool generate;
         bool switch_now;
         bool retire_now;
         AnsiString phase;    // off | idle | awaiting_dns | propagating | ready | waiting_for_retiring_slot
         std::vector<Waiting> waiting;
      };

      // What is owed at now, given the state as the checks of this pass left it.
      //
      // Waiting reasons: for start, not_scheduled, domain_inactive and interval; for
      // switch, unchecked, record_not_found, record_differs, lookup_failed,
      // key_unreadable, propagation and retiring_slot; for retire, retention.
      static Decision Decide(const Policy &policy, const State &state, __int64 now);

      // The three times the status document reports. 0 when there is none.
      static __int64 NextRotationDue(const Policy &policy, const State &state);
      static __int64 SwitchDue(const Policy &policy, const State &state);
      static __int64 RetireDue(const Policy &policy, const State &state);

      // The first-sighting rule, applied to next after a check: a match with no
      // first sighting records now; anything but a match forgets the sighting.
      static void ApplyNextCheck(State &state, int status, __int64 now);

      // A selector is DNS labels of lower-case letters, digits and hyphens (never at
      // either end of a label), joined by dots, each label at most 63 characters and
      // the whole at most 63 - short enough that selector._domainkey.domain stays a
      // legal name for any domain this server hosts. No underscore, slash, backslash
      // or "..": the selector becomes part of a file name.
      static bool IsValidSelector(const AnsiString &selector);

      // "s" and the UTC date as yyyymmdd - the name the Control Panel has always
      // suggested - with b, c, ... z appended while the name is taken.
      static AnsiString SuggestSelector(__int64 now, const std::set<AnsiString> &taken);

      // Whether any of the TXT records at one name publishes this public key. The
      // whole of RFC 6376 3.6.1 that bears on it: v absent or DKIM1; k absent (rsa)
      // or the key's own type; h absent or allowing sha256; s absent or allowing
      // email; p, with all whitespace removed, decoding to exactly the expected
      // bytes. An empty p is a revoked key and never a match. One record matching is
      // a match, which is what a verifier does (DKIM::RetrievePublicKey_ tries each).
      // Answers StatusMatches or StatusDifferent; found carries the first record,
      // shortened, when nothing matched.
      static int MatchRecords(const std::vector<AnsiString> &records, const AnsiString &keyType,
                              const std::vector<unsigned char> &expected, AnsiString &found);

      static const char *StatusWord(int status);
   };

   class DkimRotationTester
   {
   public:
      void Test();
   };
}
