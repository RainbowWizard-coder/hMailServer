// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Net.Security;
using System.Net.Sockets;
using System.Security.Authentication;
using System.Text;
using System.Threading;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.SSL;
using RegressionTests.Shared;
using hMailServer;

namespace RegressionTests.API
{
   /// <summary>
   ///    The administration listener answers only under a Host it expects, a wrong
   ///    administrator password is throttled even from loopback, and HSTS is sent
   ///    only to a browser that came over TLS.
   ///
   ///    The Host check is what stops DNS rebinding: a page a browser was rebound
   ///    to this loopback address sends that page's own name, which is neither a
   ///    loopback name, an address, nor one an administrator configured, and is
   ///    turned away with 421 before any credential is looked at. A loopback name,
   ///    an IP address, the server's host name and the names configured as the
   ///    webmail's outside address pass, which is what a reverse proxy forwards.
   ///
   ///    The throttle is what the loopback auto-ban exemption leaves open: the
   ///    administrator password, exempt from both the auto-ban (on loopback, which
   ///    is every browser behind a proxy on the same machine) and the per-name
   ///    lockout (it is not a mailbox name), was otherwise guessable at full speed.
   ///    A wrong one now costs a growing, capped delay - on every route that takes
   ///    it, since HTTP Basic is accepted on all of them - and the right one signs in
   ///    once the delay has passed.
   /// </summary>
   [TestFixture]
   public class RestApiHostAndThrottle : TestFixtureBase
   {
      private static int RestPort = 9131;
      private const string AdminPassword = "testar";

      private static string AdminCredential =>
         "Basic " + Convert.ToBase64String(Encoding.ASCII.GetBytes("Administrator:" + AdminPassword));

      private static string WrongAdminCredential =>
         "Basic " + Convert.ToBase64String(Encoding.ASCII.GetBytes("Administrator:" + AdminPassword + "-wrong"));

      [SetUp]
      public void StartRestApi()
      {
         _settings.SetAdministratorPassword(AdminPassword);
         RestPort = RestListener.Start(RestPort);
         _application.Reinitialize();

         Response probe = Send("GET", "/api/v1/status", "127.0.0.1", AdminCredential, null);
         ClassicAssert.AreEqual(200, probe.Status, "REST API did not answer /api/v1/status. Body: " + probe.Body);
      }

      [TearDown]
      public void StopRestApi()
      {
         RestListener.Stop();
         _application.Reinitialize();
      }

      [Test]
      [Description("The plain listener answers under a loopback name, any name under .localhost, an IP address literal (none of them a name a rebinding page can stand under) and no Host at all, and refuses an unexpected name with 421 before it is authenticated - the defeat of DNS rebinding against the plain loopback console. A name that merely begins like an address or like localhost is not taken for one.")]
      public void AnUnexpectedHostIsRefused()
      {
         ClassicAssert.AreEqual(200, Send("GET", "/", "127.0.0.1", null, null).Status, "A loopback IP Host is this server's own.");
         ClassicAssert.AreEqual(200, Send("GET", "/", "127.0.0.1:" + RestPort, null, null).Status, "The port is optional on the loopback Host.");
         ClassicAssert.AreEqual(200, Send("GET", "/", "[::1]:" + RestPort, null, null).Status, "The IPv6 loopback, bracketed, is this server's own.");
         ClassicAssert.AreEqual(200, Send("GET", "/", "localhost", null, null).Status, "localhost is a loopback name.");
         ClassicAssert.AreEqual(200, Send("GET", "/", "deck.localhost", null, null).Status, "A name under .localhost never leaves the browser.");
         ClassicAssert.AreEqual(200, Send("GET", "/", "192.0.2.10", null, null).Status,
            "An IP address is not a name a rebinding page can stand under; a proxy reached by address forwards one.");
         ClassicAssert.AreEqual(200, Send("GET", "/", "[2001:db8::10]:443", null, null).Status, "An IPv6 address literal likewise.");
         ClassicAssert.AreEqual(200, Send("GET", "/", null, null, null).Status, "An HTTP/1.0 request with no Host is not a browser rebinding vector and is allowed.");

         ClassicAssert.AreEqual(421, Send("GET", "/", "rebind.evil.example", null, null).Status,
            "A Host this server does not answer to is a rebound name and must be refused before anything is served.");
         ClassicAssert.AreEqual(421, Send("POST", "/api/v1/session", "rebind.evil.example", AdminCredential, null).Status,
            "The rebinding page cannot even reach the sign-in route under its own name.");
         ClassicAssert.AreEqual(421, Send("GET", "/api/v1/status", "attacker.test:8045", AdminCredential, null).Status,
            "A port does not make an unexpected Host expected.");
         ClassicAssert.AreEqual(421, Send("GET", "/", "127.0.0.1.rebind.evil.example", null, null).Status,
            "A name that begins like an address is a name, and an attacker's.");
         ClassicAssert.AreEqual(421, Send("GET", "/", "localhost.evil.example", null, null).Status,
            "A name that begins with localhost is a name, and an attacker's.");
         ClassicAssert.AreEqual(421, Send("GET", "/", "1.2.3.4.5", null, null).Status, "Five numbers are not an IPv4 address.");
      }

