// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Text;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;
using RegressionTests.SSL;
using hMailServer;

namespace RegressionTests.ExternalAccounts
{
   /// <summary>
   ///    XOAUTH2 for POP3 fetching - the other half of the Microsoft 365
   ///    Basic-auth story: collecting FROM outlook.office365.com with USER/PASS
   ///    has been off since 2022. When an external account's server is on
   ///    FetchOAuth2Hosts, the fetcher logs in with a bearer token in the same
   ///    one-line shape the outbound relay uses; everywhere else, USER/PASS is
   ///    untouched. Asserted on the exact bytes the POP3 server receives, and on
   ///    the messages actually arriving afterwards - a login that succeeds but
   ///    fetches nothing is not a login worth having.
   /// </summary>
   [TestFixture]
   public class FetchXOAuth2 : TestFixtureBase
   {

      /// <summary>
      ///    Stores a [Settings] key in the settings store over COM, which is how one is
      ///    changed from schema 6042 - hMailServer.ini holds only the store's copy.
      /// </summary>
      private void WriteSetting(string key, string value)
      {
         IniFileSetting.Write(key, value);
      }

      private FetchAccount AddFetchAccount(Account account, int port,
         string serverAddress = "127.0.0.1", eConnectionSecurity security = eConnectionSecurity.eCSNone)
      {
         var fa = account.FetchAccounts.Add();
         fa.DaysToKeepMessages = 5;
         fa.Enabled = true;
         fa.MinutesBetweenFetch = 10;
         fa.Name = "oauth-fetch";
         fa.Port = port;
         fa.ServerAddress = serverAddress;
         fa.Username = "fetch-user@example.test";
         fa.ConnectionSecurity = security;
         fa.Save();
         return fa;
      }

      [Test]
      [Description("A fetch server on the OAuth host list gets AUTH XOAUTH2 with the exact blob over TLS, and the mail arrives.")]
      public void TheConfiguredHostGetsTheBearerBlob()
      {
         WriteSetting("FetchOAuth2Hosts", "localhost");
         WriteSetting("OutboundOAuth2FixedToken", "fetch-bearer-98765");
         _application.Reinitialize();

         try
         {
            var account = SingletonProvider<TestSetup>.Instance.AddAccount(
               _domain, "fetch-oauth@example.test", "test");

            int port = TestSetup.GetNextFreePort();
            var fa = AddFetchAccount(account, port, "localhost", eConnectionSecurity.eCSTLS);

            var messages = new List<string> { "Subject: Fetched over OAuth\r\n\r\nBody.\r\n" };

            using (var pop3Server = new Pop3ServerSimulator(1, port, messages, eConnectionSecurity.eCSTLS))
            {
               pop3Server.SetCertificate(SslSetup.GetCertificate());
               pop3Server.StartListen();
               fa.DownloadNow();
               pop3Server.WaitForCompletion();

               Assert.IsNotEmpty(pop3Server.XOAuth2Blob,
                  "The fetch never presented AUTH XOAUTH2 although its server is on FetchOAuth2Hosts.");
               Assert.IsFalse(pop3Server.UserCommandReceived,
                  "USER was sent alongside the bearer - the password path must not run for an OAuth host.");

               string decoded = Encoding.UTF8.GetString(Convert.FromBase64String(pop3Server.XOAuth2Blob));
               Assert.AreEqual("user=fetch-user@example.test\u0001auth=Bearer fetch-bearer-98765\u0001\u0001", decoded,
                  "The XOAUTH2 blob is not the shape the provider parses.");

               // The AUTH line is the token itself in base64, so the POP3 client log
               // shows it without the blob, as it shows PASS.
               Assert.IsTrue(LogHandler.DefaultLogContains("SENT: AUTH XOAUTH2 ***"), "The POP3 client log does not show the bearer login.");
               StringAssert.DoesNotContain(pop3Server.XOAuth2Blob.TrimEnd('='), LogHandler.ReadCurrentDefaultLog(),
                  "The POP3 client log carries the server's bearer token.");
            }

            CustomAsserts.AssertRecipientsInDeliveryQueue(0);
            Pop3ClientSimulator.AssertMessageCount(account.Address, "test", 1);
         }
         finally
         {
            WriteSetting("FetchOAuth2Hosts", "outlook.office365.com");
            WriteSetting("OutboundOAuth2FixedToken", "");
            _application.Reinitialize();
         }
      }

