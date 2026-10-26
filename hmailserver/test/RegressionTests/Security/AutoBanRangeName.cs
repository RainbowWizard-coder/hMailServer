// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Collections.Generic;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.Security
{
   /// <summary>
   ///    The name of the IP range a run of failed logons is banned with, while a range
   ///    already stands under the name the user name gives.
   ///
   ///    The name is "Auto-ban: " and the user name, made unique with " (1)" and the
   ///    like when a range holds it, in a column of 100 characters with a unique index.
   ///    Up to 6.3.7 the suffix was put after the whole user name and the name then cut
   ///    to 100, so that from a user name of 90 characters every name the server could
   ///    choose was the same once cut: while one ban for that user name stood, the
   ///    unique index refused every other, HM6099 was reported and the address that
   ///    had earned the ban was not banned - its spent failures cleared all the same,
   ///    so it was given the next set of attempts. And the check for a name in use
   ///    compared a literal that SQL Server reads in the database's code page, so a
   ///    user name with characters outside it collided at any length.
   ///
   ///    Each test stands a range of its own under the name a ban for the user name
   ///    takes, at an address of TEST-NET-1 that nothing connects from, then fails
   ///    the threshold's logons with that user name from this machine, and finds this
   ///    machine banned under the next name. Every range the fixture made is deleted
   ///    again when each test ends, and the logon failures are cleared.
   ///
   ///    The logons are POP3's AUTH PLAIN, which carries the user name in UTF-8 as
   ///    written (RFC 4616), and no account has these names. Over COM on Windows and
   ///    REST on Linux alike: the Linux suite links this fixture. A build that cannot
   ///    prepare a user name outside ASCII for SASL (the Linux build: HM6450) never
   ///    lets such a name reach the auto-ban, and the second test says so and is
   ///    ignored there; its shape was SQL Server's alone.
   /// </summary>
   [TestFixture]
   public class AutoBanRangeName : TestFixtureBase
   {
      private const string Prefix = "Auto-ban: ";
      private const string StandingAddress = "192.0.2.77";
      private const int Threshold = 3;

      [SetUp]
      public void TurnAutoBanOn()
      {
         _settings.ClearLogonFailureList();
         _settings.AutoBanOnLogonFailure = true;
         _settings.MaxInvalidLogonAttempts = Threshold;
         _settings.MaxInvalidLogonAttemptsWithin = 5;
         _settings.AutoBanMinutes = 3;
      }

      [TearDown]
      public void RemoveTheRangesMade()
      {
         // The standing range and the ban, and any other range named as an auto-ban:
         // TestSetup leaves none before a test.
         var ranges = _settings.SecurityRanges;
         for (var i = ranges.Count - 1; i >= 0; i--)
         {
            var range = ranges[i];
            if (range.Name.StartsWith(Prefix))
               ranges.DeleteByDBID(range.ID);
         }

         _settings.ClearLogonFailureList();
         _settings.AutoBanOnLogonFailure = false;
      }

      [Test]
      [Description("A user name of 120 characters: a range stands under the first 100 characters of its " +
                   "ban's name, and the threshold's failed logons ban this machine under \"Auto-ban: \", " +
                   "86 characters of the user name and \" (1)\".")]
      public void ALongUserNameIsBannedWhileARangeStandsUnderItsName()
      {
         var userName = new string('a', 107) + "@example.test";
         var full = Prefix + userName;

         StandRange(full, full.Substring(0, 100));

         FailLogons(userName);

         AssertBannedAs(Prefix + userName.Substring(0, 86) + " (1)");
      }

      [Test]
      [Description("A user name of Cyrillic and Chinese characters, outside the Latin code pages: a range " +
                   "stands under its ban's name, and the threshold's failed logons ban this machine under " +
                   "the name and \" (1)\".")]
      public void AUserNameOutsideTheCodePageIsBannedWhileARangeStandsUnderItsName()
      {
         // Ivan.Petrov in Cyrillic and two Chinese characters, written with escapes so that the source is ASCII.
         var userName = "\u0418\u0432\u0430\u043D.\u041F\u0435\u0442\u0440\u043E\u0432\u7528\u6237@example.test";
         var full = Prefix + userName;

         StandRange(full, full);

         FailLogons(userName);

         AssertBannedAs(full + " (1)");
      }

      /// <summary>
      ///    The range a ban for the user name stood under, read back as the server
      ///    stored it - what the test stands on, checked before anything else.
      /// </summary>
      private void StandRange(string name, string expectedStoredName)
      {
         var range = _settings.SecurityRanges.Add();
         range.Name = name;
         range.LowerIP = StandingAddress;
         range.UpperIP = StandingAddress;
         range.Priority = 100;
         range.Expires = true;
         range.ExpiresTime = System.DateTime.Now.AddMinutes(30);
         range.Save();

         ClassicAssert.Contains(expectedStoredName, RangeNames(),
            "The range the test stands on is not stored under the name a ban for the user name takes.");
      }

      private void FailLogons(string userName)
      {
         for (var i = 1; i <= Threshold; i++)
         {
            var simulator = new Pop3ClientSimulator();
            string reply;
            ClassicAssert.IsFalse(simulator.ConnectAndLogonPlain(userName, "not-the-password", out reply),
               "Failed logon " + i + " was not refused: " + reply);
            simulator.Disconnect();

            // A build without Unicode normalisation refuses a credential outside ASCII
            // before the password is looked at (SASLprep, HM6450): such a name never
            // reaches the auto-ban there, which is what the Linux build does today, and
            // on its engines the check for a name in use was never a code page's.
            if (i == 1 && reply.Contains("Invalid SASL PLAIN response"))
            {
               CustomAsserts.AssertReportedError("HM6450");
               Assert.Ignore("This build cannot prepare a user name outside ASCII for SASL (HM6450), so no such name reaches the auto-ban: " + reply.Trim());
            }
         }
      }

      private void AssertBannedAs(string expectedName)
      {
         var names = RangeNames();

         ClassicAssert.Contains(expectedName, names,
            "No range was written under the next name. The ranges: " + string.Join(" | ", names));

         ClassicAssert.AreEqual("", new Pop3ClientSimulator().GetWelcomeMessage(),
            "This machine was given a greeting: it is not banned.");

         CustomAsserts.AssertNoReportedError();
      }

      private List<string> RangeNames()
      {
         var names = new List<string>();
         var ranges = _settings.SecurityRanges;
         for (var i = 0; i < ranges.Count; i++)
            names.Add(ranges[i].Name);
         return names;
      }
   }
}
