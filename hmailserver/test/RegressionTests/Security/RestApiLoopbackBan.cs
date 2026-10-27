// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Net;
using System.Net.Security;
using System.Net.Sockets;
using System.Security.Authentication;
using System.Text;
using System.Threading;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;
using RegressionTests.SSL;

namespace RegressionTests.Security
{
   /// <summary>
   ///    An account's failed sign-in at the REST listener from loopback. Every
   ///    browser behind a reverse proxy on this machine reaches the listener from
   ///    127.0.0.1 - the one peer whose X-Forwarded-Proto the listener believes - and
   ///    from 6.2.28 to 6.3.7 an account's failed HTTP Basic sign-in there, on any
   ///    route and POST /api/v1/session among them (the webmail's and the portal's
   ///    sign-in), was charged to that address by AccountLogon::Logon: three wrong
   ///    passwords from anybody behind the proxy banned 127.0.0.1 for AutoBanMinutes
   ///    from SMTP, POP3, IMAP and ManageSieve, and with it a webmail on this machine
   ///    and every local script. The listener's own accounting
   ///    (RegisterAuthenticationFailure_) spared loopback, but it ran after Logon,
   ///    which had charged the address already. Now such a failure is counted against
   ///    the name alone - the per-name lockout - and never towards a ban of the
   ///    address, and no "logon failure" line is written for it; from any other
   ///    address it is charged as before.
   ///
   ///    The loopback tests run on both suites, against the listener the suite has:
   ///    the Windows bench's own on a port of this fixture's, the Linux suite's the
   ///    one its fixture layer talks to. The test from another address needs the
   ///    listener on every address with TLS, which the Linux suite's listener is not
   ///    and cannot be made while the suite talks to it, so it runs on the Windows
   ///    bench alone. Every range a test writes is deleted again when it ends, and
   ///    the logon failures are cleared.
   /// </summary>
   [TestFixture]
   public class RestApiLoopbackBan : TestFixtureBase
   {
      private static int RestPort = 9546;
      private const int Threshold = 3;
      private const string AdminPassword = "testar";
      private const string AutoBanPrefix = "Auto-ban: ";
      private const string UserPassword = "Rest-Ban-Passw0rd!";
      private const string WrongPassword = "not-the-password";

      private string _account;

      /// <summary>
      ///    An account of the test's own, so that no test meets a name another
      ///    test's failures locked: the per-name lockout lives as long as the
      ///    service, and a locked name is refused before anything is charged.
      /// </summary>
      private void AddAccount(string localPart)
      {
         _account = localPart + "@" + _domain.Name;
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, _account, UserPassword);
      }

      [SetUp]
      public void StartTheListener()
      {
         _settings.SetAdministratorPassword(AdminPassword);

         _settings.ClearLogonFailureList();
         _settings.AutoBanOnLogonFailure = true;
         _settings.MaxInvalidLogonAttempts = Threshold;
         _settings.MaxInvalidLogonAttemptsWithin = 5;
         _settings.AutoBanMinutes = 3;

         RestPort = RestListener.Start(RestPort);
         IniFileSetting.Write("RestApiCertificateFile", "");
         IniFileSetting.Write("RestApiPrivateKeyFile", "");

         Restart();
      }

      [TearDown]
      public void StopTheListener()
      {
         var ranges = _settings.SecurityRanges;
         for (var i = ranges.Count - 1; i >= 0; i--)
         {
            var range = ranges[i];
            if (range.Name.StartsWith(AutoBanPrefix))
               ranges.DeleteByDBID(range.ID);
         }

         _settings.ClearLogonFailureList();
         _settings.AutoBanOnLogonFailure = false;

         IniFileSetting.Delete("AccountLockoutThreshold");
         RestListener.Stop();
         if (RestListener.Provided == 0)
            IniFileSetting.Write("RestApiBindAddress", "127.0.0.1");
         IniFileSetting.Write("RestApiCertificateFile", "");
         IniFileSetting.Write("RestApiPrivateKeyFile", "");
         _application.Reinitialize();
      }

      /// <summary>
      ///    The server reinitialised, and back once its REST listener answers the
      ///    administrator over loopback.
      /// </summary>
      private void Restart()
      {
         _application.Reinitialize();

         var deadline = DateTime.UtcNow.AddSeconds(20);
         var status = -1;
         while (DateTime.UtcNow < deadline)
         {
            status = StatusOf(Loopback("GET", "/api/v1/status", Basic("Administrator", AdminPassword)));
            if (status == 200)
               break;
            Thread.Sleep(200);
         }

         ClassicAssert.AreEqual(200, status, "The REST listener did not answer GET /api/v1/status over loopback.");
      }

      [Test]
      [Description("Three failed account sign-ins at GET /api/v1/me from loopback - a proxy on this machine - write " +
                   "no auto-ban for 127.0.0.1 and no logon-failure line, so SMTP and IMAP still greet loopback; " +
                   "they are counted against the name, which the per-name lockout then refuses with the right password.")]
      public void FailedAccountSignInsFromLoopbackBanNothingAndCountAgainstTheName()
      {
         ThreeFailuresFromLoopback("GET", "/api/v1/me", 200, "restban-me");
      }

