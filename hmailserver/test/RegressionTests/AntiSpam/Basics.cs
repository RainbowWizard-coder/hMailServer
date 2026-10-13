// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Text;
using NUnit.Framework;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.AntiSpam
{
   [TestFixture]
   public class Basics : TestFixtureBase
   {
      // The URI blacklist this fixture tests against, served locally.
      //
      // Every assertion here except one wants a NEGATIVE answer - "0 addresses found,
      // Match: False" - and a lookup that times out produces the same shape of
      // non-answer as one that completes, so the fixture could pass while proving
      // nothing. The one positive is the SURBL project's permanent test point, and it
      // is the reason the fixture used to fail at random: on 19 August 2026
      // TestMaxSizeNoLimit reported "Spam message not detected as spam" after ten
      // seconds because the lookup had not come back, which says nothing about the
      // size limit it is named for.
      //
      // 127.0.0.2 is what a URI blacklist returns for a listed name.
      private const string SurblTestPoint = "surbl-org-permanent-test-point.com.multi.surbl.org";

      [OneTimeSetUp]
      public void ServeTheNamesThisFixtureNeeds()
      {
         SuiteDns.Zone
            .WithA(SurblTestPoint, "127.0.0.2")
            // What the Spamhaus zones answer a public resolver - a refusal of the
            // query, not a listing - and a DBL-style listing in 127.0.1.0/24.
            .WithA("open-resolver-code.test.multi.surbl.org", "127.255.255.254")
            .WithA("dbl-listed.test.multi.surbl.org", "127.0.1.2")
            // TestMissingMXRecord needs one domain that HAS an MX and one that does
            // not, and it uses microsoft.com for the first. Served, because the
            // negative half of that test only means something if the positive half
            // reaches the server at all.
            .WithMx("microsoft.com", 10, "mx.microsoft.test")
            .WithA("mx.microsoft.test", "192.0.2.40")
            // The names a host is cut to by the Public Suffix List, under a suffix
            // of two labels (co.uk) and one of three (act.edu.au): listed for
            // SurblAsksAboutTheDomainRegisteredUnderAPublicSuffix, and with an MX
            // for MxCheckFallsBackToTheDomainRegisteredUnderAPublicSuffix. Nothing
            // is served for the hosts under them.
            .WithA("surbl-cut.co.uk.multi.surbl.org", "127.0.0.2")
            .WithA("surbl-cut.act.edu.au.multi.surbl.org", "127.0.0.2")
            .WithMx("mx-cut.act.edu.au", 10, "mx.mx-cut.test")
            .WithA("mx.mx-cut.test", "192.0.2.41")
            // SurblLooksUpTheHostAfterTheUserinfo: the host after a link's
            // userinfo, listed. Nothing is served for the names before the '@'.
            .WithA("surbl-userinfo.test.multi.surbl.org", "127.0.0.2")
            // SurblListErrorCodesAreNotListings: what the Spamhaus DBL answers
            // every address it is asked about, in either order - 127.0.1.255, IP
            // queries prohibited - and what SURBL and URIBL answer a query they
            // block, 127.0.0.1.
            .WithA("44.2.0.192.multi.surbl.org", "127.0.1.255")
            .WithA("192.0.2.44.multi.surbl.org", "127.0.1.255")
            .WithA("surbl-blocked.test.multi.surbl.org", "127.0.0.1")
            // SurblAsksAboutAnAddressReversedOnly: 192.0.2.45 is listed, which a
            // list is asked about as 45.2.0.192; and 192.0.2.46.multi.surbl.org is
            // served, which is the list saying 46.2.0.192 is listed.
            .WithA("45.2.0.192.multi.surbl.org", "127.0.0.2")
            .WithA("192.0.2.46.multi.surbl.org", "127.0.0.2")
            // SurblBlockedAnswerDoesNotSwitchTheListOff: what the Spamhaus ZRD
            // answers every address it is asked about - 127.0.2.255, IP queries
            // not supported - for 192.0.2.47.
            .WithA("47.2.0.192.multi.surbl.org", "127.0.2.255")
            // SurblAsksAboutTheIcannDomainOfAHostUnderAPrivateSuffix: a whole
            // hosting domain listed, as URIBL, which is not wildcarded, lists
            // one. Nothing is served for its tenants' names.
            .WithA("duckdns.org.multi.surbl.org", "127.0.0.2")
            // SurblReadsAHostThatIsNotAsciiAsABrowserDoes: an internationalised
            // name, listed in its xn-- form, which is how DNS carries it.
            .WithA("xn--bcher-kva.test.multi.surbl.org", "127.0.0.2");
      }

      [OneTimeTearDown]
      public void ForgetThem()
      {
         SuiteDns.Reset();
      }

      [SetUp]
      public new void SetUp()
      {
         _antiSpam = _settings.AntiSpam;
      }

      private hMailServer.AntiSpam _antiSpam;

      [Test]
      public void TestDNSBlackList()
      {
         var dnsBlackLists = SingletonProvider<TestSetup>.Instance.GetApp().Settings.AntiSpam.DNSBlackLists;

         var dnsBlackList = dnsBlackLists.Add();
         dnsBlackList.DNSHost = "zen.spamhaus.org";
         dnsBlackList.RejectMessage = "srv1";
         dnsBlackList.Score = 5;
         dnsBlackList.Active = true;
         dnsBlackList.Save();

         dnsBlackList = dnsBlackLists.Add();
         dnsBlackList.DNSHost = "bl.spamcop.net";
         dnsBlackList.RejectMessage = "srv2";
         dnsBlackList.Score = 5;
         dnsBlackList.Active = false;
         dnsBlackList.Save();

         dnsBlackList = dnsBlackLists.Add();
         dnsBlackList.DNSHost = "dnsbl.njabl.org";
         dnsBlackList.RejectMessage = "srv3";
         dnsBlackList.Score = 5;
         dnsBlackList.Active = true;
         dnsBlackList.Save();

         var application = SingletonProvider<TestSetup>.Instance.GetApp();
         _antiSpam.SpamMarkThreshold = 1;
         _antiSpam.SpamDeleteThreshold = 100;

         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "dnsbltest@example.test", "test");

         LogHandler.DeleteCurrentDefaultLog();

         SmtpClientSimulator.StaticSend(account.Address, account.Address, "Test", "TestBody");
         Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test");

         var result = LogHandler.ReadCurrentDefaultLog();

         Assert.IsTrue(result.Contains(".zen.spamhaus.org, 0 addresses found: (none), Match: False"), result);
         Assert.IsTrue(result.Contains(".dnsbl.njabl.org, 0 addresses found: (none), Match: False"), result);
         Assert.IsFalse(result.Contains(".bl.spamcop.net, 0 addresses found:"), result);
      }

      [Test]
      public void TestHeloSpamTest()
      {
         var application = SingletonProvider<TestSetup>.Instance.GetApp();

         var account1 = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "test@example.test", "test");

         // Disallow incorrect line endings.
         _antiSpam.SpamDeleteThreshold = 100;

         _antiSpam.CheckHostInHelo = true;
         _antiSpam.CheckHostInHeloScore = 125;

         // Send a messages to this account.
         var smtpClientSimulator = new SmtpClientSimulator();

         CustomAsserts.Throws<DeliveryFailedException>(() =>
            smtpClientSimulator.Send("whitelist@microsoft.com", "whitelist@example.test", "SURBL-Match", "Test"));
      }

      [Test]
      public void TestIncorrectLineEndings()
      {
         // Create a test account
         // Fetch the default domain
         var account1 = SingletonProvider<TestSetup>.Instance.AddAccount(_domain,
            "SpamProtectionLineEndings@example.test",
            "test");

         // Disallow incorrect line endings.
         SingletonProvider<TestSetup>.Instance.GetApp().Settings.AllowIncorrectLineEndings = false;

         // Send a messages to this account.

         var smtpClientSimulator = new SmtpClientSimulator();
         smtpClientSimulator.Send("SpamProtectionLineEndings@example.test", "SpamProtectionLineEndings@example.test",
            "INBOX",
            "This is a test message\r\n consisting of correct lines");

         CustomAsserts.Throws<DeliveryFailedException>(() => smtpClientSimulator.Send(
            "SpamProtectionLineEndings@example.test",
            "SpamProtectionLineEndings@example.test", "INBOX",
            "This is a test message\r consisting of incorrect lines"));

         CustomAsserts.Throws<DeliveryFailedException>(
            () => smtpClientSimulator.Send("SpamProtectionLineEndings@example.test",
               "SpamProtectionLineEndings@example.test", "INBOX",
               "This is a test message\n consisting of incorrect lines"));

         CustomAsserts.Throws<DeliveryFailedException>(() => smtpClientSimulator.Send(
            "SpamProtectionLineEndings@example.test",
            "SpamProtectionLineEndings@example.test", "INBOX",
            "This is a test message\n\r consisting of incorrect lines"));

         Pop3ClientSimulator.AssertMessageCount(account1.Address, "test", 1);
      }

      [Test]
      public void TestMaxSizeLimit()
      {
         var application = SingletonProvider<TestSetup>.Instance.GetApp();
         // Create a test account
         // Fetch the default domain
         var account1 = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "surbltest@example.test", "test");

         // Disallow incorrect line endings.
         _antiSpam.SpamMarkThreshold = 1;
         _antiSpam.SpamDeleteThreshold = 100;
         _antiSpam.AddHeaderReason = true;
         _antiSpam.AddHeaderSpam = true;
         _antiSpam.PrependSubject = true;
         _antiSpam.PrependSubjectText = "ThisIsSpam";
         _antiSpam.MaximumMessageSize = 40;

         // Enable SURBL.
         var surblServer = _antiSpam.SURBLServers[0];
         surblServer.Active = true;
         surblServer.Score = 5;
         surblServer.Save();

         // Send a messages to this account.
         var smtpClientSimulator = new SmtpClientSimulator();

         var sb = new StringBuilder();
         var iterations = 40 * 1024 / 100 + 1;
         for (var i = 0; i < iterations; i++)
            sb.Append(
               "1234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890\r\n");

         smtpClientSimulator.Send("surbltest@example.test", "surbltest@example.test", "SURBL-No-Match",
            "This is a test message with a SURBL url: -> http://surbl-org-permanent-test-point.com/ <-\r\n" +
            sb);

         var sMessageContents = Pop3ClientSimulator.AssertGetFirstMessageText(account1.Address, "test");
         if (sMessageContents.Contains("X-hMailServer-Spam") ||
             sMessageContents.Contains("ThisIsSpam"))
            throw new Exception("Spam message detected as spam even though it's larger than max spam size.");

         surblServer.Active = false;
         surblServer.Save();
      }

      [Test]
      public void TestMaxSizeNoLimit()
      {
         var application = SingletonProvider<TestSetup>.Instance.GetApp();
         // Create a test account
         // Fetch the default domain

         var account1 = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "surbltest@example.test", "test");

         // Disallow incorrect line endings.
         _antiSpam.SpamMarkThreshold = 1;
         _antiSpam.SpamDeleteThreshold = 100;
         _antiSpam.AddHeaderReason = true;
         _antiSpam.AddHeaderSpam = true;
         _antiSpam.PrependSubject = true;
         _antiSpam.PrependSubjectText = "ThisIsSpam";
         _antiSpam.MaximumMessageSize = 0;

         // Enable SURBL.
         var surblServer = _antiSpam.SURBLServers[0];
         surblServer.Active = true;
         surblServer.Score = 5;
         surblServer.Save();

         // Send a messages to this account.
         var smtpClientSimulator = new SmtpClientSimulator();

         var sb = new StringBuilder();
         var iterations = 40 * 1024 / 100 + 1;
         for (var i = 0; i < iterations; i++)
            sb.Append(
               "1234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890\r\n");

         smtpClientSimulator.Send("surbltest@example.test", "surbltest@example.test", "SURBL-No-Match",
            "This is a test message with a SURBL url: -> http://surbl-org-permanent-test-point.com/ <-\r\n" +
            sb);

         var sMessageContents = Pop3ClientSimulator.AssertGetFirstMessageText(account1.Address, "test");
         if (!sMessageContents.Contains("X-hMailServer-Spam") ||
             !sMessageContents.Contains("X-hMailServer-Reason") ||
             !sMessageContents.Contains("ThisIsSpam"))
            throw new Exception("Spam message not detected as spam.");

         surblServer.Active = false;
         surblServer.Save();
      }

      [Test]
      public void TestMissingMXRecord()
      {
         var application = SingletonProvider<TestSetup>.Instance.GetApp();

         // Create a test account
         // Fetch the default domain

         var account1 = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "missingmxrecords@example.test",
            "test");

         // Disallow incorrect line endings.

         _antiSpam.SpamDeleteThreshold = 1;
         _antiSpam.UseMXChecks = true;
         _antiSpam.UseMXChecksScore = 2;

         // Send a messages to this account.
         var smtpClientSimulator = new SmtpClientSimulator();

         smtpClientSimulator.Send("test@microsoft.com", "missingmxrecords@example.test", "INBOX",
            "This is a test message.");

         CustomAsserts.Throws<DeliveryFailedException>(
            () => smtpClientSimulator.Send("test@domain_without_mx_records421dfsam430sasd.com", account1.Address,
               "INBOX",
               "This is a test message."));

         _antiSpam.UseMXChecks = false;

         Pop3ClientSimulator.AssertMessageCount(account1.Address, "test", 1);
      }

      [Test]
      public void MxCheckFallsBackToTheDomainRegisteredUnderAPublicSuffix()
      {
         // A sender's host with no MX of its own is judged by its registrable
         // domain's, cut by the Public Suffix List. mail.mx-cut.act.edu.au has none;
         // mx-cut.act.edu.au, registered under the three-label suffix act.edu.au,
         // has one, so the first message is taken. Until 6.3.6 the cut came from
         // SURBL's list of 2006, which knew edu.au and not act.edu.au: it asked
         // act.edu.au, which has no MX, and the message was refused. Nothing
         // answers for mail.no-mx-cut.act.edu.au or its registrable domain, and
         // that message is refused, so the first was taken by the fallback and not
         // by a check that did not run.
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "mx-cut@example.test", "test");

         _antiSpam.SpamDeleteThreshold = 1;
         _antiSpam.UseMXChecks = true;
         _antiSpam.UseMXChecksScore = 2;

         var smtpClientSimulator = new SmtpClientSimulator();

         smtpClientSimulator.Send("test@mail.mx-cut.act.edu.au", account.Address, "INBOX",
            "From a host whose registrable domain has an MX.");

         CustomAsserts.Throws<DeliveryFailedException>(
            () => smtpClientSimulator.Send("test@mail.no-mx-cut.act.edu.au", account.Address, "INBOX",
               "From a host whose registrable domain has none."));

         // A sender at a public suffix itself, which has no MX here, has no
         // registrable domain to fall back to and is refused: act.edu.au, and the
         // PRIVATE suffix github.io. The list of 2006 cut both to themselves and
         // refused them too; passing a name because it is a suffix would let a
         // sender at any of the list's names past the test.
         CustomAsserts.Throws<DeliveryFailedException>(
            () => smtpClientSimulator.Send("test@act.edu.au", account.Address, "INBOX",
               "From a public suffix of three labels."));

         CustomAsserts.Throws<DeliveryFailedException>(
            () => smtpClientSimulator.Send("test@github.io", account.Address, "INBOX",
               "From a PRIVATE public suffix."));

         _antiSpam.UseMXChecks = false;

         Pop3ClientSimulator.AssertMessageCount(account.Address, "test", 1);
      }

      [Test]
      public void TestMultiLineSurblImproperlyTerminatedLine()
      {
         // Create a test account
         // Fetch the default domain

         var account1 = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "surbltest@example.test", "test");

         // Disallow incorrect line endings.
         _antiSpam.SpamMarkThreshold = 1;
         _antiSpam.SpamDeleteThreshold = 100;
         _antiSpam.AddHeaderReason = true;
         _antiSpam.AddHeaderSpam = true;
         _antiSpam.PrependSubject = true;
         _antiSpam.PrependSubjectText = "ThisIsSpam";

         // Enable SURBL.
         var surblServer = _antiSpam.SURBLServers[0];
         surblServer.Active = true;
         surblServer.Score = 5;
         surblServer.Save();

         // Send a messages to this account.
         var smtpClientSimulator = new SmtpClientSimulator();

         smtpClientSimulator.Send("surbltest@example.test", "surbltest@example.test", "SURBL-Match",
            "Wrapped URL - <a href=3D\"http://surbl-org-perm\r\nanent-test-point.com\">Test</a>");

         var sMessageContents = Pop3ClientSimulator.AssertGetFirstMessageText(account1.Address, "test");
         Assert.IsFalse(sMessageContents.Contains("X-hMailServer-Spam"), "Non-spam message detected as spam");

         surblServer.Active = false;
         surblServer.Save();
      }

      // The SURBL server armed the way the tests above arm it, with an expected
      // result of the caller's, so that a test says what an answer must be.
      private hMailServer.SURBLServer ArmSurbl_(string expectedResult)
      {
         _antiSpam.SpamMarkThreshold = 1;
         _antiSpam.SpamDeleteThreshold = 100;
         _antiSpam.AddHeaderReason = true;
         _antiSpam.AddHeaderSpam = true;
         _antiSpam.PrependSubject = true;
         _antiSpam.PrependSubjectText = "ThisIsSpam";
         var surblServer = _antiSpam.SURBLServers[0];
         surblServer.Active = true;
         surblServer.Score = 5;
         surblServer.ExpectedResult = expectedResult;
         surblServer.Save();
         return surblServer;
      }

      [Test]
      public void SurblResolverRefusalCodeIsNotAListing()
      {
         // 127.255.255.254 is what the Spamhaus zones answer a query made through a
         // public resolver: the query was refused, the name is not listed. Taken as
         // a listing it tagged every message with a link (discussion #167).
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "surbl-refusal@example.test", "test");
         var surblServer = ArmSurbl_("");
         new SmtpClientSimulator().Send("surbl-refusal@example.test", "surbl-refusal@example.test", "SURBL-Refusal",
            "A link: http://open-resolver-code.test/ here");
         var contents = Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test");
         Assert.IsFalse(contents.Contains("X-hMailServer-Spam"), "A resolver's refusal code was taken as a listing.");
         surblServer.Active = false;
         surblServer.Save();
      }

      [Test]
      public void SurblAsksAboutTheDomainRegisteredUnderAPublicSuffix()
      {
         // A link's host is looked up whole and as its registrable domain, cut by
         // the Public Suffix List. Only the cut names are listed here, so a message
         // is tagged only when the cut kept the label registered under the suffix:
         // surbl-cut.co.uk under the two-label co.uk, and surbl-cut.act.edu.au
         // under the three-label act.edu.au. Until 6.3.6 the cut came from SURBL's
         // list of 2006, which knew edu.au and not act.edu.au, and the second link
         // was looked up as act.edu.au - a public suffix, which no list lists.
         var underTwo = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "surbl-cut-two@example.test", "test");
         var underThree = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "surbl-cut-three@example.test", "test");
         var surblServer = ArmSurbl_("");
         var smtp = new SmtpClientSimulator();

         smtp.Send(underTwo.Address, underTwo.Address, "SURBL-Cut-Two",
            "A link: http://a.b.surbl-cut.co.uk/page here");
         var underTwoText = Pop3ClientSimulator.AssertGetFirstMessageText(underTwo.Address, "test");
         Assert.IsTrue(underTwoText.Contains("X-hMailServer-Spam"),
            "a.b.surbl-cut.co.uk was not looked up as surbl-cut.co.uk, which is listed.");

         smtp.Send(underThree.Address, underThree.Address, "SURBL-Cut-Three",
            "A link: http://mail.surbl-cut.act.edu.au/page here");
         var underThreeText = Pop3ClientSimulator.AssertGetFirstMessageText(underThree.Address, "test");
         Assert.IsTrue(underThreeText.Contains("X-hMailServer-Spam"),
            "mail.surbl-cut.act.edu.au was not looked up as surbl-cut.act.edu.au, which is listed.");

         surblServer.Active = false;
         surblServer.Save();
      }

      [Test]
      public void SurblExpectedResultDecidesWhatCounts()
      {
         // With 127.0.1.0-255 expected, the permanent test point's 127.0.0.2 is not a
         // listing and a DBL-style 127.0.1.2 is - the same rule a DNSBL applies.
         var outside = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "surbl-outside@example.test", "test");
         var inside = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "surbl-inside@example.test", "test");
         var surblServer = ArmSurbl_("127.0.1.0-255");
         Assert.AreEqual("127.0.1.0-255", _antiSpam.SURBLServers[0].ExpectedResult, "The expected result did not round-trip.");
         var smtp = new SmtpClientSimulator();
         smtp.Send("surbl-outside@example.test", "surbl-outside@example.test", "SURBL-Outside",
            "A link: http://surbl-org-permanent-test-point.com/ here");
         var outsideText = Pop3ClientSimulator.AssertGetFirstMessageText(outside.Address, "test");
         Assert.IsFalse(outsideText.Contains("X-hMailServer-Spam"), "An answer outside the expected result counted as a listing.");
         smtp.Send("surbl-inside@example.test", "surbl-inside@example.test", "SURBL-Inside",
            "A link: http://dbl-listed.test/ here");
         var insideText = Pop3ClientSimulator.AssertGetFirstMessageText(inside.Address, "test");
         Assert.IsTrue(insideText.Contains("X-hMailServer-Spam"), "An answer inside the expected result was not a listing.");
         surblServer.ExpectedResult = "";
         surblServer.Active = false;
         surblServer.Save();
      }

      [Test]
      public void SurblLooksUpTheHostAfterTheUserinfo()
      {
         // A link's host follows the last '@' before its path, as RFC 3986 and
         // every browser read it. Until 6.3.6 the host was taken as the characters
         // after "://" that can be in a name, so the first link below was looked up
         // as innocent.example, and a spammer escaped every URI list by writing a
         // name and an '@' before the host. Only surbl-userinfo.test is listed.
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "surbl-userinfo@example.test", "test");
         var surblServer = ArmSurbl_("");
         var smtp = new SmtpClientSimulator();

         LogHandler.DeleteCurrentDefaultLog();

         var links = new[]
         {
            "http://innocent.example@surbl-userinfo.test/",
            "https://user:secret@surbl-userinfo.test:8443/path",
            "http://innocent.example%40mail@surbl-userinfo.test/",
            "http://a@innocent.example@surbl-userinfo.test/",
            "https://www.example.com/?u=http%3A%2F%2Finnocent.example%40surbl-userinfo.test%2F",
         };

         foreach (var link in links)
         {
            smtp.Send(account.Address, account.Address, "SURBL-Userinfo", "A link: " + link + " here");
            var text = Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test");
            Assert.IsTrue(text.Contains("X-hMailServer-Spam"), link + " was not looked up as surbl-userinfo.test, which is listed.");
         }

         Assert.IsTrue(LogHandler.DefaultLogContains("SURBL: Lookup: surbl-userinfo.test.multi.surbl.org"));
         Assert.IsFalse(LogHandler.DefaultLogContains("SURBL: Lookup: innocent.example.multi.surbl.org"),
            "A name in the userinfo, before the '@', was asked about.");

         surblServer.Active = false;
         surblServer.Save();
      }

      [Test]
      public void SurblListErrorCodesAreNotListings()
      {
         // The Spamhaus DBL answers 127.0.1.255, "IP queries prohibited", to every
         // address it is asked about, and SURBL and URIBL answer 127.0.0.1 to a
         // query they block. Neither is a listing, with no expected result or with
         // one that takes the code in: until 6.3.6 a server that asked the DBL with
         // either tagged every message holding a link to an address.
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "surbl-codes@example.test", "test");
         var smtp = new SmtpClientSimulator();

         LogHandler.DeleteCurrentDefaultLog();

         foreach (var expected in new[] { "", "127.0.1.0-255", "127.0.0.*|127.0.1.*" })
         {
            var surblServer = ArmSurbl_(expected);

            smtp.Send(account.Address, account.Address, "SURBL-Address", "A link to an address: http://192.0.2.44/page here");
            var addressText = Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test");
            Assert.IsFalse(addressText.Contains("X-hMailServer-Spam"),
               "127.0.1.255 was taken as a listing with the expected result '" + expected + "'.");

            smtp.Send(account.Address, account.Address, "SURBL-Blocked", "A link: http://surbl-blocked.test/ here");
            var blockedText = Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test");
            Assert.IsFalse(blockedText.Contains("X-hMailServer-Spam"),
               "127.0.0.1 was taken as a listing with the expected result '" + expected + "'.");
         }

         // Each is logged for what it is, once for its lookup.
         Assert.IsTrue(LogHandler.DefaultLogContains("SURBL: 127.0.1.255 is the Spamhaus DBL's code for an address"), LogHandler.ReadCurrentDefaultLog());
         Assert.IsTrue(LogHandler.DefaultLogContains("SURBL: 127.0.0.1 is SURBL's and URIBL's code for a blocked query"), LogHandler.ReadCurrentDefaultLog());

         // A listing under the same expected results is still one.
         var listing = ArmSurbl_("127.0.1.0-255");
         smtp.Send(account.Address, account.Address, "SURBL-Listed", "A link: http://dbl-listed.test/ here");
         var listedText = Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test");
         Assert.IsTrue(listedText.Contains("X-hMailServer-Spam"), "A DBL listing, 127.0.1.2, was not a listing.");

         ArmSurbl_("");
         smtp.Send(account.Address, account.Address, "SURBL-Listed", "A link: http://surbl-org-permanent-test-point.com/ here");
         listedText = Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test");
         Assert.IsTrue(listedText.Contains("X-hMailServer-Spam"), "The test point, 127.0.0.2, was not a listing.");

         listing.ExpectedResult = "";
         listing.Active = false;
         listing.Save();
      }

      [Test]
      public void SurblAsksAboutAnAddressReversedOnly()
      {
         // A DNS list is asked about an address with its octets reversed. Until
         // 6.3.6 a link's address was asked about in its own order too, which is
         // the address the other way round: a link to 192.0.2.46 was tagged
         // because 46.2.0.192 is listed. An address a browser reads in another
         // form - one number, a hexadecimal part - is the same address, and
         // 192.0.2.45 is listed.
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "surbl-address@example.test", "test");
         var surblServer = ArmSurbl_("");
         var smtp = new SmtpClientSimulator();

         LogHandler.DeleteCurrentDefaultLog();

         smtp.Send(account.Address, account.Address, "SURBL-Reversed", "A link: http://192.0.2.46/ here");
         var reversedText = Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test");
         Assert.IsFalse(reversedText.Contains("X-hMailServer-Spam"), "192.0.2.46 was asked about in its own order, which is 46.2.0.192.");
         Assert.IsTrue(LogHandler.DefaultLogContains("SURBL: Lookup: 46.2.0.192.multi.surbl.org"));
         Assert.IsFalse(LogHandler.DefaultLogContains("SURBL: Lookup: 192.0.2.46.multi.surbl.org"));

         foreach (var link in new[] { "http://192.0.2.45/", "http://3221226029/", "http://0xC0.0.2.45/", "http://user@192.0.2.45:8080/" })
         {
            smtp.Send(account.Address, account.Address, "SURBL-Address", "A link: " + link + " here");
            var text = Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test");
            Assert.IsTrue(text.Contains("X-hMailServer-Spam"), link + " was not looked up as 192.0.2.45, which is listed.");
         }

         surblServer.Active = false;
         surblServer.Save();
      }

      [Test]
      public void SurblNameDnsCannotCarryDoesNotEndTheCheck()
      {
         // Before 6.3.7 a lookup that failed abandoned the list for the message
         // (DnsLookupFailures has what it does now). Until 6.3.6 a
         // link's host went to the resolver as the message wrote it, and the names
         // were asked about in sorted order: a sentence ending in a link
         // ("sentence-end.example." asked as "sentence-end.example..multi.surbl.org"),
         // "http://../" or a label of 64 characters made a name the resolver
         // refuses, and every link sorted after it went unchecked.
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "surbl-names@example.test", "test");
         var surblServer = ArmSurbl_("");

         new SmtpClientSimulator().Send(account.Address, account.Address, "SURBL-Names",
            "See http://sentence-end.example. Or http://../ or http://" + new string('a', 64) + ".example/ - " +
            "then http://surbl-org-permanent-test-point.com/ here");

         var text = Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test");
         Assert.IsTrue(text.Contains("X-hMailServer-Spam"), "A link the resolver cannot be asked about hid the listed link after it.");

         surblServer.Active = false;
         surblServer.Save();
      }

      [Test]
      public void SurblBlockedAnswerDoesNotSwitchTheListOff()
      {
         // 127.0.0.1 is what SURBL and URIBL answer a query they block, and what
         // Hostkarma answers for a name on its whitelist. It is not a listing, and
         // it does not end the list for the message: were it to, a link to a
         // whitelisted name written first would switch the list off for every link
         // after it. And the Spamhaus ZRD answers every address with 127.0.2.255,
         // IP queries not supported, which is not a listing under the ZRD's own
         // range either.
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "surbl-blocked-first@example.test", "test");
         var smtp = new SmtpClientSimulator();

         var surblServer = ArmSurbl_("127.0.0.2");
         smtp.Send(account.Address, account.Address, "SURBL-Blocked-First",
            "First http://surbl-blocked.test/ then http://surbl-org-permanent-test-point.com/ here");
         var text = Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test");
         Assert.IsTrue(text.Contains("X-hMailServer-Spam"), "127.0.0.1 for the first link switched the list off for the listed link after it.");

         ArmSurbl_("127.0.2.0-255");
         smtp.Send(account.Address, account.Address, "SURBL-ZRD-Address", "A link to an address: http://192.0.2.47/page here");
         text = Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test");
         Assert.IsFalse(text.Contains("X-hMailServer-Spam"), "127.0.2.255, the ZRD's answer to every address, was taken as a listing.");

         surblServer.ExpectedResult = "";
         surblServer.Active = false;
         surblServer.Save();
      }

      [Test]
      public void SurblAsksAboutTheNameAMailProgramEndsALinkAt()
      {
         // In a text part a link is made by the mail program that shows it, and
         // Thunderbird ends one at a ')', a '"', a '`', a '{' or a '}' that a URL
         // parser takes into the userinfo: a click on
         // http://surbl-userinfo.test)@innocent.example/ there goes to
         // surbl-userinfo.test, which is asked about as well as the host after the
         // '@'. A link written into another's query with its slashes encoded as
         // backslashes, and a link whose scheme the reading of a long link before
         // it took in, are read too.
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "surbl-mail-program@example.test", "test");
         var surblServer = ArmSurbl_("");
         var smtp = new SmtpClientSimulator();

         LogHandler.DeleteCurrentDefaultLog();

         var bodies = new[]
         {
            "A link: http://surbl-userinfo.test)@innocent.example/ here",
            "A link: https://www.example.com/?u=http%3A%5C%5Csurbl-userinfo.test%2F here",
            "A link: http:" + new string('a', 300) + "!http://surbl-userinfo.test/ here",
         };

         foreach (var body in bodies)
         {
            smtp.Send(account.Address, account.Address, "SURBL-Mail-Program", body);
            var text = Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test");
            Assert.IsTrue(text.Contains("X-hMailServer-Spam"), body + " was not looked up as surbl-userinfo.test, which is listed.");
         }

         Assert.IsTrue(LogHandler.DefaultLogContains("SURBL: Lookup: innocent.example.multi.surbl.org"),
            "The host after the '@', where a browser goes, was not asked about.");

         surblServer.Active = false;
         surblServer.Save();
      }

      [Test]
      public void SurblAsksAboutAListedLinkBehindSixteenLinksInTheTextPart()
      {
         // Links put before a listed one kept it from every URI list. Until 6.3.6
         // the SURBL test read the text part's links and then the HTML part's, in
         // the order they came, and stopped at the sixteenth name: a
         // multipart/alternative message whose text part - which an HTML mail
         // program does not show - links to sixteen innocent domains had its
         // HTML part, and the one listed link in it, never read. Now every link of
         // both parts is read and the HTML part's domains are asked about first,
         // 48 names at most (SURBL::ChooseNamesToAsk), so the listed link is asked
         // about and the message tagged.
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "surbl-pad-text@example.test", "test");
         var surblServer = ArmSurbl_("");

         LogHandler.DeleteCurrentDefaultLog();

         var plain = new StringBuilder("The offers of the week:\r\n");
         for (var i = 1; i <= 16; i++)
            plain.Append("http://pad").Append(i.ToString("D2")).Append(".example/offer").Append(i).Append("\r\n");

         SmtpClientSimulator.StaticSendRaw(account.Address, account.Address,
            MultipartAlternativeMessage_(account.Address, "SURBL-Pad-Text", plain.ToString(),
               "<html><body><p>Our offer: <a href=\"http://surbl-org-permanent-test-point.com/\">click here</a></p></body></html>"));

         var text = Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test");
         Assert.IsTrue(text.Contains("X-hMailServer-Spam"),
            "The listed link in the HTML part was kept from the list by sixteen links in the text part.");
         Assert.IsTrue(LogHandler.DefaultLogContains("SURBL: Lookup: surbl-org-permanent-test-point.com.multi.surbl.org"),
            "The listed host was never asked about.");

         surblServer.Active = false;
         surblServer.Save();
      }

      [Test]
      public void SurblAsksAboutAListedLinkBehindSixteenHiddenLinksInTheHtmlPart()
      {
         // The same in one HTML part: sixteen links nobody sees (display:none) to
         // innocent domains, then the listed one. Until 6.3.6 the sixteen took the
         // whole budget in the order of the document and the listed link was
         // never asked about; now the seventeen domains are within the 48 names
         // asked, and the message is tagged.
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "surbl-pad-html@example.test", "test");
         var surblServer = ArmSurbl_("");

         LogHandler.DeleteCurrentDefaultLog();

         var html = new StringBuilder();
         for (var i = 1; i <= 16; i++)
            html.Append("<a href=\"http://pad").Append(i.ToString("D2")).Append(".example/\" style=\"display:none\">x</a>");
         html.Append("<p>Our offer: <a href=\"http://surbl-org-permanent-test-point.com/\">click here</a></p>");

         SmtpClientSimulator.StaticSendRaw(account.Address, account.Address,
            HtmlMessage_(account.Address, "SURBL-Pad-Html", html.ToString()));

         var text = Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test");
         Assert.IsTrue(text.Contains("X-hMailServer-Spam"),
            "The listed link was kept from the list by sixteen hidden links before it in the HTML part.");
         Assert.IsTrue(LogHandler.DefaultLogContains("SURBL: Lookup: surbl-org-permanent-test-point.com.multi.surbl.org"),
            "The listed host was never asked about.");

         surblServer.Active = false;
         surblServer.Save();
      }

      // A multipart/alternative message with a text/plain and a text/html part.
      private static string MultipartAlternativeMessage_(string address, string subject, string plain, string html)
      {
         const string boundary = "surbl-boundary-8f2c";
         return "From: " + address + "\r\n" +
                "To: " + address + "\r\n" +
                "Subject: " + subject + "\r\n" +
                "MIME-Version: 1.0\r\n" +
                "Content-Type: multipart/alternative; boundary=\"" + boundary + "\"\r\n" +
                "\r\n" +
                "--" + boundary + "\r\n" +
                "Content-Type: text/plain; charset=us-ascii\r\n" +
                "\r\n" +
                plain + "\r\n" +
                "--" + boundary + "\r\n" +
                "Content-Type: text/html; charset=us-ascii\r\n" +
                "\r\n" +
                html + "\r\n" +
                "--" + boundary + "--\r\n";
      }

      // An HTML message of one part, whose body is the given HTML.
      private static string HtmlMessage_(string address, string subject, string html)
      {
         return "From: " + address + "\r\n" +
                "To: " + address + "\r\n" +
                "Subject: " + subject + "\r\n" +
                "MIME-Version: 1.0\r\n" +
                "Content-Type: text/html; charset=us-ascii\r\n" +
                "\r\n" +
                "<html><body>" + html + "</body></html>\r\n";
      }

      [Test]
      public void SurblDeletesFromAHostWhatABrowserDeletes()
      {
         // A browser deletes the code points UTS 46 ignores from a host before it
         // looks the name up - a soft hyphen, a zero-width space, a zero-width
         // no-break space - and reads a host's percent-encoding as UTF-8. Until
         // 6.3.6 each byte past ASCII was U+FFFD and nothing was
         // deleted, so these links were no name at all, and a spammer hid a
         // listed name behind one character nobody sees. surbl-userinfo.test is
         // listed.
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "surbl-ignored@example.test", "test");
         var surblServer = ArmSurbl_("");
         var smtp = new SmtpClientSimulator();

         foreach (var link in new[]
         {
            "http://surbl-user%C2%ADinfo.test/",
            "http://surbl-userinfo%E2%80%8B.test/",
            "http://surbl-%EF%BB%BFuserinfo.test/",
         })
         {
            smtp.Send(account.Address, account.Address, "SURBL-Ignored", "A link: " + link + " here");
            var text = Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test");
            Assert.IsTrue(text.Contains("X-hMailServer-Spam"), link + " was not looked up as surbl-userinfo.test, which is listed.");
         }

         // In an HTML part, the named references for them too.
         SmtpClientSimulator.StaticSendRaw(account.Address, account.Address,
            HtmlMessage_(account.Address, "SURBL-Ignored-Html", "<a href=\"http://surbl-user&shy;info.test/\">a link</a>"));
         var html = Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test");
         Assert.IsTrue(html.Contains("X-hMailServer-Spam"), "&shy; in a link's host was not deleted, as a browser deletes it.");

         surblServer.Active = false;
         surblServer.Save();
      }

      [Test]
      public void SurblReadsAHostThatIsNotAsciiAsABrowserDoes()
      {
         // A browser reads a host that is not ASCII by UTS 46 before it asks DNS
         // for it - the URL standard's domain parser: fullwidth letters and their
         // full stop mapped to ASCII, an ideographic full stop to '.', capitals to
         // small letters, and a label in Unicode turned into its xn-- form. Until
         // 6.3.7 only the code points UTS 46 ignores were deleted, so a listed
         // name written in fullwidth letters was no name at all, and a link to an
         // internationalised name was asked about only as the labels after its
         // last label in Unicode: a spammer escaped every URI list by writing
         // the name so. surbl-userinfo.test and xn--bcher-kva.test (b, u with
         // diaeresis, cher) are listed.
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "surbl-uts46@example.test", "test");
         var surblServer = ArmSurbl_("");

         LogHandler.DeleteCurrentDefaultLog();

         var messages = new[]
         {
            new { What = "a link written in fullwidth letters, with a fullwidth hyphen and full stop", Type = "text/plain",
                  Body = "A link: http://\uFF53\uFF55\uFF52\uFF42\uFF4C\uFF0D\uFF55\uFF53\uFF45\uFF52\uFF49\uFF4E\uFF46\uFF4F\uFF0E\uFF54\uFF45\uFF53\uFF54/ here" },
            new { What = "an ideographic full stop", Type = "text/plain",
                  Body = "A link: http://surbl-userinfo\u3002test/ here" },
            new { What = "fullwidth capitals in an HTML link", Type = "text/html",
                  Body = "<html><body><a href=\"http://\uFF33\uFF35\uFF32\uFF22\uFF2C\uFF0D\uFF35\uFF33\uFF25\uFF32\uFF29\uFF2E\uFF26\uFF2F\uFF0E\uFF34\uFF25\uFF33\uFF34/\">a link</a></body></html>" },
            new { What = "an internationalised name", Type = "text/plain",
                  Body = "A link: http://b\u00FCcher.test/ here" },
         };

         foreach (var message in messages)
         {
            SmtpClientSimulator.StaticSendRaw(account.Address, account.Address,
               Utf8Message_(account.Address, "SURBL-UTS46", message.Type, message.Body));
            var text = Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test");
            Assert.IsTrue(text.Contains("X-hMailServer-Spam"), message.What + " was not looked up as the listed name a browser goes to.");
         }

         Assert.IsTrue(LogHandler.DefaultLogContains("SURBL: Lookup: surbl-userinfo.test.multi.surbl.org"),
            "The fullwidth link's host was not asked about as surbl-userinfo.test.");
         Assert.IsTrue(LogHandler.DefaultLogContains("SURBL: Lookup: xn--bcher-kva.test.multi.surbl.org"),
            "The internationalised name was not asked about in its xn-- form.");

         surblServer.Active = false;
         surblServer.Save();
      }

      [Test]
      public void SurblAsksAboutTheNameBeforePunctuationPastAscii()
      {
         // A link in running text followed straight away by punctuation past
         // ASCII - a closing quotation mark, CJK corner brackets, an ideographic
         // comma - ends there in the mail programs that make the link, while
         // UTS 46 takes the character into the host's last label, as valid to it
         // as a letter, or makes a label of what follows an ideographic full
         // stop. 6.3.6 asked about the name before the character; read by
         // UTS 46 alone, a listed name so followed was asked about only as, say,
         // surbl-userinfo.xn--test-jb7a, and escaped the list. Now it is asked
         // about both ways. surbl-userinfo.test is listed.
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "surbl-punctuation@example.test", "test");
         var surblServer = ArmSurbl_("");

         LogHandler.DeleteCurrentDefaultLog();

         var messages = new[]
         {
            new { What = "a link followed by a closing quotation mark", Type = "text/plain",
                  Body = "\u201CClaim it at http://surbl-userinfo.test\u201D today" },
            new { What = "a link in CJK corner brackets, an ideographic full stop after them", Type = "text/plain",
                  Body = "\u8BF7\u8BBF\u95EE\u300Chttp://surbl-userinfo.test\u300D\u3002" },
            new { What = "a link followed by an ideographic comma and CJK, in an HTML part's text", Type = "text/html",
                  Body = "<html><body><p>\u8BBF\u95EE http://surbl-userinfo.test\u3001\u8C22\u8C22</p></body></html>" },
         };

         foreach (var message in messages)
         {
            SmtpClientSimulator.StaticSendRaw(account.Address, account.Address,
               Utf8Message_(account.Address, "SURBL-Punctuation", message.Type, message.Body));
            var text = Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test");
            Assert.IsTrue(text.Contains("X-hMailServer-Spam"), message.What + " was not looked up as the listed name before the punctuation.");
         }

         Assert.IsTrue(LogHandler.DefaultLogContains("SURBL: Lookup: surbl-userinfo.test.multi.surbl.org"),
            "The name before the punctuation was not asked about.");

         surblServer.Active = false;
         surblServer.Save();
      }

      // A message of one part in UTF-8, base64-encoded, whose body is the given
      // text: the characters past ASCII reach the server as the sender wrote them.
      private static string Utf8Message_(string address, string subject, string contentType, string body)
      {
         return "From: " + address + "\r\n" +
                "To: " + address + "\r\n" +
                "Subject: " + subject + "\r\n" +
                "MIME-Version: 1.0\r\n" +
                "Content-Type: " + contentType + "; charset=utf-8\r\n" +
                "Content-Transfer-Encoding: base64\r\n" +
                "\r\n" +
                Convert.ToBase64String(Encoding.UTF8.GetBytes(body), Base64FormattingOptions.InsertLineBreaks) + "\r\n";
      }

      [Test]
      public void SurblReadsALinkWithNoScheme()
      {
         // An attribute's value that begins with two slashes is a link to the
         // host after them, with the scheme of the page it is on: from a webmail
         // page, https. So is a query parameter's value that begins with them -
         // a redirector's target. Until 6.3.6 neither was read.
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "surbl-no-scheme@example.test", "test");
         var surblServer = ArmSurbl_("");

         foreach (var html in new[]
         {
            "<a href=\"//surbl-userinfo.test/offer\">a link</a>",
            "<img src='\\\\surbl-userinfo.test/pixel.png'>",
            "<a href=\"http:&#10;//surbl-userinfo.test/\">a link the URL parser drops a line break from</a>",
         })
         {
            SmtpClientSimulator.StaticSendRaw(account.Address, account.Address, HtmlMessage_(account.Address, "SURBL-No-Scheme", html));
            var text = Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test");
            Assert.IsTrue(text.Contains("X-hMailServer-Spam"), html + " was not looked up as surbl-userinfo.test, which is listed.");
         }

         new SmtpClientSimulator().Send(account.Address, account.Address, "SURBL-No-Scheme",
            "A link: https://www.example.com/redirect?to=//surbl-userinfo.test/ here");
         var redirect = Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test");
         Assert.IsTrue(redirect.Contains("X-hMailServer-Spam"), "A redirector's target with no scheme was not looked up.");

         // Two slashes that open no value are no link: nothing is tagged.
         SmtpClientSimulator.StaticSendRaw(account.Address, account.Address,
            HtmlMessage_(account.Address, "SURBL-Slashes", "<p>see //surbl-userinfo.test/ in the text</p>"));
         var slashes = Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test");
         Assert.IsFalse(slashes.Contains("X-hMailServer-Spam"), "Two slashes in running text were taken for a link.");

         surblServer.Active = false;
         surblServer.Save();
      }

      [Test]
      public void SurblAsksAboutTheIPv4AddressAMappedIPv6LinkReaches()
      {
         // http://[::ffff:192.0.2.45]/ is an IPv4-mapped IPv6 address, which a
         // dual-stack system connects to as 192.0.2.45; a NAT64 prefix carries
         // one the same way. Until 6.3.6 no bracketed host was
         // asked about, and 192.0.2.45, which is listed, escaped the lists
         // behind brackets. Another IPv6 address is still not asked about.
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "surbl-mapped@example.test", "test");
         var surblServer = ArmSurbl_("");
         var smtp = new SmtpClientSimulator();

         LogHandler.DeleteCurrentDefaultLog();

         foreach (var link in new[] { "http://[::ffff:192.0.2.45]/", "http://[::FFFF:C000:022D]:8080/x", "http://[64:ff9b::192.0.2.45]/" })
         {
            smtp.Send(account.Address, account.Address, "SURBL-Mapped", "A link: " + link + " here");
            var text = Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test");
            Assert.IsTrue(text.Contains("X-hMailServer-Spam"), link + " was not looked up as 192.0.2.45, which is listed.");
         }

         smtp.Send(account.Address, account.Address, "SURBL-IPv6", "A link: http://[2001:db8::45]/ here");
         var ipv6 = Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test");
         Assert.IsFalse(ipv6.Contains("X-hMailServer-Spam"), "An IPv6 address that carries no IPv4 one was taken for one.");

         Assert.IsTrue(LogHandler.DefaultLogContains("SURBL: Lookup: 45.2.0.192.multi.surbl.org"));

         surblServer.Active = false;
         surblServer.Save();
      }

      [Test]
      public void SurblAsksAboutTheIcannDomainOfAHostUnderAPrivateSuffix()
      {
         // duckdns.org is a shared-hosting suffix of the Public Suffix List's
         // PRIVATE section, so evil.duckdns.org is its own registrable domain.
         // A list that is not wildcarded, URIBL, lists a whole hosting domain
         // under the name its ICANN section registers - here duckdns.org, the
         // only name served - and answers nothing for a tenant's name. 6.3.5,
         // which cut at two labels, asked about duckdns.org; the cut by the
         // whole list did not, until it also asked about the domain the ICANN
         // section alone gives.
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "surbl-icann@example.test", "test");
         var surblServer = ArmSurbl_("");

         LogHandler.DeleteCurrentDefaultLog();

         new SmtpClientSimulator().Send(account.Address, account.Address, "SURBL-ICANN", "A link: http://evil.duckdns.org/ here");
         var text = Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test");
         Assert.IsTrue(text.Contains("X-hMailServer-Spam"), "evil.duckdns.org was not looked up as duckdns.org, which is listed.");

         Assert.IsTrue(LogHandler.DefaultLogContains("SURBL: Lookup: evil.duckdns.org.multi.surbl.org"), "The host itself was not asked about first.");
         Assert.IsTrue(LogHandler.DefaultLogContains("SURBL: Found URL: evil.duckdns.org, asked about as duckdns.org"));

         surblServer.Active = false;
         surblServer.Save();
      }

      [Test]
      public void SurblReadsALinkWhereAScriptEndsItOrAParserDropsABreak()
      {
         // Three readings of a link the way the program that follows it reads
         // it. A webmail's script that ends a link at white space by its \s ends
         // one at U+FEFF, where a browser given the whole link deletes it: the
         // name before it is asked about too. An encoder that leaves '/' alone
         // writes a link in another's query as http%3A//. And the URL parser
         // deletes a tab from anywhere in an attribute's value, inside a link in
         // its query too. surbl-userinfo.test is listed; nothing else these
         // links name is.
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "surbl-ends@example.test", "test");
         var surblServer = ArmSurbl_("");

         SmtpClientSimulator.StaticSendRaw(account.Address, account.Address,
            HtmlMessage_(account.Address, "SURBL-Script-End", "<p>See http://surbl-userinfo.test&#xFEFF;more/ now</p>"));
         var scriptEnd = Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test");
         Assert.IsTrue(scriptEnd.Contains("X-hMailServer-Spam"), "The name before a zero-width no-break space was not asked about.");

         new SmtpClientSimulator().Send(account.Address, account.Address, "SURBL-Encoded-Slashes",
            "A link: https://www.example.com/redirect?to=http%3A//surbl-userinfo.test/offer here");
         var encoded = Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test");
         Assert.IsTrue(encoded.Contains("X-hMailServer-Spam"), "A link in a query whose slashes were not encoded was not read.");

         SmtpClientSimulator.StaticSendRaw(account.Address, account.Address,
            HtmlMessage_(account.Address, "SURBL-Tab", "<a href=\"https://www.example.com/redirect?to=http%3A%2F%2Fsurbl-user&#9;info.test%2F\">a link</a>"));
         var tab = Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test");
         Assert.IsTrue(tab.Contains("X-hMailServer-Spam"), "A tab in a link in an attribute's query hid the link.");

         surblServer.Active = false;
         surblServer.Save();
      }

      [Test]
      public void TestSURBL()
      {
         var application = SingletonProvider<TestSetup>.Instance.GetApp();
         // Create a test account
         // Fetch the default domain

         var account1 = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "surbltest@example.test", "test");

         // Disallow incorrect line endings.
         _antiSpam.SpamMarkThreshold = 1;
         _antiSpam.SpamDeleteThreshold = 100;
         _antiSpam.AddHeaderReason = true;
         _antiSpam.AddHeaderSpam = true;
         _antiSpam.PrependSubject = true;
         _antiSpam.PrependSubjectText = "ThisIsSpam";

         // Enable SURBL.
         var surblServer = _antiSpam.SURBLServers[0];
         surblServer.Active = true;
         surblServer.Score = 5;
         surblServer.Save();

         // Send a messages to this account.
         var smtpClientSimulator = new SmtpClientSimulator();

         smtpClientSimulator.Send("surbltest@example.test", "surbltest@example.test", "SURBL-No-Match",
            "This is a test message without a SURBL url.");

         var sMessageContents = Pop3ClientSimulator.AssertGetFirstMessageText(account1.Address, "test");
         if (sMessageContents.Length == 0 ||
             sMessageContents.Contains("X-hMailServer-Spam") ||
             sMessageContents.Contains("ThisIsSpam"))
            throw new Exception("Non-Spam message detected as spam");

         smtpClientSimulator.Send("surbltest@example.test", "surbltest@example.test", "SURBL-Match",
            "This is a test message with a SURBL url: -> http://surbl-org-permanent-test-point.com/ <-");

         sMessageContents = Pop3ClientSimulator.AssertGetFirstMessageText(account1.Address, "test");
         if (!sMessageContents.Contains("X-hMailServer-Spam") ||
             !sMessageContents.Contains("ThisIsSpam"))
            throw new Exception("Spam message not detected as spam");

         surblServer.Active = false;
         surblServer.Save();
      }

      [Test]
      public void TestSURBLCombinedWithSignature()
      {
         var application = SingletonProvider<TestSetup>.Instance.GetApp();
         // Create a test account
         // Fetch the default domain

         _domain.SignatureEnabled = true;
         _domain.SignaturePlainText = "MyDomainSignature";
         _domain.AddSignaturesToLocalMail = true;
         _domain.Save();

         var account1 = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "surbltest@example.test", "test");

         // Disallow incorrect line endings.
         _antiSpam.SpamMarkThreshold = 1;
         _antiSpam.SpamDeleteThreshold = 100;
         _antiSpam.AddHeaderReason = true;
         _antiSpam.AddHeaderSpam = true;
         _antiSpam.PrependSubject = true;
         _antiSpam.PrependSubjectText = "ThisIsSpam";

         // Enable SURBL.
         var surblServer = _antiSpam.SURBLServers[0];
         surblServer.Active = true;
         surblServer.Score = 5;
         surblServer.Save();

         // Send a messages to this account.
         var smtpClientSimulator = new SmtpClientSimulator();

         smtpClientSimulator.Send("surbltest@example.test", "surbltest@example.test", "SURBL-No-Match",
            "This is a test message without a SURBL url.");

         var sMessageContents = Pop3ClientSimulator.AssertGetFirstMessageText(account1.Address, "test");
         if (sMessageContents.Length == 0 ||
             sMessageContents.Contains("X-hMailServer-Spam") ||
             sMessageContents.Contains("ThisIsSpam"))
            throw new Exception("Non-Spam message detected as spam");

         Assert.IsTrue(sMessageContents.Contains(_domain.SignaturePlainText));

         smtpClientSimulator.Send("surbltest@example.test", "surbltest@example.test", "SURBL-No-Match",
            "This is a test message with a SURBL url: -> http://surbl-org-permanent-test-point.com/ <-");

         sMessageContents = Pop3ClientSimulator.AssertGetFirstMessageText(account1.Address, "test");
         if (!sMessageContents.Contains("X-hMailServer-Spam") ||
             !sMessageContents.Contains("ThisIsSpam"))
            throw new Exception("Spam message not detected as spam");

         Assert.IsTrue(sMessageContents.Contains(_domain.SignaturePlainText));
      }

      [Test]
      public void TestSURBLCorrectNegative()
      {
         var account1 = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "surbltest@example.test", "test");

         // Disallow incorrect line endings.
         _antiSpam.SpamMarkThreshold = 1;
         _antiSpam.SpamDeleteThreshold = 100;
         _antiSpam.AddHeaderReason = true;
         _antiSpam.AddHeaderSpam = true;
         _antiSpam.PrependSubject = true;
         _antiSpam.PrependSubjectText = "ThisIsSpam";

         // Enable SURBL.
         var surblServer = _antiSpam.SURBLServers[0];
         surblServer.Active = true;
         surblServer.Score = 5;
         surblServer.Save();

         // Send a messages to this account.
         SmtpClientSimulator.StaticSend("surbltest@example.test", "surbltest@example.test", "SURBL-Match",
            "This is a test message without a SURBL url: -> http://www.youtube.com/ <-");

         var sMessageContents = Pop3ClientSimulator.AssertGetFirstMessageText(account1.Address, "test");
         if (sMessageContents.Contains("X-hMailServer-Spam"))
            throw new Exception("Non-spam message detected as spam");

         surblServer.Active = false;
         surblServer.Save();
      }

      [Test]
      public void TestSURBLWithWrappedURL()
      {
         // Create a test account
         // Fetch the default domain
         var account1 = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "surbltest@example.test", "test");

         // Disallow incorrect line endings.
         _antiSpam.SpamMarkThreshold = 1;
         _antiSpam.SpamDeleteThreshold = 100;
         _antiSpam.AddHeaderReason = true;
         _antiSpam.AddHeaderSpam = true;
         _antiSpam.PrependSubject = true;
         _antiSpam.PrependSubjectText = "ThisIsSpam";

         // Enable SURBL.
         var surblServer = _antiSpam.SURBLServers[0];
         surblServer.Active = true;
         surblServer.Score = 5;
         surblServer.Save();

         // Send a messages to this account.
         var smtpClientSimulator = new SmtpClientSimulator();

         smtpClientSimulator.Send("surbltest@example.test", "surbltest@example.test", "SURBL-Match",
            "Wrapped URL - <a href=3D\"http://surbl-org-perma=\r\nnent-test-point.com\">Test</a>");

         var sMessageContents = Pop3ClientSimulator.AssertGetFirstMessageText(account1.Address, "test");
         Assert.IsTrue(sMessageContents.Contains("X-hMailServer-Spam"), "Spam message not detected as spam");

         surblServer.Active = false;
         surblServer.Save();
      }

      [Test]
      public void TestSingleLineUrlFollowedByNewline()
      {
         // Create a test account
         // Fetch the default domain
         var account1 = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "surbltest@example.test", "test");

         // Disallow incorrect line endings.
         _antiSpam.SpamMarkThreshold = 1;
         _antiSpam.SpamDeleteThreshold = 100;
         _antiSpam.AddHeaderReason = true;
         _antiSpam.AddHeaderSpam = true;
         _antiSpam.PrependSubject = true;
         _antiSpam.PrependSubjectText = "ThisIsSpam";

         // Enable SURBL.
         var surblServer = _antiSpam.SURBLServers[0];
         surblServer.Active = true;
         surblServer.Score = 5;
         surblServer.Save();

         // Send a messages to this account.
         var smtpClientSimulator = new SmtpClientSimulator();

         smtpClientSimulator.Send("surbltest@example.test", "surbltest@example.test", "SURBL-Match",
            "Wrapped URL - <a href=3D\"http://surbl-org-permanent-test-point.com\r\nHello\">Test</a>");

         var sMessageContents = Pop3ClientSimulator.AssertGetFirstMessageText(account1.Address, "test");
         Assert.IsTrue(sMessageContents.Contains("X-hMailServer-Spam"), "Spam message not detected as spam");

         surblServer.Active = false;
         surblServer.Save();
      }

      [Test]
      public void TestSurblMultipleAddresses()
      {
         // Create a test account
         // Fetch the default domain
         var account1 = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "surbltest@example.test", "test");

         // Disallow incorrect line endings.
         _antiSpam.SpamMarkThreshold = 1;
         _antiSpam.SpamDeleteThreshold = 100;
         _antiSpam.AddHeaderReason = true;
         _antiSpam.AddHeaderSpam = true;
         _antiSpam.PrependSubject = true;
         _antiSpam.PrependSubjectText = "ThisIsSpam";

         // Enable SURBL.
         var surblServer = _antiSpam.SURBLServers[0];
         surblServer.Active = true;
         surblServer.Score = 5;
         surblServer.Save();

         // Send a messages to this account.
         var smtpClientSimulator = new SmtpClientSimulator();

         smtpClientSimulator.Send("surbltest@example.test", "surbltest@example.test", "SURBL-Match",
            "Wrapped URL - <a href=3D\"http://surbl-org-permanent-test-point.com\">Test</a>\r\nWrapped URL - <a href=3D\"http://surbl-org-permanent-test-point.com\">Test</a>\r\nWrapped URL - <a href=3D\"http://surbl-org-permanent-test-point.com\">Test</a>\r\n");

         var sMessageContents = Pop3ClientSimulator.AssertGetFirstMessageText(account1.Address, "test");
         Assert.IsTrue(sMessageContents.Contains("X-hMailServer-Spam"), "Spam message not detected as spam");

         surblServer.Active = false;
         surblServer.Save();
      }

      [Test]
      public void TestSurblAddressEndingWithSingleQuote()
      {
         LogHandler.DeleteCurrentDefaultLog();

         // Create a test account
         // Fetch the default domain
         var account1 = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "surbltest@example.test", "test");

         // Disallow incorrect line endings.
         _antiSpam.SpamMarkThreshold = 1;
         _antiSpam.SpamDeleteThreshold = 100;
         _antiSpam.AddHeaderReason = true;
         _antiSpam.AddHeaderSpam = true;
         _antiSpam.PrependSubject = true;
         _antiSpam.PrependSubjectText = "ThisIsSpam";

         // Enable SURBL.
         var surblServer = _antiSpam.SURBLServers[0];
         surblServer.Active = true;
         surblServer.Score = 5;
         surblServer.Save();

         // Send a messages to this account.
         var smtpClientSimulator = new SmtpClientSimulator();

         smtpClientSimulator.Send("surbltest@example.test", "surbltest@example.test", "SURBL-Match",
            "Wrapped URL - <a href=3D'http://surbl-org-permanent-test-point.com'>Test</a>\r\n");

         surblServer.Active = false;
         surblServer.Save();

         Assert.IsFalse(LogHandler.DefaultLogContains("Lookup: surbl-org-permanent-test-point.com'.multi.surbl.org"));
         Assert.IsTrue(LogHandler.DefaultLogContains("Lookup: surbl-org-permanent-test-point.com.multi.surbl.org"));
      }

      [Test]
      public void TestSurblMultipleNegatives()
      {
         LogHandler.DeleteCurrentDefaultLog();

         // Create a test account
         // Fetch the default domain
         var account1 = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "surbltest@example.test", "test");

         // Disallow incorrect line endings.
         _antiSpam.SpamMarkThreshold = 1;
         _antiSpam.SpamDeleteThreshold = 100;
         _antiSpam.AddHeaderReason = true;
         _antiSpam.AddHeaderSpam = true;
         _antiSpam.PrependSubject = true;
         _antiSpam.PrependSubjectText = "ThisIsSpam";

         // Enable SURBL.
         var surblServer = _antiSpam.SURBLServers[0];
         surblServer.Active = true;
         surblServer.Score = 5;
         surblServer.Save();

         // Send a messages to this account.
         var smtpClientSimulator = new SmtpClientSimulator();

         smtpClientSimulator.Send("surbltest@example.test", "surbltest@example.test", "SURBL-Match",
            "Wrapped URL - <a href=3D\"http://test.example1fdafdsfds.com\">Test</a>\r\nWrapped URL - <a href=3D\"http://test.example2fdafdsfds.com\">Test</a>\r\nWrapped URL - <a href=3D\"http://test.example3fdafdsfds.com\">Test</a>\r\n");

         var sMessageContents = Pop3ClientSimulator.AssertGetFirstMessageText(account1.Address, "test");
         Assert.IsFalse(sMessageContents.Contains("X-hMailServer-Spam"), "Spam message not detected as spam");

         surblServer.Active = false;
         surblServer.Save();

         Assert.IsTrue(LogHandler.DefaultLogContains("SURBL: Lookup: example1fdafdsfds.com.multi.surbl.org"));
         Assert.IsTrue(LogHandler.DefaultLogContains("SURBL: Lookup: example2fdafdsfds.com.multi.surbl.org"));
         Assert.IsTrue(LogHandler.DefaultLogContains("SURBL: Lookup: example3fdafdsfds.com.multi.surbl.org"));
      }

      [Test]
      public void SurblTestRealWorldBody1()
      {
         LogHandler.DeleteCurrentDefaultLog();

         // Create a test account
         // Fetch the default domain
         var account1 = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "surbltest@example.test", "test");

         // Disallow incorrect line endings.
         _antiSpam.SpamMarkThreshold = 1;
         _antiSpam.SpamDeleteThreshold = 100;
         _antiSpam.AddHeaderReason = true;
         _antiSpam.AddHeaderSpam = true;
         _antiSpam.PrependSubject = true;
         _antiSpam.PrependSubjectText = "ThisIsSpam";

         // Enable SURBL.
         var surblServer = _antiSpam.SURBLServers[0];
         surblServer.Active = true;
         surblServer.Score = 5;
         surblServer.Save();

         // Send a messages to this account.
         var smtpClientSimulator = new SmtpClientSimulator();

         smtpClientSimulator.Send("surbltest@example.test", "surbltest@example.test", "SURBL-Match",
            TestResources.SecuniaBody1);

         var sMessageContents = Pop3ClientSimulator.AssertGetFirstMessageText(account1.Address, "test");
         Assert.IsFalse(sMessageContents.Contains("X-hMailServer-Spam"), "Spam message not detected as spam");

         surblServer.Active = false;
         surblServer.Save();

         Assert.IsTrue(LogHandler.DefaultLogContains("SURBL: 3 unique domain addresses found."));
         Assert.IsTrue(LogHandler.DefaultLogContains("Found URL: secunia.com"));
         Assert.IsTrue(LogHandler.DefaultLogContains("SURBL: Found URL: secunia.com"));
         Assert.IsTrue(LogHandler.DefaultLogContains("SURBL: Found URL: ca.secunia.com"));
         Assert.IsTrue(LogHandler.DefaultLogContains("SURBL: Found URL: ubuntu.com"));

         Assert.IsTrue(LogHandler.DefaultLogContains("SURBL: Lookup: secunia.com.multi.surbl.org"));
         Assert.IsTrue(LogHandler.DefaultLogContains("SURBL: Lookup: ca.secunia.com.multi.surbl.org"));
         Assert.IsTrue(LogHandler.DefaultLogContains("SURBL: Lookup: ubuntu.com.multi.surbl.org"));
      }

      [Test]
      [Description(
         "Every enabled spam test must still run when neither threshold is set. Scoring off with " +
         "greylisting on is a supported configuration, but the pipeline's early exit compared the " +
         "running score against max(mark, delete) - and 0 >= 0 is true before any test has run, so " +
         "only the first enabled test ever ran. That produced no visible scoring difference, because " +
         "neither action is armed, but PerformGreyListing looks for the SPF result in that same " +
         "result set to honour BypassGreyListingOnSPFSuccess, and SPF is the fifth test in the order.")]
      public void TestAllTestsRunWhenNeitherThresholdIsSet()
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "nothreshold@example.test", "test");

         // Both actions disabled: nothing can be marked and nothing can be deleted.
         _antiSpam.SpamMarkThreshold = 0;
         _antiSpam.SpamDeleteThreshold = 0;

         // Two pre-transmission tests, first and fifth in the pipeline order. What
         // is being observed is that the fifth one still runs; neither has to
         // produce a score, and the HELO check does not even apply to a loopback
         // client - the runner logs each test it ran either way.
         _antiSpam.CheckHostInHelo = true;
         _antiSpam.CheckHostInHeloScore = 3;
         _antiSpam.UseSPF = true;
         _antiSpam.UseSPFScore = 3;

         SmtpClientSimulator.StaticSend("test@example.com", account.Address, "Test", "TestBody");

         Pop3ClientSimulator.AssertMessageCount(account.Address, "test", 1);

         Assert.IsTrue(LogHandler.DefaultLogContains("Spam test: SpamTestHeloHost"),
            LogHandler.ReadCurrentDefaultLog());
         Assert.IsTrue(LogHandler.DefaultLogContains("Spam test: SpamTestSPF"),
            LogHandler.ReadCurrentDefaultLog());
      }
   }
}