      [Test]
      [Description("The server's own host name (Settings.HostName, the name it greets mail with, usually the name its webmail is published at) is a Host the plain listener answers to, so a reverse proxy that forwards it works without another setting; cleared, the same name is refused again.")]
      public void TheServersHostNameIsAnswered()
      {
         string before = _settings.HostName;
         try
         {
            _settings.HostName = "mx.proxy.test";
            ClassicAssert.AreEqual(200, Send("GET", "/", "mx.proxy.test", null, null).Status,
               "The server's own host name must be answered, or a proxy forwarding it stops working.");
            ClassicAssert.AreEqual(200, Send("GET", "/", "MX.Proxy.Test:443", null, null).Status,
               "A Host is compared without its case and its port.");
         }
         finally
         {
            _settings.HostName = before;
         }

         ClassicAssert.AreEqual(421, Send("GET", "/", "mx.proxy.test", null, null).Status,
            "A host name no longer configured must stop being answered.");
      }

      [Test]
      [Description("A wrong administrator password from loopback is throttled on EVERY route that takes it, not only POST /api/v1/session: a guess is answered 401, the next inside the delay is refused 429 with a Retry-After without being evaluated - on the sign-in route and on any other, which is where the guesses would otherwise have moved - and once the delay has passed the right password is answered and signs in. The negative control for the bypass: GET /api/v1/status with a wrong password inside the delay, which the route-only throttle answered 401, that is, evaluated.")]
      public void AWrongAdministratorPasswordIsThrottledOnEveryRoute()
      {
         // Three evaluated wrong guesses, each after the delay the one before set
         // has passed: 0.5 s, then 1 s, then a 2 s delay, which leaves time to make
         // the requests below inside it on any machine.
         for (int i = 0; i < 3; i++)
            Assert.AreEqual(401, EvaluatedWrongGuess("POST", "/api/v1/session"), "Wrong guess " + i + " was not evaluated and refused.");

         Response onSession = Send("POST", "/api/v1/session", "127.0.0.1", WrongAdminCredential, null);
         ClassicAssert.AreEqual(429, onSession.Status, "A wrong administrator password inside the delay must be refused 429: " + onSession.Body);
         Assert.Greater(RetryAfter(onSession.Headers), 0, "The throttle's 429 must carry a Retry-After telling the caller how long to wait.");

         // The bypass, closed: the same guess on another route that takes Basic.
         Response onStatus = Send("GET", "/api/v1/status", "127.0.0.1", WrongAdminCredential, null);
         ClassicAssert.AreEqual(429, onStatus.Status,
            "A wrong administrator password on /api/v1/status inside the delay was evaluated - the throttle is bypassed one route over: " + onStatus.Body);

         // And not evaluated: the right password inside the delay is refused the
         // same way, which is what makes the 429 say nothing about the guess.
         Response rightInside = Send("GET", "/api/v1/status", "127.0.0.1", AdminCredential, null);
         ClassicAssert.AreEqual(429, rightInside.Status,
            "A password inside the delay was evaluated - a 200 here for the right one and a 429 for a wrong one would tell a guesser which was which: " + rightInside.Body);

         // A delay, not a lockout: once it has passed, the right password is
         // answered on every route and signs in, which also clears the throttle.
         Thread.Sleep((RetryAfter(rightInside.Headers) + 1) * 1000);
         Response afterwards = Send("GET", "/api/v1/status", "127.0.0.1", AdminCredential, null);
         ClassicAssert.AreEqual(200, afterwards.Status, "After the delay the right administrator password must be answered: " + afterwards.Body);
         Response signedIn = Send("POST", "/api/v1/session", "127.0.0.1", AdminCredential, null);
         ClassicAssert.AreEqual(201, signedIn.Status, "After the delay the right administrator password must sign in: " + signedIn.Body);
      }

