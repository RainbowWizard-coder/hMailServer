// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Shared;

namespace RegressionTests.Infrastructure.Persistence
{
   [TestFixture]
   public class Limitations : TestFixtureBase
   {
      [Test]
      public void TestDomainAliasRenameToSameNameAsDomain()
      {
         var domains = SingletonProvider<TestSetup>.Instance.GetApp().Domains;

         var da = _domain.DomainAliases.Add();
         da.AliasName = "test2.com";
         da.Save();

         try
         {
            da.AliasName = "example.test";
            da.Save();
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            return;
         }

         Assert.Fail("Domain alias with same name as domain was permitted.");
      }

      [Test]
      public void TestDomainAliasWithSameNameAsDomain()
      {
         var domains = SingletonProvider<TestSetup>.Instance.GetApp().Domains;

         var da = _domain.DomainAliases.Add();
         da.AliasName = "example.test";

         try
         {
            da.Save();
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            return;
         }

         Assert.Fail("Domain alias with same name as domain was permitted.");
      }

      [Test]
      public void TestDomainLimitAccountMaxSizeTest1()
      {
         _domain.MaxAccountSize = 25;
         _domain.Save();

         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "test1@example.test", "secret1", 1);
         account.Save();
      }

      [Test]
      public void TestDomainLimitAccountMaxSizeTest2()
      {
         _domain.MaxAccountSize = 25;
         _domain.Save();

         try
         {
            SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "test1@example.test", "secret1", 32);
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            return;
         }

