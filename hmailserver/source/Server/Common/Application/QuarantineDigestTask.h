// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "../BO/ScheduledTask.h"

namespace HM
{
   // Sends the quarantine digests, and spreads them.
   //
   // WHY IT RUNS EVERY FIVE MINUTES AND NOT ONCE A DAY
   //
   // A digest that goes to every account at 07:00 is a mail storm this server
   // aims at itself: a thousand accounts is a thousand messages queued in one
   // second, each with its own delivery, its own rules, its own Sieve script and
   // its own quota check, on a machine that is also accepting the morning's real
   // mail. So the task wakes every five minutes and, when a domain's digest hour
   // is the current one, sends only its share: the accounts still waiting
   // divided by the ticks left in the hour, rounded up. At 07:00 that is a
   // twelfth of them, at 07:55 it is all that remain, and the hour always
   // finishes what it started.
   //
   // WHAT STOPS A SECOND DIGEST THE SAME DAY
   //
   // Two things, and both are needed. The hour has to match, which alone would
   // still allow a second send five minutes after the first if something new was
   // quarantined in between; and the account's last digest has to be more than
   // twenty hours old. Twenty rather than twenty-four so that a clock that
   // drifts, or an hour changed by an administrator, does not skip a day
   // entirely.
   class QuarantineDigestTask : public ScheduledTask
   {
   public:
      QuarantineDigestTask();
      ~QuarantineDigestTask();

      virtual void DoWork();

      // One pass. Returns the number of digests sent. Public so that a test can
      // run a pass without waiting for the scheduler, and so that a pass can be
      // forced from the COM Utilities object.
      //
      // `force` ignores the hour and the twenty-hour rule and sends to every
      // account of every domain that has the digest on and something new held -
      // which is what an administrator pressing "send it now" means, and what a
      // test needs. It does not ignore the opt-out, the missing base URL or the
      // empty digest: those are the reader's decision, the administrator's
      // omission and the rule that there is nothing to say.
      static int RunPass(bool force);

      // One pass as the scheduler would run it in the given hour of the day,
      // 0 to 23: the hour decides who is due - the digest switch and the hour
      // are settings with levels, an account's own over its domain's
      // (docs/SettingLevels.md) - and, as a forced pass, nothing is spread and
      // the twenty-hour rule is not asked. What a test needs to prove that an
      // account with an hour of its own is sent at its hour and its domain's
      // accounts at theirs, without waiting for either.
      static int RunPassAtHour(int hour);

   private:
      static int TicksLeftInHour_(int minute);
      static int RunPass_(bool force, int atHour);
   };
}