      [Test]
      [Description("A wrong account sign-in is not refused with the administrator throttle's 429: accounts are governed by the per-name lockout, and the administrator throttle is for the one credential that lockout does not cover.")]
      public void AnAccountSignInIsNotCaughtByTheAdministratorThrottle()
      {
         Account account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "throttle@" + _domain.Name, "Original-Passw0rd!");
         string wrong = "Basic " + Convert.ToBase64String(Encoding.ASCII.GetBytes(account.Address + ":wrong-once"));

         Response first = Send("POST", "/api/v1/session", "127.0.0.1", wrong, null);
         ClassicAssert.AreEqual(401, first.Status, "A single wrong account sign-in is a plain 401: " + first.Body);
         Response second = Send("POST", "/api/v1/session", "127.0.0.1", wrong, null);
         ClassicAssert.AreEqual(401, second.Status,
            "A second wrong account sign-in straight after is a plain 401 too, not the administrator throttle's 429: " + second.Body);
      }

      [Test]
      [Description("A Host an administrator configured as the address the webmail is reached at from outside is accepted, so a reverse proxy that forwards the outside name keeps working. The confidential/digest/S-MIME/list base URLs and - the gap this guards - the single-sign-in return address (oidc_redirect_base_url, which OIDC and SAML advertise as their callback and a browser returns to under that name) are each taken as a name this server answers to. Cleared, the same Host is refused again, so it was the configuration and not a blanket allow. Without the single-sign-in host in the allow set, an OIDC/SAML sign-in behind a host-forwarding proxy would be refused 421 at its callback.")]
      public void AConfiguredOutsideNameIsAcceptedForAReverseProxy()
      {
         try
         {
            // A base URL whose host is the outside name. Set over the admin API
            // (EffectNow), which is what the Host check reads live each request.
            ClassicAssert.AreEqual(200, Send("PUT", "/api/v1/settings", "127.0.0.1", AdminCredential,
               "{\"confidential_base_url\":\"https://mail.proxy.test\"}").Status, "Setting the confidential base URL failed.");
            ClassicAssert.AreEqual(200, Send("GET", "/", "mail.proxy.test", null, null).Status,
               "A Host an administrator configured as the webmail's outside address must be answered, or a reverse proxy that forwards it stops working.");
            ClassicAssert.AreEqual(200, Send("GET", "/", "mail.proxy.test:" + RestPort, null, null).Status,
               "The port is optional on a configured outside Host too.");

            // The single-sign-in return address: without the fix this host is not
            // in the allow set and the OIDC/SAML callback is refused 421.
            ClassicAssert.AreEqual(200, Send("PUT", "/api/v1/settings", "127.0.0.1", AdminCredential,
               "{\"oidc_redirect_base_url\":\"https://sso.proxy.test\"}").Status, "Setting the single-sign-in redirect base URL failed.");
            ClassicAssert.AreEqual(200, Send("GET", "/", "sso.proxy.test", null, null).Status,
               "The single-sign-in return address must be a Host this server answers to, or an OIDC/SAML sign-in behind a host-forwarding proxy cannot complete.");

            // An outside name that was not configured is still refused, so the
            // allow is the configuration and not a blanket one.
            ClassicAssert.AreEqual(421, Send("GET", "/", "unconfigured.proxy.test", null, null).Status,
               "A Host no setting names must still be refused.");
         }
         finally
         {
            Send("PUT", "/api/v1/settings", "127.0.0.1", AdminCredential,
               "{\"confidential_base_url\":\"\",\"oidc_redirect_base_url\":\"\"}");
         }

         // Cleared: the once-configured name is refused again.
         ClassicAssert.AreEqual(421, Send("GET", "/", "mail.proxy.test", null, null).Status,
            "A base URL cleared must stop its host being answered.");
         ClassicAssert.AreEqual(421, Send("GET", "/", "sso.proxy.test", null, null).Status,
            "A single-sign-in redirect base URL cleared must stop its host being answered.");
      }

