// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Threading;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;
using hMailServer;

namespace RegressionTests.SMTP
{
   /// <summary>
   ///    Outbound XOAUTH2 - the Microsoft 365 Basic-auth cutover item (December
   ///    2026). When a route's destination is on the configured OAuth host list,
   ///    the outbound AUTH step presents a bearer token in Microsoft's XOAUTH2
   ///    shape instead of LOGIN with the password; everywhere else, nothing
   ///    changes. Both halves are asserted on the EXACT BYTES the relay receives,
   ///    decoded from the AUTH line - because the blob's precise shape
   ///    (user=...\x01auth=Bearer ...\x01\x01, base64, one round trip) is the
   ///    entire interoperability contract with Exchange Online.
   ///
   ///    The token here is a fixed one (OutboundOAuth2FixedToken), the setting
   ///    that exists for tokens obtained outside this server - which also makes
   ///    the test independent of any identity provider. The fetch-and-cache path
   ///    shares everything downstream of the token's origin.
   /// </summary>
   [TestFixture]
   public class OutboundXOAuth2 : TestFixtureBase
   {

      private void WriteSetting(string key, string value)
      {
         IniFileSetting.Write(key, value);
      }

      /// <summary>A quoted-printable text with its soft line breaks removed and =20 read as the space it is, so a sentence can be looked for whole.</summary>
      private static string QuotedPrintableUnfolded(string quotedPrintable)
      {
         return quotedPrintable.Replace("=\r\n", "").Replace("=20", " ");
      }

      private Route AddAuthedRoute(int port, eConnectionSecurity security = eConnectionSecurity.eCSNone)
      {
         var route = _settings.Routes.Add();
         route.DomainName = "oauth-relay.test";
         route.TargetSMTPHost = "localhost";
         route.TargetSMTPPort = port;
         route.NumberOfTries = 1;
         route.MinutesBetweenTry = 5;
         route.RelayerRequiresAuth = true;
         route.RelayerAuthUsername = "relay-account@example.test";
         route.SetRelayerAuthPassword("basic-password");
         route.ConnectionSecurity = security;
         route.Save();
         return route;
      }

      [Test]
      [Description("A destination on the OAuth host list gets AUTH XOAUTH2 with the exact Microsoft blob, over STARTTLS.")]
      public void TheConfiguredHostGetsTheBearerBlob()
      {
         WriteSetting("OutboundOAuth2Hosts", "localhost");
         WriteSetting("OutboundOAuth2FixedToken", "test-bearer-token-12345");
         _application.Reinitialize();

         try
         {
            var smtpServerPort = TestSetup.GetNextFreePort();
            AddAuthedRoute(smtpServerPort, eConnectionSecurity.eCSSTARTTLSRequired);

            var deliveryResults = new Dictionary<string, int> { ["dummy@oauth-relay.test"] = 250 };

            using (var server = new SmtpServerSimulator(1, smtpServerPort, eConnectionSecurity.eCSSTARTTLSRequired))
            {
               server.AddRecipientResult(deliveryResults);
               server.StartListen();

               SmtpClientSimulator.StaticSend("sender@example.test", "dummy@oauth-relay.test", "OAuth relay", "Body");

               server.WaitForCompletion();

               Assert.IsNotEmpty(server.XOAuth2Blob,
                  "The relay never received AUTH XOAUTH2 although its host is on the OAuth list.");

               string decoded = Encoding.UTF8.GetString(Convert.FromBase64String(server.XOAuth2Blob));

               Assert.AreEqual("user=relay-account@example.test\u0001auth=Bearer test-bearer-token-12345\u0001\u0001", decoded,
                  "The XOAUTH2 blob is not the shape Exchange Online parses - user, ^A, auth=Bearer token, ^A^A.");

               Assert.IsTrue(server.MessageData.Contains("OAuth relay"),
                  "The message did not deliver after the bearer login succeeded.");
            }
         }
         finally
         {
            WriteSetting("OutboundOAuth2Hosts", "smtp.office365.com");
            WriteSetting("OutboundOAuth2FixedToken", "");
            _application.Reinitialize();
         }
      }