         Assert.Fail("Max account size limit exceeded.");
      }

      [Test]
      public void TestDomainLimitAccountMaxSizeTest3()
      {
         _domain.MaxAccountSize = 25;
         _domain.Save();

         try
         {
            var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "test1@example.test", "secret1",
               0);
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            return;
         }

         Assert.Fail("Max account size limit exceeded (max size = 0)");
      }

      [Test]
      public void TestDomainLimitNumberOfAccounts()
      {
         _domain.MaxNumberOfAccountsEnabled = true;
         _domain.MaxNumberOfAccounts = 3;
         _domain.Save();

         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "test1@example.test", "secret1");
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "test2@example.test", "secret1");
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "test3@example.test", "secret1");

         try
         {
            SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "test4@example.test", "secret1");
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            return;
         }

         Assert.Fail("Number of accounts exceeded max no of accounts");
      }

      [Test]
      public void TestDomainLimitNumberOfAliases()
      {
         _domain.MaxNumberOfAliasesEnabled = true;
         _domain.MaxNumberOfAliases = 4;
         _domain.Save();

         SingletonProvider<TestSetup>.Instance.AddAlias(_domain, "test1@example.test", "test@example.test");
         SingletonProvider<TestSetup>.Instance.AddAlias(_domain, "test2@example.test", "test@example.test");
         SingletonProvider<TestSetup>.Instance.AddAlias(_domain, "test3@example.test", "test@example.test");
         SingletonProvider<TestSetup>.Instance.AddAlias(_domain, "test4@example.test", "test@example.test");

         try
         {
            SingletonProvider<TestSetup>.Instance.AddAlias(_domain, "test5@example.test", "test@example.test");
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            return;
         }

         Assert.Fail("Number of aliases exceeded max no of accounts");
      }

      [Test]
      public void TestDomainLimitNumberOfDistributionLists()
      {
         _domain.MaxNumberOfDistributionListsEnabled = true;
         _domain.MaxNumberOfDistributionLists = 2;
         _domain.Save();

         var recipients = new List<string>();
         SingletonProvider<TestSetup>.Instance.AddDistributionList(_domain, "test1@example.test", recipients);
         SingletonProvider<TestSetup>.Instance.AddDistributionList(_domain, "test2@example.test", recipients);

         try
         {
            SingletonProvider<TestSetup>.Instance.AddDistributionList(_domain, "test3@example.test", recipients);
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            return;
         }

         Assert.Fail("Number of aliases exceeded max no of accounts");
      }

      [Test]
      public void TestDomainMaxMessageSizeLimit()
      {
         _domain.MaxMessageSize = 0;
         _domain.Save();

         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "test@example.test", "secret1", 0);
         var message = new StringBuilder();

         for (var i = 0; i < 10000; i++) message.Append("ABCDEFGH");

         SmtpClientSimulator.StaticSend("test@example.test", "test@example.test", "TestSubject",
            message.ToString());
         Pop3ClientSimulator.AssertMessageCount("test@example.test", "secret1", 1);
         _domain.MaxMessageSize = 50;
         _domain.Save();

         CustomAsserts.Throws<DeliveryFailedException>(() => SmtpClientSimulator.StaticSend("test@example.test",
            "test@example.test", "TestSubject",
            message.ToString()));
      }


      [Test]
      public void TestDomainMaxSizeLimit()
      {
         _domain.MaxSize = 30;
         _domain.Save();

         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "test1@example.test", "secret1", 10);
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "test2@example.test", "secret1", 10);
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "test3@example.test", "secret1", 10);


         try
         {
            SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "test4@example.test", "secret1", 10);
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            return;
         }

         Assert.Fail("Max domain size limit exceeded.");
      }

      [Test]
      public void TestDomainMaxSizeLimitZeroSizeAccount()
      {
         _domain.MaxSize = 30;
         _domain.Save();

         try
         {
            SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "test1@example.test", "secret1", 0);
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            return;
         }

         Assert.Fail("Max domain size limit exceeded.");
      }

      [Test]
      public void TestDomainNameDuplicate()
      {
         var domains = SingletonProvider<TestSetup>.Instance.GetApp().Domains;
         SingletonProvider<TestSetup>.Instance.AddDomain(domains, "test2.com");

         try
         {
            SingletonProvider<TestSetup>.Instance.AddDomain(domains, "test2.com");
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            return;
         }

         Assert.Fail("Duplicate domain name was permitted.");
      }


      [Test]
      public void TestDomainNameDuplicateDomainRename()
      {
         var domains = SingletonProvider<TestSetup>.Instance.GetApp().Domains;
         var domain = SingletonProvider<TestSetup>.Instance.AddDomain(domains, "test2.com");

         domain.Name = "example.test";

         try
         {
            domain.Save();
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            return;
         }

         Assert.Fail("Duplicate domain name was permitted.");
      }

      [Test]
      public void TestDomainWithSameNameAsDomainAlias()
      {
         var domains = SingletonProvider<TestSetup>.Instance.GetApp().Domains;

         var da = _domain.DomainAliases.Add();
         da.AliasName = "test2.com";
         da.Save();

         try
         {
            SingletonProvider<TestSetup>.Instance.AddDomain(domains, "test2.com");
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            return;
         }

         Assert.Fail("Domain with same name as domain alias was permitted.");
      }

      [Test]
      [Description("Issue 195, Creating two SMTP routes allowed for same domain.")]
      public void TestDuplicateRoutes()
      {
         var app = SingletonProvider<TestSetup>.Instance.GetApp();
         var routes = app.Settings.Routes;

         var route = routes.Add();
         route.DomainName = "example.test";
         route.Save();

         var route2 = routes.Add();
         route2.DomainName = "example.test";

         var ex = Assert.Throws<COMException>(() => route2.Save());
         StringAssert.Contains("Another route with this name already exists.", ex.Message);
      }

      [Test]
      public void TestNameDuplicateAccount()
      {
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "test@example.test", "secret1");

         try
         {
            SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "test@example.test", "secret2");
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            return;
         }

         Assert.Fail("Duplicate account was permitted");
      }

      [Test]
      public void TestNameDuplicateAlias()
      {
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "test@example.test", "secret1");

         try
         {
            SingletonProvider<TestSetup>.Instance.AddAlias(_domain, "test@example.test", "secret2");
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            return;
         }

         Assert.Fail("Alias with same name as account was permitted.");
      }

      [Test]
      public void TestNameDuplicateDistributionList()
      {
         SingletonProvider<TestSetup>.Instance.AddAlias(_domain, "test@example.test", "test2@example.test");

         var recipients = new List<string>();

         try
         {
            SingletonProvider<TestSetup>.Instance.AddDistributionList(_domain, "test@example.test", recipients);
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            return;
         }

         Assert.Fail("Distribution list with same name as alias was permitted.");
      }

      [Test]
      public void TestNameDuplicateGroup()
      {
         SingletonProvider<TestSetup>.Instance.AddGroup("test@example.test");

         try
         {
            SingletonProvider<TestSetup>.Instance.AddGroup("test@example.test");
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            return;
         }

         Assert.Fail("Two groups with same name was permitted.");
      }

      [Test]
      public void TestSaveInvalidIPRange()
      {
         var app = SingletonProvider<TestSetup>.Instance.GetApp();
         var range = app.Settings.SecurityRanges.Add();

         range.Name = "Test";
         range.LowerIP = "1.1.1.1";
         range.UpperIP = "0.0.0.0";
         var ex = Assert.Throws<COMException>(() => range.Save());
         StringAssert.Contains("The lower IP address must be lower or the same as the upper IP address.", ex.Message);
      }


      [Test]
      public void TestSaveValidIPRange()
      {
         var app = SingletonProvider<TestSetup>.Instance.GetApp();
         var range = app.Settings.SecurityRanges.Add();

         range.Name = "Test";
         range.LowerIP = "0.0.0.0";
         range.UpperIP = "0.0.0.1";
         range.Save();
      }

      // Texts that are no address to the server on any platform since 6.3.6: the
      // older IPv4 forms Windows' own parser read until then (192.168.001.001 as
      // 192.168.1.1, 192.168.1 as 192.168.0.1, 10.1, hexadecimal, one number), an
      // address with a port or in brackets, and what was never an address.
      private static readonly string[] NotAddresses =
      {
         "192.168.001.001", "192.168.1", "10.1", "0x7f.0.0.1", "3232235777", "192.0.2.7:25", "[::1]", "[2001:db8::1]:25",
         "", "not-an-address"
      };

      [Test]
      [Description("An IP range's bound that is no address is refused over COM, with the reason, and the range keeps the address it had. Until 6.3.6 LowerIP and UpperIP ignored the parse and stored 0.0.0.0: a range whose lower bound was typed 192.168.001.001 - which Windows read as 192.168.1.1 until the server read an address the same way on every platform - began at 0.0.0.0, with whatever it allows.")]
      public void AnIPRangeBoundThatIsNoAddressIsRefusedNotStoredAsZero()
      {
         var app = SingletonProvider<TestSetup>.Instance.GetApp();
         var range = app.Settings.SecurityRanges.Add();
         range.Name = "Bound refusal";
         range.LowerIP = "192.0.2.10";
         range.UpperIP = "192.0.2.20";

         foreach (string text in NotAddresses)
         {
            var lower = Assert.Throws<COMException>(() => range.LowerIP = text, "LowerIP took '" + text + "'.");
            StringAssert.Contains("LowerIP is not an IP address", lower.Message);
            var upper = Assert.Throws<COMException>(() => range.UpperIP = text, "UpperIP took '" + text + "'.");
            StringAssert.Contains("UpperIP is not an IP address", upper.Message);

            ClassicAssert.AreEqual("192.0.2.10", range.LowerIP, "LowerIP changed when '" + text + "' was refused.");
            ClassicAssert.AreEqual("192.0.2.20", range.UpperIP, "UpperIP changed when '" + text + "' was refused.");
         }

         // And an address in either family is still taken.
         range.LowerIP = "2001:db8::1";
         range.UpperIP = "2001:db8::ff";
         ClassicAssert.AreEqual("2001:db8::1", range.LowerIP);
         ClassicAssert.AreEqual("2001:db8::ff", range.UpperIP);
      }

      [Test]
      [Description("An incoming relay's bound that is no address is refused over COM, and the relay keeps the address it had - never 0.0.0.0, which until 6.3.6 made it a relay for every address below its upper bound.")]
      public void AnIncomingRelayBoundThatIsNoAddressIsRefusedNotStoredAsZero()
      {
         var app = SingletonProvider<TestSetup>.Instance.GetApp();
         var relay = app.Settings.IncomingRelays.Add();
         relay.Name = "Bound refusal";
         relay.LowerIP = "192.0.2.10";
         relay.UpperIP = "192.0.2.20";

         foreach (string text in NotAddresses)
         {
            var lower = Assert.Throws<COMException>(() => relay.LowerIP = text, "LowerIP took '" + text + "'.");
            StringAssert.Contains("LowerIP is not an IP address", lower.Message);
            var upper = Assert.Throws<COMException>(() => relay.UpperIP = text, "UpperIP took '" + text + "'.");
            StringAssert.Contains("UpperIP is not an IP address", upper.Message);

            ClassicAssert.AreEqual("192.0.2.10", relay.LowerIP, "LowerIP changed when '" + text + "' was refused.");
            ClassicAssert.AreEqual("192.0.2.20", relay.UpperIP, "UpperIP changed when '" + text + "' was refused.");
         }
      }

      [Test]
      [Description("A white-list entry's bound that is no address is refused over COM, and the entry keeps the address it had - never 0.0.0.0, which until 6.3.6 white-listed every address below the upper bound.")]
      public void AWhiteListBoundThatIsNoAddressIsRefusedNotStoredAsZero()
      {
         var app = SingletonProvider<TestSetup>.Instance.GetApp();
         var entry = app.Settings.AntiSpam.WhiteListAddresses.Add();
         entry.LowerIPAddress = "192.0.2.10";
         entry.UpperIPAddress = "192.0.2.20";

         foreach (string text in NotAddresses)
         {
            var lower = Assert.Throws<COMException>(() => entry.LowerIPAddress = text, "LowerIPAddress took '" + text + "'.");
            StringAssert.Contains("LowerIPAddress is not an IP address", lower.Message);
            var upper = Assert.Throws<COMException>(() => entry.UpperIPAddress = text, "UpperIPAddress took '" + text + "'.");
            StringAssert.Contains("UpperIPAddress is not an IP address", upper.Message);

            ClassicAssert.AreEqual("192.0.2.10", entry.LowerIPAddress, "LowerIPAddress changed when '" + text + "' was refused.");
            ClassicAssert.AreEqual("192.0.2.20", entry.UpperIPAddress, "UpperIPAddress changed when '" + text + "' was refused.");
         }
      }
   }
}