      [Test]
      [Description("HSTS is sent to a browser that came over TLS - here through the reverse proxy on this machine, which says so with X-Forwarded-Proto: https - for the name it used and not the names under it (no includeSubDomains: this listener does not own them), and never on the plain-loopback path, where there is no TLS to pin. The negative control is the plain request, which must carry none.")]
      public void HstsIsSentOnlyWhenTheBrowserCameOverTls()
      {
         Response plain = Send("GET", "/", "127.0.0.1", null, null);
         ClassicAssert.AreEqual(200, plain.Status);
         ClassicAssert.IsNull(plain.Header("Strict-Transport-Security"), "HSTS on the plain-loopback path: " + plain.Headers);

         Response proxied = Send("GET", "/", "127.0.0.1", null, null, "X-Forwarded-Proto: https\r\n");
         ClassicAssert.AreEqual(200, proxied.Status);
         ClassicAssert.AreEqual("max-age=31536000", proxied.Header("Strict-Transport-Security"),
            "A browser that came over TLS through the loopback proxy must be told to stay on it: " + proxied.Headers);

         Response api = Send("GET", "/api/v1/status", "127.0.0.1", AdminCredential, null, "X-Forwarded-Proto: https\r\n");
         ClassicAssert.AreEqual(200, api.Status);
         ClassicAssert.AreEqual("max-age=31536000", api.Header("Strict-Transport-Security"), "The API's answers carry it too.");
      }

      [Test]
      [Description("A listener with its own certificate sends HSTS on every answer, and answers any Host: its certificate is what stops a rebound name (the browser refuses a certificate for another name), so an administrator reaching it by an address or by a name this server was not told is not refused.")]
      public void ATlsListenerSendsHstsAndAnswersAnyHost()
      {
         if (RestListener.Provided > 0)
            Assert.Ignore("The suite's own listener is in use; this test moves it to TLS.");

         try
         {
            IniFileSetting.Write("RestApiCertificateFile", Paths.Combine(SslSetup.GetSslCertPath(), "example.crt"));
            IniFileSetting.Write("RestApiPrivateKeyFile", Paths.Combine(SslSetup.GetSslCertPath(), "example.key"));
            _application.Reinitialize();
            Thread.Sleep(500);

            Response byName = Send("GET", "/", "not-configured.tls.test", null, null, null, true);
            ClassicAssert.AreEqual(200, byName.Status, "A TLS listener refused a Host it was not told: " + byName.Body);
            ClassicAssert.AreEqual("max-age=31536000", byName.Header("Strict-Transport-Security"), "No HSTS over TLS: " + byName.Headers);

            Response byAddress = Send("GET", "/api/v1/status", "127.0.0.1:" + RestPort, AdminCredential, null, null, true);
            ClassicAssert.AreEqual(200, byAddress.Status, byAddress.Body);
            ClassicAssert.AreEqual("max-age=31536000", byAddress.Header("Strict-Transport-Security"));
         }
         finally
         {
            IniFileSetting.Write("RestApiCertificateFile", "");
            IniFileSetting.Write("RestApiPrivateKeyFile", "");
         }
      }

      // ----------------------------------------------------------------- helpers

      // A wrong administrator password that the server evaluates: sent, and sent
      // again after the Retry-After of any 429, until it is answered with
      // something else. Its status.
      private static int EvaluatedWrongGuess(string method, string path)
      {
         for (int attempt = 0; attempt < 10; attempt++)
         {
            Response response = Send(method, path, "127.0.0.1", WrongAdminCredential, null);
            if (response.Status != 429)
               return response.Status;
            Thread.Sleep(RetryAfter(response.Headers) * 1000 + 100);
         }
         return 429;
      }