      [Test]
      [Description("Three failed sign-ins at POST /api/v1/session from loopback - the webmail's and the portal's " +
                   "sign-in behind a proxy on this machine - write no auto-ban for 127.0.0.1 and no logon-failure " +
                   "line, so SMTP and IMAP still greet loopback; they are counted against the name, which the " +
                   "per-name lockout then refuses with the right password.")]
      public void FailedSessionSignInsFromLoopbackBanNothingAndCountAgainstTheName()
      {
         ThreeFailuresFromLoopback("POST", "/api/v1/session", 201, "restban-session");
      }

      [Test]
      [Description("Three failed account sign-ins over HTTPS from this machine's own address that is not loopback " +
                   "are still charged to it: an auto-ban is written for the address.")]
      public void FailedAccountSignInsFromAnotherAddressAreStillCharged()
      {
         if (RestListener.Provided > 0)
            Assert.Ignore("The suite's own REST listener is plain HTTP on loopback, and the fixture layer talks to it; it cannot be moved to every address with TLS while the suite runs.");

         var local = TestSetup.GetLocalIpAddress();
         if (local == null || IPAddress.IsLoopback(local))
            Assert.Ignore("This machine has no address other than loopback that the server answers on.");

         AddAccount("restban-remote");

         IniFileSetting.Write("RestApiBindAddress", "0.0.0.0");
         IniFileSetting.Write("RestApiCertificateFile", Paths.Combine(SslSetup.GetSslCertPath(), "example.crt"));
         IniFileSetting.Write("RestApiPrivateKeyFile", Paths.Combine(SslSetup.GetSslCertPath(), "example.key"));
         _application.Reinitialize();

         var deadline = DateTime.UtcNow.AddSeconds(20);
         var probe = -1;
         while (DateTime.UtcNow < deadline)
         {
            probe = StatusOf(Exchange(local, true, "GET", "/api/v1/status", Basic("Administrator", AdminPassword), ""));
            if (probe == 200)
               break;
            Thread.Sleep(200);
         }

         ClassicAssert.AreEqual(200, probe, "The REST listener did not answer GET /api/v1/status over HTTPS from " + local + ".");

         for (var i = 1; i <= Threshold; i++)
         {
            var refusal = Exchange(local, true, "GET", "/api/v1/me", Basic(_account, WrongPassword), "");
            ClassicAssert.AreEqual(401, StatusOf(refusal), "Failed sign-in " + i + " from " + local + " was not refused with 401: " + refusal);
         }

         // Charged by the account's logon, under the account's name, as before: the
         // listener's own accounting of the refusal, which charges the address too,
         // names its range "Auto-ban: REST API".
         ClassicAssert.IsNotNull(RangeHolding(local),
            "No auto-ban was written for " + local + " after " + Threshold + " failed account sign-ins from it. The ranges: " + RangeNames());
         ClassicAssert.IsNotNull(RangeHolding(local, AutoBanPrefix + _account),
            "No auto-ban for " + local + " was written by the account's logon, under the account's name. The ranges: " + RangeNames());
      }

      // ------------------------------------------------------------------ helpers

      private void ThreeFailuresFromLoopback(string method, string path, int signedIn, string localPart)
      {
         AddAccount(localPart);

         // The name is not locked, and loopback is greeted, before the failures.
         ClassicAssert.AreEqual(signedIn, StatusOf(Loopback(method, path, Basic(_account, UserPassword))),
            "The right password from loopback was not let in before the failed sign-ins.");
         AssertGreeted(TestPorts.Smtp, "220", "before the failed sign-ins");
         LogHandler.DeleteCurrentDefaultLog();

         IniFileSetting.Write("AccountLockoutThreshold", Threshold.ToString());
         Restart();

         for (var i = 1; i <= Threshold; i++)
         {
            var refusal = Loopback(method, path, Basic(_account, WrongPassword));
            ClassicAssert.AreEqual(401, StatusOf(refusal), "Failed sign-in " + i + " at " + method + " " + path + " from loopback was not refused with 401: " + refusal);
         }

         ClassicAssert.IsNull(RangeHolding(IPAddress.Loopback),
            "An auto-ban was written for 127.0.0.1 after " + Threshold + " failed account sign-ins at " + method + " " + path +
            " from loopback. The ranges: " + RangeNames());
         ClassicAssert.IsFalse(LogHandler.DefaultLogContains("from 127.0.0.1 for " + _account),
            "A failed sign-in from loopback was counted towards a ban of 127.0.0.1 (an \"Auto-ban: logon failure\" line names it):\r\n" +
            LogHandler.ReadCurrentDefaultLog());
         AssertGreeted(TestPorts.Smtp, "220", "after the failed sign-ins");
         AssertGreeted(TestPorts.Imap, "* OK", "after the failed sign-ins");

         ClassicAssert.AreEqual(401, StatusOf(Loopback(method, path, Basic(_account, UserPassword))),
            "The right password was not refused once " + Threshold + " failures had locked the name.");

         // With the threshold back at its default a successful sign-in erases the
         // name's entry in the lockout store, which lives as long as the service.
         IniFileSetting.Delete("AccountLockoutThreshold");
         Restart();

         ClassicAssert.AreEqual(signedIn, StatusOf(Loopback(method, path, Basic(_account, UserPassword))),
            "The right password from loopback was not let in once the lockout was off.");
      }

