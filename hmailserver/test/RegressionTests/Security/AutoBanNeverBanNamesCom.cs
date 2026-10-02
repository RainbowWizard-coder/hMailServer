// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Runtime.InteropServices;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Shared;

namespace RegressionTests.Security
{
   /// <summary>
   ///    The never-ban list by reverse name over COM: the four members of Settings
   ///    the Control Panel's Auto-ban page uses - AutoBanNeverBanNames,
   ///    AddAutoBanNeverBanName, RemoveAutoBanNeverBanName and
   ///    CheckAutoBanNeverBanName. The behaviour of the list is held over REST and
   ///    SMTP by AutoBanNeverBanNames, which both suites run; this is the COM
   ///    surface alone, so it is not linked into the Linux suite.
   ///
   ///    The members are reached through dynamic, as SpamTraps reaches its own: the
   ///    suite's interop is generated from the registered type library, and naming
   ///    them early-bound would not compile against a bench whose server predates
   ///    them.
   /// </summary>
   [TestFixture]
   public class AutoBanNeverBanNamesCom : TestFixtureBase
   {
      private const string Pattern = ".nbn-com.test";

      [TearDown]
      public void TakeItBack()
      {
         dynamic settings = _settings;

         foreach (string pattern in new[] { Pattern, "mail.nbn-com.test" })
         {
            string json = (string) settings.AutoBanNeverBanNames;
            int at = json.IndexOf("\"pattern\":\"" + pattern + "\"", StringComparison.Ordinal);
            if (at < 0)
               continue;

            int idAt = json.LastIndexOf("\"id\":", at, StringComparison.Ordinal);
            int comma = json.IndexOf(',', idAt);
            settings.RemoveAutoBanNeverBanName(int.Parse(json.Substring(idAt + 5, comma - idAt - 5)));
         }

         SuiteDns.Reset();
      }

      [Test]
      [Description("A pattern is added and listed over COM, removed once, and the second removal says there was nothing to remove.")]
      public void TheListIsReachableOverCom()
      {
         dynamic settings = _settings;

         int id = (int) settings.AddAutoBanNeverBanName(Pattern, "added over COM");
         ClassicAssert.IsTrue(id > 0);

         string listed = (string) settings.AutoBanNeverBanNames;
         StringAssert.Contains("\"pattern\":\"" + Pattern + "\"", listed, listed);
         StringAssert.Contains("\"note\":\"added over COM\"", listed, listed);

         ClassicAssert.IsTrue((bool) settings.RemoveAutoBanNeverBanName(id), "The first removal did not say it removed the row.");
         ClassicAssert.IsFalse((bool) settings.RemoveAutoBanNeverBanName(id), "A row removed twice was removed twice.");

         listed = (string) settings.AutoBanNeverBanNames;
         StringAssert.DoesNotContain(Pattern, listed, listed);
      }

      [Test]
      [Description("COM refuses what the list may not hold with the same words REST uses, and adds nothing.")]
      public void ComRefusesWhatTheListMayNotHold()
      {
         dynamic settings = _settings;

         string before = (string) settings.AutoBanNeverBanNames;

         foreach (var refusal in new[] { new { pattern = ".com", says = "public suffix" },
                                         new { pattern = "192.0.2.1", says = "AutoBanNeverBan" },
                                         new { pattern = "*.nbn-com.test", says = Pattern } })
         {
            COMException thrown = Assert.Throws<COMException>(() => settings.AddAutoBanNeverBanName(refusal.pattern, ""));
            StringAssert.Contains(refusal.says, thrown.Message, refusal.pattern);
         }

         ClassicAssert.AreEqual(before, (string) settings.AutoBanNeverBanNames, "A refused pattern changed the list.");

         // The negative control: a host name under the same domain is accepted.
         int id = (int) settings.AddAutoBanNeverBanName("mail.nbn-com.test", "");
         ClassicAssert.IsTrue(id > 0);
      }

      [Test]
      [Description("The check over COM spares an address whose covered reverse name resolves back to it, does not spare one with no reverse name, and refuses text that is not an address.")]
      public void TheCheckIsReachableOverCom()
      {
         dynamic settings = _settings;
         settings.AddAutoBanNeverBanName(Pattern, "");

         SuiteDns.Zone.WithRaw("90.100.51.198.in-addr.arpa", 12, DnsWire.Name("mx90.nbn-com.test"));
         SuiteDns.Zone.WithA("mx90.nbn-com.test", "198.51.100.90");

         string spared = (string) settings.CheckAutoBanNeverBanName("198.51.100.90");
         StringAssert.Contains("\"spared\":true", spared, spared);
         StringAssert.Contains("\"name\":\"mx90.nbn-com.test\"", spared, spared);

         string nameless = (string) settings.CheckAutoBanNeverBanName("198.51.100.91");
         StringAssert.Contains("\"spared\":false", nameless, nameless);
         StringAssert.Contains("\"reverse_names\":[]", nameless, nameless);

         COMException thrown = Assert.Throws<COMException>(() => settings.CheckAutoBanNeverBanName("not an address"));
         StringAssert.Contains("IPv4 or IPv6", thrown.Message);
      }
   }
}