      private static int RetryAfter(string headers)
      {
         foreach (string line in headers.Split(new[] { "\r\n" }, StringSplitOptions.None))
         {
            if (line.StartsWith("Retry-After:", StringComparison.OrdinalIgnoreCase))
            {
               if (int.TryParse(line.Substring("Retry-After:".Length).Trim(), out int seconds))
                  return seconds;
            }
         }
         return 0;
      }

      private sealed class Response
      {
         public int Status;
         public string Headers;
         public string Body;

         public string Header(string name)
         {
            foreach (string line in Headers.Split(new[] { "\r\n" }, StringSplitOptions.None))
            {
               if (line.StartsWith(name + ":", StringComparison.OrdinalIgnoreCase))
                  return line.Substring(name.Length + 1).Trim();
            }
            return null;
         }
      }

      // A raw request with a Host of the caller's choosing (or none), connecting
      // to the loopback listener regardless of what the Host says - which is how a
      // rebinding page reaches this address while naming another. Over TLS when
      // asked, accepting the test certificate whatever name it carries.
      private static Response Send(string method, string path, string hostHeader, string authorization, string requestBody,
                                   string extraHeaders = null, bool tls = false)
      {
         using (var client = new TcpClient())
         {
            Exception last = null;
            for (int attempt = 0; attempt < 25; attempt++)
            {
               try
               {
                  client.Connect("127.0.0.1", RestPort);
                  last = null;
                  break;
               }
               catch (SocketException ex)
               {
                  last = ex;
                  Thread.Sleep(200);
               }
            }

            if (last != null)
               throw last;

            using (NetworkStream network = client.GetStream())
            using (var memory = new MemoryStream())
            {
               Stream stream = network;
               SslStream ssl = null;
               if (tls)
               {
                  ssl = new SslStream(network, false, (sender, certificate, chain, errors) => true);
                  ssl.AuthenticateAsClient("localhost", null, SslProtocols.Tls12 | SslProtocols.Tls13, false);
                  stream = ssl;
               }

               try
               {
                  var head = new StringBuilder();
                  head.Append(method + " " + path + " HTTP/1.0\r\n");
                  if (hostHeader != null)
                     head.Append("Host: " + hostHeader + "\r\n");
                  if (authorization != null)
                     head.Append("Authorization: " + authorization + "\r\n");
                  if (extraHeaders != null)
                     head.Append(extraHeaders);

                  byte[] bodyBytes = requestBody == null ? new byte[0] : Encoding.UTF8.GetBytes(requestBody);
                  if (requestBody != null)
                  {
                     head.Append("Content-Type: application/json\r\n");
                     head.Append("Content-Length: " + bodyBytes.Length + "\r\n");
                  }

                  head.Append("Connection: close\r\n\r\n");

                  byte[] headerBytes = Encoding.ASCII.GetBytes(head.ToString());
                  stream.Write(headerBytes, 0, headerBytes.Length);
                  if (bodyBytes.Length > 0)
                     stream.Write(bodyBytes, 0, bodyBytes.Length);

                  byte[] buffer = new byte[4096];
                  int read;
                  try
                  {
                     while ((read = stream.Read(buffer, 0, buffer.Length)) > 0)
                        memory.Write(buffer, 0, read);
                  }
                  catch (IOException) when (tls)
                  {
                     // A TLS peer that closes without a close_notify ends the read
                     // here; what was read is the answer.
                  }
               }
               finally
               {
                  ssl?.Dispose();
               }

               string raw = Encoding.UTF8.GetString(memory.ToArray());

               var response = new Response();
               int split = raw.IndexOf("\r\n\r\n", StringComparison.Ordinal);
               response.Headers = split >= 0 ? raw.Substring(0, split) : raw;
               response.Body = split >= 0 ? raw.Substring(split + 4) : "";

               string[] lines = response.Headers.Split(new[] { "\r\n" }, StringSplitOptions.None);
               if (lines.Length > 0)
               {
                  string[] parts = lines[0].Split(' ');
                  if (parts.Length >= 2)
                     int.TryParse(parts[1], out response.Status);
               }

               return response;
            }
         }
      }
   }
}
