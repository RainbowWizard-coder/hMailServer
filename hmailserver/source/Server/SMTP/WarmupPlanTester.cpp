// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "WarmupPlanTester.h"
#include "IpWarmup.h"
#include "OutboundProviders.h"

#include <stdexcept>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      void Expect(bool condition, const char *message)
      {
         if (!condition)
            throw std::logic_error(message);
      }

      struct Sample
      {
         int plan;
         int day;
         int google;     // a named provider's ceiling; -1 for none
         int other;      // every other provider's
      };

      // Three sample days of each plan, and the first day past each: the
      // tables docs/IpWarmup.md prints, read back.
      const Sample Samples[] =
      {
         { IpWarmup::Standard,  1,     50,    250 },
         { IpWarmup::Standard,  9,  10000,  50000 },
         { IpWarmup::Standard, 30, 100000, 500000 },
         { IpWarmup::Standard, 31,     -1,     -1 },

         // Half of each, twice the days: day N is standard day (N + 1) / 2.
         { IpWarmup::Gentle,    1,     25,    125 },
         { IpWarmup::Gentle,    2,     25,    125 },
         { IpWarmup::Gentle,   15,   5000,  25000 },
         { IpWarmup::Gentle,   60,  50000, 250000 },
         { IpWarmup::Gentle,   61,     -1,     -1 },

         // Twice each, half the days: day N is standard day 2N - 1.
         { IpWarmup::Fast,      1,    100,    500 },
         { IpWarmup::Fast,      4,  10000,  50000 },
         { IpWarmup::Fast,     15, 200000, 1000000 },
         { IpWarmup::Fast,     16,     -1,     -1 },
      };
   }

   void
   WarmupPlanTester::Test()
   {
      TestPlans_();
      TestDays_();
      TestDatesAndAddresses_();
      TestProviders_();
   }

   void
   WarmupPlanTester::TestPlans_()
   {
      for (const Sample &sample : Samples)
      {
         Expect(IpWarmup::Ceiling(sample.plan, sample.day, OutboundProviders::Google) == sample.google,
            "A plan's ceiling for a named provider is not the one its table prints.");
         Expect(IpWarmup::Ceiling(sample.plan, sample.day, OutboundProviders::Microsoft) == sample.google,
            "The five named providers do not share one ceiling.");
         Expect(IpWarmup::Ceiling(sample.plan, sample.day, OutboundProviders::Other) == sample.other,
            "The other providers' ceiling is not five times the named ones'.");
      }

      // The standard table, every day of it.
      const int standard[31] = { 50, 100, 200, 500, 1000, 2000, 5000, 10000, 10000, 10000,
                                 20000, 20000, 20000, 20000, 50000, 50000, 50000, 50000, 50000, 50000, 50000,
                                 100000, 100000, 100000, 100000, 100000, 100000, 100000, 100000, 100000, -1 };

      for (int day = 1; day <= 31; day++)
         Expect(IpWarmup::BaseCeiling(IpWarmup::Standard, day) == standard[day - 1], "The standard plan's table is not the one written.");

      // Every plan has a ceiling on its last day and none the day after.
      for (int plan = 0; plan < IpWarmup::PlanCount; plan++)
      {
         Expect(IpWarmup::BaseCeiling(plan, IpWarmup::LastDay(plan)) > 0, "A plan has no ceiling on its last day.");
         Expect(IpWarmup::BaseCeiling(plan, IpWarmup::LastDay(plan) + 1) == -1, "A plan has a ceiling after its last day.");
      }

      // A day before the first is the first.
      Expect(IpWarmup::BaseCeiling(IpWarmup::Standard, 0) == 50, "Day 0 is not held to day 1's ceiling.");

      int plan = -1;
      Expect(IpWarmup::PlanFromName(_T("Gentle"), plan) && plan == IpWarmup::Gentle, "A plan's name is not read back.");
      Expect(!IpWarmup::PlanFromName(_T("quick"), plan), "A name that is no plan was read as one.");
      Expect(IpWarmup::PlanName(IpWarmup::Fast) == _T("fast"), "A plan's name is not written.");
   }

   void
   WarmupPlanTester::TestDays_()
   {
      // 24 September 2026, 13:00 UTC, and the schedule begun eight days before.
      const __int64 moment = (__int64) 1790254800;
      const __int64 today = IpWarmup::DayOf(moment);

      Expect(IpWarmup::FormatDate(today) == _T("2026-09-24"), "A moment's UTC day is not the calendar's.");
      Expect(IpWarmup::DayOf(moment - 13 * 3600) == today, "Midnight UTC is not the day's first second.");
      Expect(IpWarmup::DayOf(moment - 13 * 3600 - 1) == today - 1, "The second before midnight UTC is not the day before.");

      Expect(IpWarmup::WarmupDay(today, today) == 1, "A schedule begun today is not on day 1.");
      Expect(IpWarmup::WarmupDay(today - 8, today) == 9, "A schedule begun eight days ago is not on day 9.");
      Expect(IpWarmup::Ceiling(IpWarmup::Standard, IpWarmup::WarmupDay(today - 8, today), OutboundProviders::Google) == 10000,
         "Day 9 of the standard plan does not allow 10,000.");
      Expect(IpWarmup::WarmupDay(today + 3, today) == 1, "A schedule that begins later is not held to day 1.");
   }

   void
   WarmupPlanTester::TestDatesAndAddresses_()
   {
      __int64 day = 0;

      Expect(IpWarmup::ParseDate(_T("1970-01-01"), day) && day == 0, "The epoch is not day 0.");
      Expect(IpWarmup::ParseDate(_T("2026-09-24"), day) && IpWarmup::FormatDate(day) == _T("2026-09-24"), "A date does not come back as itself.");
      Expect(IpWarmup::ParseDate(_T("2028-02-29"), day), "A leap day was refused.");
      Expect(!IpWarmup::ParseDate(_T("2027-02-29"), day), "The 29th of February of a common year was accepted.");
      Expect(!IpWarmup::ParseDate(_T("2026-04-31"), day), "The 31st of April was accepted.");
      Expect(!IpWarmup::ParseDate(_T("2026-9-24"), day), "A date not written YYYY-MM-DD was accepted.");
      Expect(!IpWarmup::ParseDate(_T("yesterday"), day), "A word was read as a date.");

      Expect(IpWarmup::DayFromDate(2000, 3, 1) - IpWarmup::DayFromDate(2000, 2, 28) == 2, "2000 was not a leap year.");
      Expect(IpWarmup::DayFromDate(2100, 3, 1) - IpWarmup::DayFromDate(2100, 2, 28) == 1, "2100 was a leap year.");

      String canonical;
      Expect(IpWarmup::CanonicalAddress(_T(" 192.0.2.10 "), canonical) && canonical == _T("192.0.2.10"), "An IPv4 address is not taken as written.");
      Expect(IpWarmup::CanonicalAddress(_T("2001:DB8:0:0::1"), canonical) && canonical == _T("2001:db8::1"), "An IPv6 address is not written the way a delivery names it.");
      Expect(!IpWarmup::CanonicalAddress(_T("192.0.2.300"), canonical), "An address that is not one was accepted.");
      Expect(!IpWarmup::CanonicalAddress(_T("mail.example.com"), canonical), "A host name was accepted as an address.");
      Expect(!IpWarmup::CanonicalAddress(_T("0.0.0.0"), canonical), "The unspecified address was accepted.");
      Expect(!IpWarmup::CanonicalAddress(_T("::"), canonical), "The unspecified IPv6 address was accepted.");
      Expect(!IpWarmup::CanonicalAddress(_T(""), canonical), "An empty address was accepted.");
   }

   void
   WarmupPlanTester::TestProviders_()
   {
      Expect(OutboundProviders::ClassifyHost(_T("alt1.aspmx.l.google.com")) == OutboundProviders::Google, "A Google MX host is not Google's.");
      Expect(OutboundProviders::ClassifyHost(_T("ASPMX.L.GOOGLE.COM.")) == OutboundProviders::Google, "Case and the root's dot changed the answer.");
      Expect(OutboundProviders::ClassifyHost(_T("example-com.mail.protection.outlook.com")) == OutboundProviders::Microsoft, "An Exchange Online host is not Microsoft's.");
      Expect(OutboundProviders::ClassifyHost(_T("mta5.am0.yahoodns.net")) == OutboundProviders::Yahoo, "A Yahoo MX host is not Yahoo's.");
      Expect(OutboundProviders::ClassifyHost(_T("mx01.mail.icloud.com")) == OutboundProviders::Apple, "An iCloud MX host is not Apple's.");
      Expect(OutboundProviders::ClassifyHost(_T("mail.protonmail.ch")) == OutboundProviders::Proton, "A Proton MX host is not Proton's.");
      Expect(OutboundProviders::ClassifyHost(_T("notgoogle.com")) == OutboundProviders::Other, "A name that merely ends in a provider's letters was taken for it.");
      Expect(OutboundProviders::ClassifyHost(_T("mx.example.com")) == OutboundProviders::Other, "An ordinary host was taken for a provider.");
      Expect(OutboundProviders::ClassifyHost(_T("")) == OutboundProviders::Other, "No host name was taken for a provider.");

      Expect(OutboundProviders::ClassifyHost(_T("aspmx.l.google.com.evil.test")) == OutboundProviders::Other, "A provider's name inside another domain was taken for the provider.");

      OutboundProviders::Provider provider = OutboundProviders::Other;
      for (int index = 0; index < OutboundProviders::Count; index++)
      {
         Expect(OutboundProviders::FromKey(OutboundProviders::Key((OutboundProviders::Provider) index), provider) && (int) provider == index,
            "A provider's key does not read back as the provider.");
      }
      Expect(!OutboundProviders::FromKey(_T("gmail"), provider), "A key that is no provider was read as one.");
   }
}
