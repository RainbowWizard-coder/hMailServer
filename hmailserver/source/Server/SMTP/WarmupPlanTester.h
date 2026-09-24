// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

namespace HM
{
   // The IP warm-up's arithmetic (IpWarmup.h, OutboundProviders.h), decided
   // from values alone: the three plans' ceilings on sample days, the other
   // providers' five times, the day a schedule is on, the dates and the
   // addresses the routes accept, and which provider a host is. Runs inside
   // the service with the other class tests; it touches no table.
   class WarmupPlanTester
   {
   public:
      void Test();

   private:
      void TestPlans_();
      void TestDays_();
      void TestDatesAndAddresses_();
      void TestProviders_();
   };
}
