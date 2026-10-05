// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "DkimRotationPlan.h"
#include "DkimKeyPair.h"
#include "DKIM.h"
#include "DKIMParameters.h"
#include "../../Util/Encoding/Base64.h"

#include <cstring>
#include <ctime>
#include <stdexcept>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      AnsiString WithoutWhitespace(const AnsiString &value)
      {
         AnsiString result;
         for (int i = 0; i < value.GetLength(); i++)
         {
            char character = value[i];
            if (character == ' ' || character == '\t' || character == '\r' || character == '\n')
               continue;
            result += character;
         }
         return result;
      }

      bool ListAllows(const AnsiString &list, const char *wanted, const char *wildcard)
      {
         std::vector<AnsiString> items = StringParser::SplitString(list, ":");
         for (AnsiString item : items)
         {
            item.Trim();
            if (item.CompareNoCase(wanted) == 0 || (wildcard != nullptr && item == wildcard))
               return true;
         }
         return false;
      }

      bool Utc(__int64 seconds, struct tm &parts)
      {
#ifdef HM_PLATFORM_POSIX
         time_t value = (time_t) seconds;
         return ::gmtime_r(&value, &parts) != nullptr;
#else
         __time64_t value = seconds;
         return _gmtime64_s(&parts, &value) == 0;
#endif
      }
   }

   bool
   DkimRotationPlan::Policy::Validate(const Policy &policy, AnsiString &field, AnsiString &error)
   {
      if (policy.interval_days < 1 || policy.interval_days > 3650)
      {
         field = "dkim_rotation_interval_days";
         error = "dkim_rotation_interval_days must be a whole number between 1 and 3650";
         return false;
      }

      if (policy.propagation_hours < 1 || policy.propagation_hours > 720)
      {
         field = "dkim_rotation_propagation_hours";
         error = "dkim_rotation_propagation_hours must be a whole number between 1 and 720";
         return false;
      }

      if (policy.retention_days < 1 || policy.retention_days > 365)
      {
         field = "dkim_rotation_retention_days";
         error = "dkim_rotation_retention_days must be a whole number between 1 and 365";
         return false;
      }

      if (policy.key_bits != 2048 && policy.key_bits != 3072 && policy.key_bits != 4096)
      {
         field = "dkim_rotation_key_bits";
         error = "dkim_rotation_key_bits must be 2048, 3072 or 4096";
         return false;
      }

      return true;
   }

   __int64
   DkimRotationPlan::NextRotationDue(const Policy &policy, const State &state)
   {
      if (!policy.enabled || state.current.IsEmpty())
         return 0;

      return state.current.since + (__int64) policy.interval_days * SecondsPerDay;
   }

   __int64
   DkimRotationPlan::SwitchDue(const Policy &policy, const State &state)
   {
      if (state.next.IsEmpty() || state.next_first_seen <= 0)
         return 0;

      return state.next_first_seen + (__int64) policy.propagation_hours * SecondsPerHour;
   }

   __int64
   DkimRotationPlan::RetireDue(const Policy &policy, const State &state)
   {
      if (state.retiring.IsEmpty())
         return 0;

      return state.retiring.since + (__int64) policy.retention_days * SecondsPerDay;
   }

   void
   DkimRotationPlan::ApplyNextCheck(State &state, int status, __int64 now)
   {
      state.next.status = status;
      state.next.checked = now;

      if (status == StatusMatches)
      {
         if (state.next_first_seen <= 0)
            state.next_first_seen = now;
      }
      else
      {
         state.next_first_seen = 0;
      }
   }

   void
   DkimRotationPlan::Stalled(const State &state, bool scheduleOn, __int64 now, int thresholdDays, bool &waiting, bool &failed)
   {
      if (thresholdDays < 1)
         thresholdDays = 1;

      const bool hasNext = !state.next.IsEmpty();
      const bool running = scheduleOn || hasNext || !state.retiring.IsEmpty();

      const __int64 lookedAt = state.next.checked > now ? state.next.checked : now;
      waiting = hasNext && state.next_first_seen <= 0 &&
                lookedAt - state.next.since >= (__int64) thresholdDays * SecondsPerDay;

      failed = running && !state.last_error.IsEmpty() && state.last_error_time > 0;
   }

   DkimRotationPlan::Decision
   DkimRotationPlan::Decide(const Policy &policy, const State &state, __int64 now)
   {
      Decision decision;

      const bool hasNext = !state.next.IsEmpty();
      const bool hasRetiring = !state.retiring.IsEmpty();
      const bool hasCurrent = !state.current.IsEmpty() && !state.current.key_file.IsEmpty();

      // Retire first: a retire that is due in this pass frees the slot a switch in
      // the same pass may be waiting for.
      if (hasRetiring)
      {
         const __int64 due = RetireDue(policy, state);

         if (now >= due)
            decision.retire_now = true;
         else
            decision.waiting.push_back(Waiting("retire", "retention", due, String()));
      }

      if (hasNext)
      {
         switch (state.next.status)
         {
         case StatusMatches:
            {
               const __int64 due = SwitchDue(policy, state);

               if (due <= 0)
               {
                  decision.phase = "awaiting_dns";
                  decision.waiting.push_back(Waiting("switch", "unchecked", 0, String()));
               }
               else if (now < due)
               {
                  decision.phase = "propagating";
                  decision.waiting.push_back(Waiting("switch", "propagation", due, String()));
               }
               else if (hasRetiring && !decision.retire_now)
               {
                  decision.phase = "waiting_for_retiring_slot";
                  decision.waiting.push_back(Waiting("switch", "retiring_slot", RetireDue(policy, state), String(state.retiring.selector)));
               }
               else
               {
                  decision.phase = "ready";
                  decision.switch_now = true;
               }
            }
            break;
         case StatusNotFound:
            decision.phase = "awaiting_dns";
            decision.waiting.push_back(Waiting("switch", "record_not_found", 0, state.next.detail));
            break;
         case StatusDifferent:
            decision.phase = "awaiting_dns";
            decision.waiting.push_back(Waiting("switch", "record_differs", 0, state.next.detail));
            break;
         case StatusLookupFailed:
            decision.phase = "awaiting_dns";
            decision.waiting.push_back(Waiting("switch", "lookup_failed", 0, state.next.detail));
            break;
         case StatusKeyUnreadable:
            decision.phase = "awaiting_dns";
            decision.waiting.push_back(Waiting("switch", "key_unreadable", 0, state.next.detail));
            break;
         default:
            decision.phase = "awaiting_dns";
            decision.waiting.push_back(Waiting("switch", "unchecked", 0, String()));
            break;
         }

         return decision;
      }

      // Nothing staged: whether the schedule owes a new key. A rotation started by
      // hand is carried to its end with the schedule off; only the start of one is
      // the schedule's to decide.
      if (!policy.enabled)
      {
         decision.phase = hasRetiring ? "idle" : "off";
         decision.waiting.push_back(Waiting("start", "not_scheduled", 0, String()));
         return decision;
      }

      decision.phase = "idle";

      if (!state.domain_active)
      {
         decision.waiting.push_back(Waiting("start", "domain_inactive", 0, String()));
         return decision;
      }

      // A domain with no key at all gets its first one the same way: generated,
      // published, seen and then made the signing key. There is nothing to wait for.
      if (!hasCurrent)
      {
         decision.generate = true;
         return decision;
      }

      const __int64 due = NextRotationDue(policy, state);
      if (now >= due)
         decision.generate = true;
      else
         decision.waiting.push_back(Waiting("start", "interval", due, String()));

      return decision;
   }

   bool
   DkimRotationPlan::IsValidSelector(const AnsiString &selector)
   {
      if (selector.IsEmpty() || selector.GetLength() > 63)
         return false;

      int labelLength = 0;
      char previous = '.';

      for (int i = 0; i < selector.GetLength(); i++)
      {
         const char character = selector[i];

         if (character == '.')
         {
            // An empty label (a leading dot or two in a row) or a label ending in a
            // hyphen.
            if (labelLength == 0 || previous == '-')
               return false;

            labelLength = 0;
            previous = character;
            continue;
         }

         const bool letter = character >= 'a' && character <= 'z';
         const bool digit = character >= '0' && character <= '9';
         const bool hyphen = character == '-';

         if (!letter && !digit && !hyphen)
            return false;

         if (hyphen && labelLength == 0)
            return false;

         labelLength++;
         if (labelLength > 63)
            return false;

         previous = character;
      }

      return labelLength > 0 && previous != '-';
   }

   AnsiString
   DkimRotationPlan::SuggestSelector(__int64 now, const std::set<AnsiString> &taken)
   {
      struct tm parts = {};
      Utc(now, parts);

      AnsiString base;
      base.Format("s%04d%02d%02d", parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday);

      if (taken.find(base) == taken.end())
         return base;

      for (char suffix = 'b'; suffix <= 'z'; suffix++)
      {
         AnsiString candidate = base;
         candidate += suffix;
         if (taken.find(candidate) == taken.end())
            return candidate;
      }

      // Twenty-six rotations started on one day. The time of day separates the
      // twenty-seventh.
      AnsiString candidate;
      candidate.Format("s%04d%02d%02d%02d%02d%02d", parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday,
                       parts.tm_hour, parts.tm_min, parts.tm_sec);
      return candidate;
   }

   int
   DkimRotationPlan::MatchRecords(const std::vector<AnsiString> &records, const AnsiString &keyType,
                                  const std::vector<unsigned char> &expected, AnsiString &found)
   {
      found = "";

      if (expected.empty())
         return StatusDifferent;

      for (const AnsiString &record : records)
      {
         // A fresh instance per record: Load merges into existing values.
         DKIMParameters parameters;
         parameters.Load(record);

         if (parameters.GetIsSet("v") && parameters.GetValue("v") != "DKIM1")
            continue;

         AnsiString type = parameters.GetIsSet("k") ? parameters.GetValue("k") : AnsiString("rsa");
         if (type.CompareNoCase(keyType) != 0)
            continue;

         if (parameters.GetIsSet("h") && !ListAllows(parameters.GetValue("h"), "sha256", nullptr))
            continue;

         if (parameters.GetIsSet("s") && !ListAllows(parameters.GetValue("s"), "email", "*"))
            continue;

         const AnsiString encoded = WithoutWhitespace(parameters.GetValue("p"));

         // Revoked.
         if (encoded.IsEmpty())
            continue;

         const AnsiString decoded = Base64::Decode(encoded.c_str(), encoded.GetLength());

         if (decoded.GetLength() != (int) expected.size())
            continue;

         if (memcmp(decoded.c_str(), &expected[0], expected.size()) == 0)
            return StatusMatches;
      }

      if (!records.empty())
      {
         found = records[0];
         if (found.GetLength() > 120)
            found = found.Mid(0, 120) + "...";
      }

      return StatusDifferent;
   }

   const char *
   DkimRotationPlan::StatusWord(int status)
   {
      switch (status)
      {
      case StatusMatches:
         return "matches";
      case StatusNotFound:
         return "not_found";
      case StatusDifferent:
         return "different";
      case StatusLookupFailed:
         return "lookup_failed";
      case StatusKeyUnreadable:
         return "key_unreadable";
      default:
         return "unchecked";
      }
   }

   // ---------------------------------------------------------------------------

   namespace
   {
      void Require(bool condition, const char *what)
      {
         if (!condition)
            throw std::logic_error(what);
      }

      bool Waits(const DkimRotationPlan::Decision &decision, const char *step, const char *reason)
      {
         for (const DkimRotationPlan::Waiting &waiting : decision.waiting)
         {
            if (waiting.step == step && waiting.reason == reason)
               return true;
         }
         return false;
      }

      DkimRotationPlan::Slot MakeSlot(const char *selector, __int64 since, int status)
      {
         DkimRotationPlan::Slot slot;
         slot.selector = selector;
         slot.key_file = _T("key.pem");
         slot.since = since;
         slot.status = status;
         return slot;
      }
   }

   void
   DkimRotationTester::Test()
   {
      typedef DkimRotationPlan Plan;

      const __int64 T = 1789430400;   // 2026-09-15 00:00:00 UTC
      const __int64 hour = Plan::SecondsPerHour;
      const __int64 day = Plan::SecondsPerDay;

      Plan::Policy policy;
      policy.enabled = true;
      policy.interval_days = 30;
      policy.propagation_hours = 72;
      policy.retention_days = 7;
      policy.key_bits = 2048;

      // ---- the policy's own limits
      {
         AnsiString field, error;
         Require(Plan::Policy::Validate(policy, field, error), "A valid policy was refused.");

         Plan::Policy bad = policy;
         bad.interval_days = 0;
         Require(!Plan::Policy::Validate(bad, field, error) && field == "dkim_rotation_interval_days", "An interval of 0 days was accepted.");
         bad = policy;
         bad.propagation_hours = 721;
         Require(!Plan::Policy::Validate(bad, field, error) && field == "dkim_rotation_propagation_hours", "A propagation of 721 hours was accepted.");
         bad = policy;
         bad.retention_days = 0;
         Require(!Plan::Policy::Validate(bad, field, error) && field == "dkim_rotation_retention_days", "A retention of 0 days was accepted.");
         bad = policy;
         bad.key_bits = 1024;
         Require(!Plan::Policy::Validate(bad, field, error) && field == "dkim_rotation_key_bits", "A 1024-bit key size was accepted.");
      }

      // ---- the interval, one second either side
      {
         Plan::State state;
         state.current = MakeSlot("s1", T, Plan::StatusMatches);

         Plan::Decision early = Plan::Decide(policy, state, T + 30 * day - 1);
         Require(!early.generate && Waits(early, "start", "interval"), "A rotation started one second before its interval had passed.");

         Plan::Decision due = Plan::Decide(policy, state, T + 30 * day);
         Require(due.generate, "A rotation did not start when its interval had passed.");

         // Off: nothing starts however long the key has been in use.
         Plan::Policy off = policy;
         off.enabled = false;
         Plan::Decision never = Plan::Decide(off, state, T + 3650 * day);
         Require(!never.generate && !never.switch_now && !never.retire_now && never.phase == "off",
                 "A domain with rotation off and nothing in flight was given something to do.");

         // Inactive: the schedule does not start one.
         state.domain_active = false;
         Plan::Decision inactive = Plan::Decide(policy, state, T + 31 * day);
         Require(!inactive.generate && Waits(inactive, "start", "domain_inactive"), "A rotation started for an inactive domain.");

         // No key at all: the first one is generated at once.
         Plan::State empty;
         Require(Plan::Decide(policy, empty, T).generate, "A domain with rotation on and no key was not given one.");
      }

      // ---- the propagation period counts from the first sighting
      {
         Plan::State state;
         state.current = MakeSlot("s1", T, Plan::StatusMatches);
         state.next = MakeSlot("s2", T, Plan::StatusUnchecked);

         Plan::ApplyNextCheck(state, Plan::StatusNotFound, T + hour);
         Require(state.next_first_seen == 0, "A record that was not found started the propagation clock.");
         Plan::Decision missing = Plan::Decide(policy, state, T + 3650 * day);
         Require(!missing.switch_now && Waits(missing, "switch", "record_not_found") && missing.phase == "awaiting_dns",
                 "A switch was decided while the record was missing.");

         Plan::ApplyNextCheck(state, Plan::StatusDifferent, T + 2 * hour);
         Require(state.next_first_seen == 0 && !Plan::Decide(policy, state, T + 3650 * day).switch_now,
                 "A record carrying another key started the propagation clock or allowed a switch.");

         const __int64 seen = T + 3 * hour;
         Plan::ApplyNextCheck(state, Plan::StatusMatches, seen);
         Require(state.next_first_seen == seen, "The first matching check did not record the first sighting.");

         Plan::ApplyNextCheck(state, Plan::StatusMatches, seen + hour);
         Require(state.next_first_seen == seen, "A later matching check moved the first sighting.");

         Plan::Decision early = Plan::Decide(policy, state, seen + 72 * hour - 1);
         Require(!early.switch_now && Waits(early, "switch", "propagation") && early.phase == "propagating",
                 "A switch was decided one second before the propagation period had passed.");

         Plan::Decision due = Plan::Decide(policy, state, seen + 72 * hour);
         Require(due.switch_now && due.phase == "ready", "No switch was decided when the propagation period had passed.");

         // A record that disappears sends the wait back to zero.
         Plan::ApplyNextCheck(state, Plan::StatusLookupFailed, seen + 71 * hour);
         Require(state.next_first_seen == 0, "A failed lookup kept the first sighting.");
         Plan::ApplyNextCheck(state, Plan::StatusMatches, seen + 73 * hour);
         Require(!Plan::Decide(policy, state, seen + 73 * hour).switch_now, "A record seen again was switched to without waiting again.");
         Require(Plan::Decide(policy, state, seen + 145 * hour).switch_now, "A record seen again was never switched to.");
      }

      // ---- a busy retiring slot holds the switch, and the retention period
      {
         Plan::State state;
         state.current = MakeSlot("s2", T, Plan::StatusMatches);
         state.retiring = MakeSlot("s1", T, Plan::StatusMatches);
         state.next = MakeSlot("s3", T, Plan::StatusUnchecked);
         Plan::ApplyNextCheck(state, Plan::StatusMatches, T);

         Plan::Decision held = Plan::Decide(policy, state, T + 72 * hour);
         Require(!held.switch_now && !held.retire_now && Waits(held, "switch", "retiring_slot") && Waits(held, "retire", "retention") &&
                 held.phase == "waiting_for_retiring_slot",
                 "A switch was decided while the retiring slot was busy.");

         Plan::Decision edge = Plan::Decide(policy, state, T + 7 * day - 1);
         Require(!edge.retire_now, "The old selector was retired one second before its retention period ended.");

         Plan::Decision both = Plan::Decide(policy, state, T + 7 * day);
         Require(both.retire_now && both.switch_now, "Retiring the old selector did not free the slot for the switch in the same pass.");
      }

      // ---- what the dkim.rotation.stalled alert counts
      {
         bool waiting = false, failed = false;

         // A next key unseen: not before the threshold, and from the threshold on.
         Plan::State unseen;
         unseen.current = MakeSlot("s1", T, Plan::StatusMatches);
         unseen.next = MakeSlot("s2", T, Plan::StatusNotFound);
         Plan::Stalled(unseen, false, T + 7 * day - 1, 7, waiting, failed);
         Require(!waiting && !failed, "A next key unseen for one second less than the threshold was counted as stalled.");
         Plan::Stalled(unseen, false, T + 7 * day, 7, waiting, failed);
         Require(waiting && !failed, "A next key unseen for the whole threshold was not counted as stalled.");

         // Measured to the rotation's own later look, and never once it has been seen.
         unseen.next.checked = T + 8 * day;
         Plan::Stalled(unseen, false, T, 7, waiting, failed);
         Require(waiting, "A pass with the clock moved past the threshold was not what the alert measured to.");
         unseen.next_first_seen = T + 8 * day;
         Plan::Stalled(unseen, false, T + 30 * day, 7, waiting, failed);
         Require(!waiting, "A next key whose record has been seen was counted as stalled.");

         // A failure counts while the rotation runs...
         Plan::State broken;
         broken.current = MakeSlot("s1", T, Plan::StatusMatches);
         broken.last_error = _T("The DKIM key directory could not be created.");
         broken.last_error_time = T;
         Plan::Stalled(broken, true, T, 7, waiting, failed);
         Require(failed && !waiting, "A failed rotation with the schedule on was not counted.");

         Plan::State retiring = broken;
         retiring.retiring = MakeSlot("s0", T, Plan::StatusMatches);
         Plan::Stalled(retiring, false, T, 7, waiting, failed);
         Require(failed, "A failed rotation with an old selector still retiring was not counted.");

         // ...and not on a domain with the schedule off and nothing in flight, where
         // nothing would ever try again or clear it.
         Plan::Stalled(broken, false, T + 3650 * day, 7, waiting, failed);
         Require(!failed && !waiting, "A failed Rotate now on a domain with the schedule off and nothing staged was counted as stalled.");
      }

      // ---- selectors
      {
         Require(Plan::IsValidSelector("s20260915"), "A dated selector was refused.");
         Require(Plan::IsValidSelector("mail-2026.eu"), "A dotted, hyphenated selector was refused.");
         Require(!Plan::IsValidSelector("../x"), "../x was accepted as a selector.");
         Require(!Plan::IsValidSelector("a_b"), "An underscore was accepted in a selector.");
         Require(!Plan::IsValidSelector("a/b"), "A slash was accepted in a selector.");
         Require(!Plan::IsValidSelector("a\\b"), "A backslash was accepted in a selector.");
         Require(!Plan::IsValidSelector("A1"), "An upper-case selector was accepted.");
         Require(!Plan::IsValidSelector("-a"), "A selector starting with a hyphen was accepted.");
         Require(!Plan::IsValidSelector("a-"), "A selector ending with a hyphen was accepted.");
         Require(!Plan::IsValidSelector("a..b"), "An empty label was accepted.");
         Require(!Plan::IsValidSelector(""), "An empty selector was accepted.");
         Require(!Plan::IsValidSelector(AnsiString("a") + AnsiString("bcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklm")),
                 "A selector longer than 63 characters was accepted.");

         std::set<AnsiString> taken;
         Require(Plan::SuggestSelector(T, taken) == "s20260915", "The suggested selector was not s and the UTC date.");
         taken.insert("s20260915");
         Require(Plan::SuggestSelector(T, taken) == "s20260915b", "A taken selector was suggested again.");
         taken.insert("s20260915b");
         Require(Plan::SuggestSelector(T, taken) == "s20260915c", "The second suffix was not c.");
      }

      // ---- matching a record against a key
      {
         std::vector<unsigned char> key;
         for (int i = 0; i < 40; i++)
            key.push_back((unsigned char) (i * 7 + 3));

         const AnsiString p = Base64::Encode((const char *) &key[0], (int) key.size());
         AnsiString found;

         std::vector<AnsiString> records;
         records.push_back("v=DKIM1; k=rsa; p=" + p);
         Require(Plan::MatchRecords(records, "rsa", key, found) == Plan::StatusMatches, "The exact record did not match.");

         // Whitespace inside p, and a key split across strings the resolver joined.
         AnsiString spaced = "v=DKIM1; k=rsa; p=" + p.Mid(0, 10) + " " + p.Mid(10, 10) + "\t" + p.Mid(20);
         records.clear();
         records.push_back(spaced);
         Require(Plan::MatchRecords(records, "rsa", key, found) == Plan::StatusMatches, "Whitespace inside p= stopped a match.");

         records.clear();
         records.push_back("k=rsa; p=" + p);
         Require(Plan::MatchRecords(records, "rsa", key, found) == Plan::StatusMatches, "A record without v= did not match.");

         records.clear();
         records.push_back("p=" + p);
         Require(Plan::MatchRecords(records, "rsa", key, found) == Plan::StatusMatches, "A record without k= was not read as rsa.");

         records.clear();
         records.push_back("v=DKIM2; k=rsa; p=" + p);
         Require(Plan::MatchRecords(records, "rsa", key, found) == Plan::StatusDifferent, "v=DKIM2 matched.");

         records.clear();
         records.push_back("v=DKIM1; k=rsa; p=" + p);
         Require(Plan::MatchRecords(records, "ed25519", key, found) == Plan::StatusDifferent, "An rsa record matched an Ed25519 key.");

         records.clear();
         records.push_back("v=DKIM1; k=rsa; h=sha1; p=" + p);
         Require(Plan::MatchRecords(records, "rsa", key, found) == Plan::StatusDifferent, "A record allowing only sha1 matched.");

         records.clear();
         records.push_back("v=DKIM1; k=rsa; h=sha1:sha256; p=" + p);
         Require(Plan::MatchRecords(records, "rsa", key, found) == Plan::StatusMatches, "A record allowing sha256 among others did not match.");

         records.clear();
         records.push_back("v=DKIM1; k=rsa; p=");
         Require(Plan::MatchRecords(records, "rsa", key, found) == Plan::StatusDifferent, "A revoked record (empty p=) matched.");

         std::vector<unsigned char> other = key;
         other[5] ^= 0xFF;
         const AnsiString otherP = Base64::Encode((const char *) &other[0], (int) other.size());

         records.clear();
         records.push_back("v=DKIM1; k=rsa; p=" + otherP);
         Require(Plan::MatchRecords(records, "rsa", key, found) == Plan::StatusDifferent && !found.IsEmpty(),
                 "A record carrying another key matched, or the record found was not reported.");

         records.push_back("v=DKIM1; k=rsa; p=" + p);
         Require(Plan::MatchRecords(records, "rsa", key, found) == Plan::StatusMatches, "One matching record among several did not match.");
      }

      // ---- a generated key: its record is what a verifier needs
      {
         AnsiString pem;
         String error;
         Require(DkimKeyPair::Generate(2048, pem, error), "A 2048-bit key could not be generated.");
         Require(!DkimKeyPair::Generate(1024, pem, error), "A 1024-bit key was generated.");
         Require(DkimKeyPair::Generate(2048, pem, error), "A 2048-bit key could not be generated the second time.");

         AnsiString keyType;
         std::vector<unsigned char> publicKey;
         Require(DkimKeyPair::Derive(pem, keyType, publicKey) && keyType == "rsa", "The generated key's public half could not be derived.");

         AnsiString record = DkimKeyPair::RecordValue(keyType, publicKey);
         Require(record.Find("v=DKIM1; k=rsa; p=") == 0, "The record did not start v=DKIM1; k=rsa; p=.");

         DKIMParameters parameters;
         parameters.Load(record);
         AnsiString p = parameters.GetValue("p");

         AnsiString header = "from:someone@example.com\r\nsubject:rotation\r\ndkim-signature:v=1; a=rsa-sha256; b=";
         AnsiString signature = DKIM::SignHash_(pem, header, HashCreator::SHA256);
         Require(!signature.IsEmpty(), "The generated key could not sign.");

         DKIM dkim;
         Require(dkim.VerifyHeaderHash_(header, "rsa-sha256", signature, p) == DKIM::Pass,
                 "A signature by the generated key did not verify against the record derived from it.");

         // The negative control: the same signature against another key's record.
         AnsiString otherPem;
         Require(DkimKeyPair::Generate(2048, otherPem, error), "A second key could not be generated.");
         AnsiString otherType;
         std::vector<unsigned char> otherKey;
         Require(DkimKeyPair::Derive(otherPem, otherType, otherKey), "The second key's public half could not be derived.");
         DKIMParameters otherParameters;
         otherParameters.Load(DkimKeyPair::RecordValue(otherType, otherKey));
         Require(dkim.VerifyHeaderHash_(header, "rsa-sha256", signature, otherParameters.GetValue("p")) != DKIM::Pass,
                 "A signature verified against another key's record.");

         Require(!DkimKeyPair::Derive("-----BEGIN PRIVATE KEY-----\nnot a key\n-----END PRIVATE KEY-----\n", keyType, publicKey),
                 "Text that is not a key was read as one.");
      }
   }
}