      /// <summary>
      ///    The bearer token is this server's own client_credentials token, which
      ///    sends as every mailbox the app registration was granted, so it goes
      ///    over TLS only, as the fetchers' does. Before the fix a route on the list
      ///    with connection security none sent AUTH XOAUTH2 - the token in base64 -
      ///    in the clear, and this test's first assertion failed; now nothing is
      ///    sent in its place, the message waits, and the error log says why.
      /// </summary>
      [Test]
      [Description("A route on the OAuth host list without TLS never sends the bearer token, nor falls back to the password.")]
      public void TheBearerTokenIsNeverSentInTheClear()
      {
         WriteSetting("OutboundOAuth2Hosts", "localhost");
         WriteSetting("OutboundOAuth2FixedToken", "test-bearer-token-12345");
         _application.Reinitialize();

         try
         {
            var smtpServerPort = TestSetup.GetNextFreePort();
            AddAuthedRoute(smtpServerPort);

            // A local sender, so that the route's one try ends in a bounce that is
            // read here rather than left in the queue.
            var sender = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "oauth-clear@" + _domain.Name, "test");

            var deliveryResults = new Dictionary<string, int> { ["dummy@oauth-relay.test"] = 250 };

            using (var server = new SmtpServerSimulator(1, smtpServerPort))
            {
               server.AddRecipientResult(deliveryResults);
               server.StartListen();

               SmtpClientSimulator.StaticSend(sender.Address, "dummy@oauth-relay.test", "Clear relay", "Body");

               server.WaitForCompletion();

               Assert.IsEmpty(server.XOAuth2Blob,
                  "The server's bearer token went to a relay over a connection without TLS.");
               Assert.IsFalse((server.MessageData ?? "").Contains("Clear relay"),
                  "The message was delivered although the only login this route may use could not be sent.");
            }

            CustomAsserts.AssertReportedError("over TLS only");
            CustomAsserts.AssertRecipientsInDeliveryQueue(0, true);

            // The bounce's human-readable part is UTF-8 quoted-printable, as every
            // bounce's is, and its 76-character lines break the sentence with a soft
            // line break ("sent ove=" / "r TLS only"), so it is looked for with the
            // soft line breaks removed. The delivery-status part is 7bit and says
            // 4.7.0, the security class the 450 is given.
            string bounce = Pop3ClientSimulator.AssertGetFirstMessageText(sender.Address, "test");
            StringAssert.Contains("The OAuth2 token for this destination is sent over TLS only, and the connection has no TLS.",
               QuotedPrintableUnfolded(bounce), "The bounce does not say why the message was not relayed.");
            StringAssert.Contains("Status: 4.7.0", bounce, "The bounce's delivery status is not the security one.");
         }
         finally
         {
            WriteSetting("OutboundOAuth2Hosts", "smtp.office365.com");
            WriteSetting("OutboundOAuth2FixedToken", "");
            _application.Reinitialize();
         }
      }

