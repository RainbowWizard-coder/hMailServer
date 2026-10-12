// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;

namespace RegressionTests.Shared
{
   public static class RetryableAssert
   {
      public static void AreEqual(int actual, int expected)
      {
         if (actual != expected) throw new Exception(string.Format("{0} does not equal {1}", actual, expected));
      }

      public static void IsTrue(bool value)
      {
         if (!value) throw new Exception("The value is false.");
      }

      public static void StringContains(string expected, string actual)
      {
         if (!actual.Contains(expected))
            throw new Exception(string.Format("{0} does not contain {1}", actual, expected));
      }

      // At most max. This was GreaterOrEqual(expected, actual), called by the one
      // caller - SessionLimitingTests.AssertMaxSessionCount - with the most it may be
      // as the expected value, so that a count that had not come down passed and only
      // one that had come down further failed (upstream 1cf0bcd63).
      public static void LessOrEqual(long max, long actual)
      {
         if (actual > max)
            throw new Exception(string.Format("{0} is greater than {1}", actual, max));
      }
   }
}