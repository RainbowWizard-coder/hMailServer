// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Threading;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Shared;

namespace RegressionTests.SSL.StartTls
{
   [TestFixture]
   internal class ImapServerTests : TestFixtureBase
   {
      [OneTimeSetUp]
      public new void TestFixtureSetUp()
      {
         SslSetup.SetupSSLPorts(_application);


         Thread.Sleep(1000);
      }

      [SetUp]
      public new void SetUp()
      {
         _account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "test@example.test", "test");
      }

      private Account _account;

      [Test]
      public void IfStartTlsNotEnabledStartTlsShouldNotBeShownInEhloResponse()
      {
         var imapSimulator = new ImapClientSimulator(false, 143);
         imapSimulator.Connect();
         var data = imapSimulator.GetCapabilities();

         Assert.IsFalse(data.Contains("STARTTLS"));
      }

      [Test]
      public void IfStartTlsIsEnabledStartTlsShouldBeShownInEhloResponse()
      {
         var imapSimulator = new ImapClientSimulator(false, 14302);
         imapSimulator.Connect();
         var data = imapSimulator.GetCapabilities();

         Assert.IsTrue(data.Contains("STARTTLS"));
      }

      [Test]
      public void StlsCommandShouldSwithToTls()
      {
         var imapSimulator = new ImapClientSimulator(false, 14302);
         imapSimulator.Connect();
         var data = imapSimulator.GetCapabilities();
         imapSimulator.SendSingleCommand("A01 STARTTLS");
         imapSimulator.Handshake();

         // command is sent over TLS.
         imapSimulator.GetCapabilities();

         imapSimulator.Logon(_account.Address, "test");
      }

      [Test]
      [Description("A second STARTTLS inside the TLS session it started is refused, and STARTTLS is no longer offered there. Negative control: the first STARTTLS on the same connection is taken.")]
      public void ASecondStartTlsIsRefusedOnceTlsIsActive()
      {
         var imapSimulator = new ImapClientSimulator(false, 14302);
         imapSimulator.Connect();
         StringAssert.Contains("STARTTLS", imapSimulator.SendSingleCommand("A01 CAPABILITY"), "Offered before TLS.");
         StringAssert.Contains("A02 OK", imapSimulator.SendSingleCommand("A02 STARTTLS"));
         imapSimulator.Handshake();

         // Over TLS now: neither offered nor taken again (RFC 9051 6.2.1). It
         // used to be offered and answered OK, and a second handshake started
         // on the running session.
         StringAssert.DoesNotContain("STARTTLS", imapSimulator.SendSingleCommand("A03 CAPABILITY"), "Not offered once TLS is active.");
         StringAssert.Contains("A04 BAD", imapSimulator.SendSingleCommand("A04 STARTTLS"));

         // The session goes on.
         ClassicAssert.IsTrue(imapSimulator.Logon(_account.Address, "test"));
         imapSimulator.Disconnect();
      }

      [Test]
      [Description("STARTTLS after LOGIN is refused and not offered: it is a command of the not authenticated state only (RFC 9051 6.2.1).")]
      public void StartTlsAfterLoginIsRefused()
      {
         var imapSimulator = new ImapClientSimulator(false, 14302);
         imapSimulator.Connect();
         ClassicAssert.IsTrue(imapSimulator.Logon(_account.Address, "test"));

         StringAssert.DoesNotContain("STARTTLS", imapSimulator.SendSingleCommand("A02 CAPABILITY"), "Not offered once signed in.");
         StringAssert.Contains("A03 BAD", imapSimulator.SendSingleCommand("A03 STARTTLS"));

         // Still a working plain session.
         StringAssert.Contains("A04 OK", imapSimulator.SendSingleCommand("A04 SELECT INBOX"));
         imapSimulator.Disconnect();
      }

      [Test]
      public void IfStlsRequiredLogonShouldSucceedIfStls()
      {
         var imapSimulator = new ImapClientSimulator(false, 14303);
         imapSimulator.Connect();
         imapSimulator.SendSingleCommand("A01 STARTTLS");
         imapSimulator.Handshake();

         // command is sent over TLS.
         imapSimulator.GetCapabilities();

         Assert.IsTrue(imapSimulator.Logon(_account.Address, "test"));
      }

      [Test]
      public void IfStlsRequiredLogonShouldFailIfNoStls()
      {
         var imapSimulator = new ImapClientSimulator(false, 14303);
         imapSimulator.Connect();

         string errorMessage;
         Assert.IsFalse(imapSimulator.Logon(_account.Address, "test", out errorMessage));

         Assert.IsTrue(errorMessage.Contains("A01 BAD STARTTLS is required."));
      }

      [Test]
      public void IfStlsOptionalButSslRequiredByIpRangeForAuthThenAuthShouldFail()
      {
         var range = SingletonProvider<TestSetup>.Instance.GetApp().Settings.SecurityRanges
            .get_ItemByName("My computer");
         range.RequireSSLTLSForAuth = true;
         range.Save();

         var imapSimulator = new ImapClientSimulator(false, 14302);
         imapSimulator.Connect();

         string errorMessage;
         Assert.IsFalse(imapSimulator.Logon(_account.Address, "test", out errorMessage));
         Assert.IsTrue(errorMessage.Contains("A01 BAD A SSL/TLS-connection is required for authentication."));
      }
   }
}