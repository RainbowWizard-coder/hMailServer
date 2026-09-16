// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Runtime.InteropServices;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Shared;
using hMailServer;

namespace RegressionTests.API
{
   /// <summary>
   ///    The reputation watch over COM: the three settings on Settings, and the
   ///    report and the check on Utilities.
   ///
   ///    The Control Panel and every script an administrator has written reach the
   ///    server this way, so the refusals have to be here too: a list or an address
   ///    the watch cannot use is refused where it is typed, in the same words the
   ///    REST settings answer with.
   ///
   ///    Not linked into the Linux suite, which has no COM.
   /// </summary>
   [TestFixture]
   public class ReputationWatchCom : TestFixtureBase
   {
      private string savedLists_;
      private string savedAddresses_;
      private bool savedEnabled_;

      [SetUp]
      public void RememberTheSettings()
      {
         savedEnabled_ = ComEnabled(_settings);
         savedLists_ = ComLists(_settings);
         savedAddresses_ = ComAddresses(_settings);
      }

      [TearDown]
      public void PutTheSettingsBack()
      {
         ComSetEnabled(_settings, savedEnabled_);
         ComSetLists(_settings, savedLists_);
         ComSetAddresses(_settings, savedAddresses_);

         _application.Database.ExecuteSQL("delete from hm_reputationchecks");
      }

      [Test]
      [Description("The three settings round-trip over COM, which is how the Control Panel writes them.")]
      public void TheSettingsRoundTrip()
      {
         ComSetEnabled(_settings, false);
         Assert.IsFalse(ComEnabled(_settings));

         ComSetEnabled(_settings, true);
         Assert.IsTrue(ComEnabled(_settings));

         ComSetLists(_settings, "ipv4:bl.example.test=127.0.0.2|127.0.0.4-7, other.example.test");
         Assert.AreEqual("ipv4:bl.example.test=127.0.0.2|127.0.0.4-7, other.example.test", ComLists(_settings));

         ComSetAddresses(_settings, "192.0.2.25 2001:db8::25");
         Assert.AreEqual("192.0.2.25 2001:db8::25", ComAddresses(_settings));
      }

      [Test]
      [Description("A list that is not a zone, a listing code that is not an address, and a host name where an address belongs are each refused with the reason.")]
      public void WhatTheWatchCannotUseIsRefused()
      {
         COMException lists = Assert.Throws<COMException>(() => ComSetLists(_settings, "not a zone!"));
         StringAssert.Contains("bl.spamcop.net", lists.Message, "The refusal shows what a list looks like.");

         COMException codes = Assert.Throws<COMException>(() => ComSetLists(_settings, "bl.example.test=127.0.0"));
         StringAssert.Contains("127.0.0.0/8", codes.Message);

         COMException addresses = Assert.Throws<COMException>(() => ComSetAddresses(_settings, "mail.example.com"));
         StringAssert.Contains("IP address", addresses.Message);

         // And nothing was stored: a refusal that leaves the value behind is worse
         // than no refusal, because the next check would use it.
         Assert.AreNotEqual("not a zone!", ComLists(_settings));
         Assert.AreNotEqual("mail.example.com", ComAddresses(_settings));
      }

      [Test]
      [Description("Utilities answers the report without looking anything up, and a check makes the lookups and says what it did.")]
      public void TheReportAndTheCheckAnswerOverCom()
      {
         ComSetEnabled(_settings, true);
         ComSetLists(_settings, "report.example.test");
         ComSetAddresses(_settings, "192.0.2.25");

         string report = ComGetReport(_application.Utilities);
         StringAssert.Contains("\"addresses\"", report);
         StringAssert.Contains("\"lists\"", report);

         string checkedReport = ComRunCheck(_application.Utilities);
         StringAssert.Contains("\"run\"", checkedReport, "A check says what it did.");
         StringAssert.Contains("192.0.2.25", checkedReport, "And what it checked.");

         // The list published nothing at all, so the watch says it is not answering
         // rather than that the address is clean.
         StringAssert.Contains("list_not_answering", checkedReport);
         StringAssert.DoesNotContain("\"status\":\"not_listed\"", checkedReport);
      }

      // The reputation watch's members of Settings and Utilities are newer than the
      // type library build-tests.ps1 compiles against until an elevated
      // build/register-com.ps1 has run, so they bind late, as the DKIM key
      // rotation's do; a refusal still arrives as the COMException each test expects.
      private static bool ComEnabled(Settings settings) => (bool) ((dynamic) settings).ReputationWatchEnabled;

      private static void ComSetEnabled(Settings settings, bool value) => ((dynamic) settings).ReputationWatchEnabled = value;

      private static string ComLists(Settings settings) => (string) ((dynamic) settings).ReputationWatchLists;

      private static void ComSetLists(Settings settings, string value) => ((dynamic) settings).ReputationWatchLists = value;

      private static string ComAddresses(Settings settings) => (string) ((dynamic) settings).ReputationWatchAddresses;

      private static void ComSetAddresses(Settings settings, string value) => ((dynamic) settings).ReputationWatchAddresses = value;

      private static string ComGetReport(object utilities) => (string) ((dynamic) utilities).GetReputationReport();

      private static string ComRunCheck(object utilities) => (string) ((dynamic) utilities).RunReputationCheck();
   }
}