      /// <summary>
      ///    OUT-1: the server's token reaches every mailbox the app registration was
      ///    granted, so it goes over TLS only. Before the fix a fetch account on the
      ///    list with connection security none sent AUTH XOAUTH2 - the token in
      ///    base64 - in the clear, and this test's first assertion failed; now the
      ///    fetcher sends neither the token nor USER/PASS, and says why in the error
      ///    log.
      /// </summary>
      [Test]
      [Description("A fetch account on the OAuth host list without TLS never sends the bearer token, nor falls back to USER/PASS.")]
      public void TheServersTokenIsNeverSentInTheClear()
      {
         WriteSetting("FetchOAuth2Hosts", "127.0.0.1");
         WriteSetting("OutboundOAuth2FixedToken", "fetch-bearer-98765");
         _application.Reinitialize();

         try
         {
            var account = SingletonProvider<TestSetup>.Instance.AddAccount(
               _domain, "fetch-clear@example.test", "test");

            int port = TestSetup.GetNextFreePort();
            var fa = AddFetchAccount(account, port);

            var messages = new List<string> { "Subject: Never fetched\r\n\r\nBody.\r\n" };

            using (var pop3Server = new Pop3ServerSimulator(1, port, messages))
            {
               pop3Server.StartListen();
               fa.DownloadNow();
               pop3Server.WaitForCompletion();

               Assert.IsEmpty(pop3Server.XOAuth2Blob,
                  "The server's bearer token went to a fetch server over a connection without TLS.");
               Assert.IsFalse(pop3Server.UserCommandReceived,
                  "USER/PASS is no fallback for a host on FetchOAuth2Hosts.");
            }

            CustomAsserts.AssertReportedError("over TLS only");
            Pop3ClientSimulator.AssertMessageCount(account.Address, "test", 0);
         }
         finally
         {
            WriteSetting("FetchOAuth2Hosts", "outlook.office365.com");
            WriteSetting("OutboundOAuth2FixedToken", "");
            _application.Reinitialize();
         }
      }

      /// <summary>
      ///    The negative control: a server off the list keeps USER/PASS. An
      ///    implementation that presented bearers to every POP3 server would pass
      ///    the positive test and fail here - by leaking tokens to every mailbox
      ///    this server collects from.
      /// </summary>
      [Test]
      [Description("A fetch server off the OAuth host list still logs in with USER/PASS - the control.")]
      public void AnUnlistedHostStillUsesUserPass()
      {
         WriteSetting("FetchOAuth2Hosts", "some-other-provider.example.net");
         WriteSetting("OutboundOAuth2FixedToken", "fetch-bearer-98765");
         _application.Reinitialize();

         try
         {
            var account = SingletonProvider<TestSetup>.Instance.AddAccount(
               _domain, "fetch-plain@example.test", "test");

            int port = TestSetup.GetNextFreePort();
            var fa = AddFetchAccount(account, port);

            var messages = new List<string> { "Subject: Fetched with a password\r\n\r\nBody.\r\n" };

            using (var pop3Server = new Pop3ServerSimulator(1, port, messages))
            {
               pop3Server.StartListen();
               fa.DownloadNow();
               pop3Server.WaitForCompletion();

               Assert.IsEmpty(pop3Server.XOAuth2Blob,
                  "A bearer token was presented to a POP3 server that is not on FetchOAuth2Hosts.");
               Assert.IsTrue(pop3Server.UserCommandReceived,
                  "The ordinary USER/PASS login stopped working with OAuth configured for a different host.");
            }

            CustomAsserts.AssertRecipientsInDeliveryQueue(0);
            Pop3ClientSimulator.AssertMessageCount(account.Address, "test", 1);
         }
         finally
         {
            WriteSetting("FetchOAuth2Hosts", "outlook.office365.com");
            WriteSetting("OutboundOAuth2FixedToken", "");
            _application.Reinitialize();
         }
      }
   }
}
