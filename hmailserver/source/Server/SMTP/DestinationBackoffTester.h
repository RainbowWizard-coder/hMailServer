// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

namespace HM
{
   // The per-destination back-off (DestinationBackoff.h), decided from values
   // and a clock the test moves: the signal table, the window, its doubling to
   // the ceiling, the probe and what ends it. Runs inside the service with the
   // other class tests, so it is small and certain.
   class DestinationBackoffTester
   {
   public:
      void Test();

   private:
      void TestSignals_();
      void TestWindowAndProbe_();
      void TestCeilingAndStaleProbe_();
      void TestListingAndClearing_();
   };
}