      /// <summary>
      ///    An auto-ban's range that holds the address alone - the one named name,
      ///    when a name is given.
      /// </summary>
      private hMailServer.SecurityRange RangeHolding(IPAddress address, string name = null)
      {
         var ranges = _settings.SecurityRanges;
         for (var i = 0; i < ranges.Count; i++)
         {
            var range = ranges[i];
            if (range.Name.StartsWith(AutoBanPrefix) && (name == null || range.Name == name) &&
                range.LowerIP == address.ToString() && range.UpperIP == address.ToString())
               return range;
         }

         return null;
      }

      private string RangeNames()
      {
         var names = new List<string>();
         var ranges = _settings.SecurityRanges;
         for (var i = 0; i < ranges.Count; i++)
            names.Add(ranges[i].Name + " (" + ranges[i].LowerIP + "-" + ranges[i].UpperIP + ")");
         return string.Join(" | ", names);
      }

      private static void AssertGreeted(int port, string expected, string when)
      {
         var greeting = Greeting(IPAddress.Loopback, port);
         ClassicAssert.IsTrue(greeting.StartsWith(expected),
            "The listener on port " + port + " did not greet loopback " + when + ": '" + greeting + "'");
      }

      /// <summary>
      ///    The first line a listener sends to a new connection from loopback; ""
      ///    when it closes or resets without one.
      /// </summary>
      private static string Greeting(IPAddress to, int port)
      {
         var client = new TcpClient(new IPEndPoint(to, 0));
         try
         {
            client.ReceiveTimeout = 15000;
            client.Connect(to, port);

            var stream = client.GetStream();
            var line = new StringBuilder();
            var one = new byte[1];
            while (!line.ToString().EndsWith("\r\n") && stream.Read(one, 0, 1) > 0)
               line.Append((char) one[0]);
            return line.ToString().TrimEnd('\r', '\n');
         }
         catch (IOException)
         {
            return "";
         }
         catch (SocketException)
         {
            return "";
         }
         finally
         {
            client.Close();
         }
      }

      private static string Basic(string user, string password)
      {
         return "Basic " + Convert.ToBase64String(Encoding.UTF8.GetBytes(user + ":" + password));
      }

      private static string Loopback(string method, string path, string authorization)
      {
         return Exchange(IPAddress.Loopback, false, method, path, authorization, "");
      }

      private static int StatusOf(string response)
      {
         int status;
         if (response == null || !response.StartsWith("HTTP/") || response.IndexOf(' ') < 0 ||
             response.Length < response.IndexOf(' ') + 4 ||
             !int.TryParse(response.Substring(response.IndexOf(' ') + 1, 3), out status))
            return -1;
         return status;
      }

      /// <summary>
      ///    One request on a new connection from the address given (the client's
      ///    socket bound to it) to the same address on the fixture's REST port, over
      ///    TLS when asked, and everything read until the server closes. "" when
      ///    nothing was answered.
      /// </summary>
      private static string Exchange(IPAddress from, bool tls, string method, string path, string authorization, string extraHeaders)
      {
         var client = new TcpClient(new IPEndPoint(from, 0));
         try
         {
            client.ReceiveTimeout = 15000;
            client.SendTimeout = 15000;
            client.Connect(from, RestPort);

            Stream stream = client.GetStream();
            if (tls)
            {
               var secure = new SslStream(stream, false, (sender, certificate, chain, errors) => true);
               secure.AuthenticateAsClient("mail.example.test", null, SslProtocols.Tls12 | SslProtocols.Tls13, false);
               stream = secure;
            }

            var request = method + " " + path + " HTTP/1.0\r\nHost: 127.0.0.1\r\n" +
                          (authorization != null ? "Authorization: " + authorization + "\r\n" : "") +
                          extraHeaders + "Connection: close\r\n" +
                          (method == "POST" ? "Content-Length: 0\r\n" : "") + "\r\n";
            var bytes = Encoding.ASCII.GetBytes(request);
            stream.Write(bytes, 0, bytes.Length);
            stream.Flush();

            using (var memory = new MemoryStream())
            {
               var buffer = new byte[8192];
               try
               {
                  int read;
                  while ((read = stream.Read(buffer, 0, buffer.Length)) > 0)
                     memory.Write(buffer, 0, read);
               }
               catch (IOException)
               {
               }

               return Encoding.UTF8.GetString(memory.ToArray());
            }
         }
         catch (IOException)
         {
            return "";
         }
         catch (SocketException)
         {
            return "";
         }
         catch (AuthenticationException)
         {
            return "";
         }
         finally
         {
            client.Close();
         }
      }
   }
}