      /// <summary>
      ///    A relay that refuses the bearer login - Exchange Online answers 535
      ///    5.7.3 for a mailbox with SMTP AUTH off, or an app it has not granted -
      ///    fails the delivery, and the bounce to the message's sender says what
      ///    this server sent ("hMailServer sent: ..."). The AUTH XOAUTH2 line is
      ///    the token itself in base64, so it is named there, and in the SMTP log,
      ///    without its blob, as the password is. Before the fix the bounce quoted
      ///    the whole line, handing the server's own token - which sends as, and
      ///    through FetchOAuth2Hosts reads, every mailbox the app registration was
      ///    granted - to whoever sent the message, and the log kept it too; the
      ///    "&lt;Token removed&gt;" and the not-contains assertions fail against that code.
      /// </summary>
      [Test]
      [Description("A refused bearer login bounces without the token, and the SMTP log shows the AUTH line without it.")]
      public void ARefusedBearerLoginKeepsTheTokenOutOfTheBounceAndTheLog()
      {
         WriteSetting("OutboundOAuth2Hosts", "localhost");
         WriteSetting("OutboundOAuth2FixedToken", "refused-bearer-token-67890");
         _application.Reinitialize();

         try
         {
            var smtpServerPort = TestSetup.GetNextFreePort();
            AddAuthedRoute(smtpServerPort, eConnectionSecurity.eCSSTARTTLSRequired);

            // A local sender, so that the refusal ends in a bounce that is read here.
            var sender = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "oauth-refused@" + _domain.Name, "test");

            var deliveryResults = new Dictionary<string, int> { ["dummy@oauth-relay.test"] = 250 };
            string blob;

            using (var server = new SmtpServerSimulator(1, smtpServerPort, eConnectionSecurity.eCSSTARTTLSRequired))
            {
               server.XOAuth2Reply = "535 5.7.3 Authentication unsuccessful";
               server.AddRecipientResult(deliveryResults);
               server.StartListen();

               SmtpClientSimulator.StaticSend(sender.Address, "dummy@oauth-relay.test", "Refused bearer", "Body");

               server.WaitForCompletion();

               blob = server.XOAuth2Blob;
               Assert.IsNotEmpty(blob, "The relay never received AUTH XOAUTH2, so this run shows nothing about where the token goes.");
               Assert.IsFalse((server.MessageData ?? "").Contains("Refused bearer"),
                  "The message was delivered although the relay refused the login.");
            }

            CustomAsserts.AssertRecipientsInDeliveryQueue(0, true);

            // The base64 alphabet is not quoted in quoted-printable, so the blob
            // without its '=' padding is looked for in the bounce with the soft line
            // breaks removed - a line holding it is always longer than 76 characters.
            string token = blob.TrimEnd('=');
            string bounce = QuotedPrintableUnfolded(Pop3ClientSimulator.AssertGetFirstMessageText(sender.Address, "test"));
            StringAssert.Contains("Remote server replied: 535 5.7.3 Authentication unsuccessful", bounce,
               "The bounce is not the one for the refused login.");
            StringAssert.Contains("hMailServer sent: AUTH XOAUTH2 <Token removed>", bounce,
               "The bounce does not say that the bearer login was what the relay refused.");
            StringAssert.DoesNotContain(token, bounce, "The bounce to the sender carries the server's bearer token.");

            Assert.IsTrue(LogHandler.DefaultLogContains("SENT: AUTH XOAUTH2 ***"), "The SMTP log does not show the bearer login.");
            StringAssert.DoesNotContain(token, LogHandler.ReadCurrentDefaultLog(), "The SMTP log carries the server's bearer token.");
         }
         finally
         {
            WriteSetting("OutboundOAuth2Hosts", "smtp.office365.com");
            WriteSetting("OutboundOAuth2FixedToken", "");
            _application.Reinitialize();
         }
      }

      /// <summary>
      ///    A TCP listener on 127.0.0.1 that accepts every connection and never says a
      ///    word on any of them - a token endpoint that has stopped answering - and
      ///    counts what it accepted.
      /// </summary>
      private sealed class SilentEndpoint : IDisposable
      {
         private readonly TcpListener _listener;
         private readonly List<TcpClient> _held = new List<TcpClient>();
         private int _accepted;

         public SilentEndpoint(int port)
         {
            _listener = new TcpListener(IPAddress.Loopback, port);
            _listener.Start();
            _listener.BeginAcceptTcpClient(Accept, null);
         }

         public int Accepted => Volatile.Read(ref _accepted);

         private void Accept(IAsyncResult result)
         {
            TcpClient client;
            try
            {
               client = _listener.EndAcceptTcpClient(result);
            }
            catch (Exception stopped) when (stopped is ObjectDisposedException || stopped is SocketException)
            {
               // The listener was stopped: nothing more to accept.
               return;
            }

            lock (_held)
               _held.Add(client);
            Interlocked.Increment(ref _accepted);

            try
            {
               _listener.BeginAcceptTcpClient(Accept, null);
            }
            catch (Exception stopped) when (stopped is ObjectDisposedException || stopped is SocketException)
            {
               // Stopped between the two: the connection just taken is closed with the rest.
            }
         }

         public void Dispose()
         {
            _listener.Stop();
            lock (_held)
            {
               foreach (TcpClient client in _held)
                  client.Close();
            }
         }
      }

      /// <summary>
      ///    A token endpoint that takes the connection and never answers costs the
      ///    delivery the token request's twenty seconds, on every platform, and the
      ///    delivery then goes on with the password, as the error log says. Until
      ///    6.3.6 the token request set its timeout in the Windows form, which Linux
      ///    refuses, and read with Asio's synchronous calls, which on Linux wait with
      ///    no timeout: there the delivery thread, and every other that wanted a token
      ///    behind it, was held for as long as the endpoint stayed connected. The old
      ///    request also took a port in OutboundOAuth2TokenUrl for part of the host
      ///    name and failed to resolve it, so this endpoint, on a port of its own, was
      ///    never reached: against that code the first assertion fails, on both
      ///    platforms. network-deadline-tests holds the same request to its deadline
      ///    on Linux without a delivery around it.
      /// </summary>
      [Test]
      [Description("A token endpoint that never answers is given up on at the deadline, and the delivery goes on with the password.")]
      public void ATokenEndpointThatNeverAnswersIsGivenUpOn()
      {
         int tokenPort = TestSetup.GetNextFreePort();

         using (var endpoint = new SilentEndpoint(tokenPort))
         {
            WriteSetting("OutboundOAuth2Hosts", "localhost");
            WriteSetting("OutboundOAuth2FixedToken", "");
            WriteSetting("OutboundOAuth2TokenUrl", "https://127.0.0.1:" + tokenPort + "/token");
            WriteSetting("OutboundOAuth2ClientId", "regression-tests");
            WriteSetting("OutboundOAuth2ClientSecret", "not-a-secret");
            _application.Reinitialize();

            try
            {
               var smtpServerPort = TestSetup.GetNextFreePort();
               AddAuthedRoute(smtpServerPort, eConnectionSecurity.eCSSTARTTLSRequired);

               var deliveryResults = new Dictionary<string, int> { ["dummy@oauth-relay.test"] = 250 };

               using (var server = new SmtpServerSimulator(1, smtpServerPort, eConnectionSecurity.eCSSTARTTLSRequired))
               {
                  // The delivery waits out the token request's twenty seconds first.
                  server.SecondsToWaitBeforeTerminate = 120;
                  server.AddRecipientResult(deliveryResults);
                  server.StartListen();

                  SmtpClientSimulator.StaticSend("sender@example.test", "dummy@oauth-relay.test", "Silent token endpoint", "Body");

                  server.WaitForCompletion();

                  Assert.Greater(endpoint.Accepted, 0,
                     "The token endpoint's port was never reached: the request did not take the port in OutboundOAuth2TokenUrl.");
                  Assert.IsEmpty(server.XOAuth2Blob, "A bearer token was presented although none could be obtained.");
                  Assert.IsTrue(server.MessageData.Contains("Silent token endpoint"),
                     "The delivery did not go on with the password once the token request was given up.");
               }

               CustomAsserts.AssertReportedError("Could not reach the token endpoint", "in the time allowed");
            }
            finally
            {
               WriteSetting("OutboundOAuth2Hosts", "smtp.office365.com");
               WriteSetting("OutboundOAuth2TokenUrl", "");
               WriteSetting("OutboundOAuth2ClientId", "");
               WriteSetting("OutboundOAuth2ClientSecret", "");
               _application.Reinitialize();
            }
         }
      }

      /// <summary>
      ///    The negative control: a destination NOT on the host list keeps the
      ///    password login it always had. An implementation that sent bearers
      ///    everywhere would pass the positive test and fail here - by leaking a
      ///    token to every relay the server talks to.
      /// </summary>
      [Test]
      [Description("A destination off the OAuth host list still authenticates with LOGIN - the control.")]
      public void AnUnlistedHostStillUsesLogin()
      {
         WriteSetting("OutboundOAuth2Hosts", "some-other-relay.example.net");
         WriteSetting("OutboundOAuth2FixedToken", "test-bearer-token-12345");
         _application.Reinitialize();

         try
         {
            var smtpServerPort = TestSetup.GetNextFreePort();
            AddAuthedRoute(smtpServerPort);

            var deliveryResults = new Dictionary<string, int> { ["dummy@oauth-relay.test"] = 250 };

            using (var server = new SmtpServerSimulator(1, smtpServerPort))
            {
               server.AddRecipientResult(deliveryResults);
               server.StartListen();

               SmtpClientSimulator.StaticSend("sender@example.test", "dummy@oauth-relay.test", "Password relay", "Body");

               server.WaitForCompletion();

               Assert.IsEmpty(server.XOAuth2Blob,
                  "A bearer token was presented to a relay that is not on the OAuth host list.");
               Assert.IsTrue(server.MessageData.Contains("Password relay"),
                  "The ordinary LOGIN delivery stopped working with OAuth configured for a different host.");
            }
         }
         finally
         {
            WriteSetting("OutboundOAuth2Hosts", "smtp.office365.com");
            WriteSetting("OutboundOAuth2FixedToken", "");
            _application.Reinitialize();
         }
      }
   }
}
