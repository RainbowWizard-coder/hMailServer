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
   ///    The IP ranges at the web services listener's accept. Up to 6.3.7 the
   ///    listener asked the country policy alone, so an address an IP range bans -
   ///    an auto-ban written for its failed CardDAV, CalDAV or Autodiscover sign-ins,
   ///    or an administrator's range that allows it nothing - was refused by every
   ///    mail listener and went on connecting here, and guessing passwords at the
   ///    DAV sign-in, for as long as it liked. Now a range that allows none of SMTP,
   ///    POP3 and IMAP refuses the connection, closed before a byte is read; a range
   ///    that allows SMTP alone does not, since this listener serves MTA-STS policies
   ///    to the world; and loopback and the trusted proxies, whose failures are
   ///    charged for every client behind them, are never refused by a range.
   ///
   ///    The suites connect from loopback, which the rule leaves alone, so these
   ///    tests connect from this machine's own private address that is not loopback
   ///    (TestSetup.GetLocalIpAddress; a run on a machine without one is ignored
   ///    there, saying why), to the listener bound to every address: plain HTTP for
   ///    the MTA-STS policy and HTTPS, with the suite's example certificate, for
   ///    the DAV sign-in, which is answered over HTTPS only and from an address the
   ///    listener does not take for a proxy. Over COM on Windows and REST on Linux
   ///    alike: the Linux suite links this fixture. Every range a test writes is
   ///    deleted again when it ends, and the logon failures are cleared.
   /// </summary>
   [TestFixture]
   public class WebServicesBan : TestFixtureBase
   {
      private const int HttpPort = 9181;
      private const int HttpsPort = 9182;
      private const int Threshold = 3;
      private const string AutoBanPrefix = "Auto-ban: ";
      private const string OwnPrefix = "WebServicesBan ";
      private const string UserPassword = "Web-Ban-Passw0rd!";

      private IPAddress _local;
      private string _account;

      private string PolicyHost
      {
         get { return "mta-sts." + _domain.Name; }
      }

      [SetUp]
      public void StartTheListener()
      {
         // This machine's private address that is not loopback, on which the server
         // answers: the one the fixtures that need to come from outside "My computer"
         // use. The Linux suite's TestSetup skips the test when there is none.
         _local = TestSetup.GetLocalIpAddress();
         if (_local == null || IPAddress.IsLoopback(_local))
            Assert.Ignore("This machine has no address other than loopback that the server answers on, and loopback is never refused by an IP range at the web services listener.");

         _account = "webban@" + _domain.Name;
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, _account, UserPassword);

         _settings.ClearLogonFailureList();
         _settings.AutoBanOnLogonFailure = true;
         _settings.MaxInvalidLogonAttempts = Threshold;
         _settings.MaxInvalidLogonAttemptsWithin = 5;
         _settings.AutoBanMinutes = 3;

         IniFileSetting.Write("WebServicesBindAddress", "0.0.0.0");
         IniFileSetting.Write("WebServicesHttpPort", HttpPort.ToString());
         IniFileSetting.Write("WebServicesHttpsPort", HttpsPort.ToString());
         IniFileSetting.Write("WebServicesCertificateFile", Paths.Combine(SslSetup.GetSslCertPath(), "example.crt"));
         IniFileSetting.Write("WebServicesPrivateKeyFile", Paths.Combine(SslSetup.GetSslCertPath(), "example.key"));
         IniFileSetting.Write("WebServicesTrustedProxies", "");
         IniFileSetting.Write("MtaStsHostingEnabled", "1");
         IniFileSetting.Write("MtaStsPolicyMx", "mail." + _domain.Name);

         Restart();
      }

      /// <summary>
      ///    The server reinitialised, and back once its web services listener
      ///    serves the MTA-STS policy over loopback.
      /// </summary>
      private void Restart()
      {
         _application.Reinitialize();

         var deadline = DateTime.UtcNow.AddSeconds(20);
         int status = -1;
         while (DateTime.UtcNow < deadline)
         {
            status = StatusOf(Policy(IPAddress.Loopback));
            if (status == 200)
               break;
            Thread.Sleep(200);
         }

         ClassicAssert.AreEqual(200, status, "The web services listener did not serve the MTA-STS policy over loopback.");
      }

      [TearDown]
      public void StopTheListener()
      {
         var ranges = _settings.SecurityRanges;
         for (var i = ranges.Count - 1; i >= 0; i--)
         {
            var range = ranges[i];
            if (range.Name.StartsWith(AutoBanPrefix) || range.Name.StartsWith(OwnPrefix))
               ranges.DeleteByDBID(range.ID);
         }

         _settings.ClearLogonFailureList();
         _settings.AutoBanOnLogonFailure = false;

         IniFileSetting.Write("WebServicesHttpPort", "0");
         IniFileSetting.Write("WebServicesHttpsPort", "0");
         IniFileSetting.Write("WebServicesCertificateFile", "");
         IniFileSetting.Write("WebServicesPrivateKeyFile", "");
         IniFileSetting.Write("WebServicesTrustedProxies", "");
         IniFileSetting.Write("MtaStsPolicyMx", "");
         IniFileSetting.Write("WebServicesBindAddress", "127.0.0.1");
         _application.Reinitialize();
      }

      [Test]
      [Description("Three failed CardDAV sign-ins over HTTPS from this machine's own address write an auto-ban, " +
                   "and the address's next connection to the web services listener is refused, over HTTP and " +
                   "over HTTPS, while loopback is still served.")]
      public void AnAddressBannedForFailedDavSignInsIsRefusedAtItsNextConnection()
      {
         AssertServed(_local, "before any sign-in");

         for (var i = 1; i <= Threshold; i++)
         {
            var status = StatusOf(DavOverHttps(_local, _account, "not-the-password"));
            ClassicAssert.AreEqual(401, status, "Failed sign-in " + i + " from " + _local + " was not refused with 401.");
         }

         var ban = RangeHolding(_local, AutoBanPrefix);
         ClassicAssert.IsNotNull(ban, "No auto-ban was written for " + _local + " after " + Threshold + " failed sign-ins. The ranges: " + RangeNames());

         AssertRefused(_local, "after the auto-ban");
         ClassicAssert.AreEqual("", DavOverHttps(_local, _account, UserPassword),
            "A new HTTPS connection from the banned address was answered.");
         AssertServed(IPAddress.Loopback, "from loopback, which no ban holds");
      }

      [Test]
      [Description("An auto-ban's range written over the API for this machine's own address - expiring, allowing " +
                   "nothing - refuses the address's next connection.")]
      public void AnAddressAnAutoBanWrittenOverTheApiHoldsIsRefused()
      {
         AssertServed(_local, "before the ban");

         StandRange(AutoBanPrefix + "webban written by the test", _local.ToString(), false, false, false, true);

         AssertRefused(_local, "with an auto-ban's range in force");
         AssertServed(IPAddress.Loopback, "from loopback");
      }

      [Test]
      [Description("An administrator's range that allows nothing, not an auto-ban's and not expiring, refuses the " +
                   "address's next connection.")]
      public void AnAdministratorsRangeThatAllowsNothingIsRefused()
      {
         StandRange(OwnPrefix + "deny everything", _local.ToString(), false, false, false, false);

         AssertRefused(_local, "with an administrator's range that allows nothing");
      }

      [Test]
      [Description("A range that allows SMTP alone - the Internet range of a server that keeps IMAP and POP3 to " +
                   "its own network - does not refuse: the MTA-STS policy is served to the address.")]
      public void ARangeThatAllowsSmtpAloneIsNotRefused()
      {
         StandRange(OwnPrefix + "SMTP alone", _local.ToString(), true, false, false, false);

         AssertServed(_local, "with a range that allows SMTP alone");
      }

      [Test]
      [Description("Loopback is not refused while a range that allows nothing holds 127.0.0.1: a proxy on this " +
                   "machine answers for every client behind it.")]
      public void LoopbackIsNotRefusedWhileABanHoldsIt()
      {
         StandRange(AutoBanPrefix + "webban loopback", "127.0.0.1", false, false, false, true);

         AssertServed(IPAddress.Loopback, "while a ban holds 127.0.0.1");
      }

      [Test]
      [Description("A trusted proxy (WebServicesTrustedProxies) is not refused while a range that allows nothing " +
                   "holds its address, and its X-Forwarded-Proto is still believed: a failed sign-in through it is " +
                   "answered 401.")]
      public void ATrustedProxyIsNotRefusedWhileABanHoldsIt()
      {
         IniFileSetting.Write("WebServicesTrustedProxies", _local.ToString());
         Restart();

         StandRange(AutoBanPrefix + "webban proxy", _local.ToString(), false, false, false, true);

         AssertServed(_local, "as a trusted proxy a ban holds");

         var refusal = Exchange(_local, HttpPort, false,
            Request("OPTIONS", "/dav/", PolicyHost, Basic(_account, "not-the-password"), "X-Forwarded-Proto: https\r\n", true));
         ClassicAssert.AreEqual(401, StatusOf(refusal), "A failed sign-in through the trusted proxy was not answered 401: " + refusal);
      }

      [Test]
      [Description("A connection opened before its address was banned, and kept alive, is closed at its next failed " +
                   "sign-in, and the address's next connection is refused.")]
      public void AConnectionOpenedBeforeTheBanIsClosedAtItsNextFailedSignIn()
      {
         using (var connection = new KeptConnection(_local, HttpsPort))
         {
            var first = connection.Send(Request("OPTIONS", "/dav/", PolicyHost, Basic(_account, UserPassword), "", false));
            ClassicAssert.AreEqual(200, StatusOf(first), "The signed-in OPTIONS before the ban was not answered 200: " + first);
            ClassicAssert.IsTrue(connection.IsOpen(), "The connection was not kept alive after a successful sign-in.");

            StandRange(AutoBanPrefix + "webban kept alive", _local.ToString(), false, false, false, true);

            var second = connection.Send(Request("OPTIONS", "/dav/", PolicyHost, Basic(_account, "not-the-password"), "", false));
            ClassicAssert.AreEqual(401, StatusOf(second), "The failed sign-in on the kept connection was not answered 401: " + second);
            ClassicAssert.IsFalse(connection.IsOpen(), "The connection opened before the ban was not closed at its failed sign-in.");
         }

         AssertRefused(_local, "after the failed sign-in");
      }

      // ------------------------------------------------------------------ helpers

      private void StandRange(string name, string address, bool smtp, bool pop3, bool imap, bool expires)
      {
         var range = _settings.SecurityRanges.Add();
         range.Name = name;
         range.LowerIP = address;
         range.UpperIP = address;
         range.Priority = 100;
         range.AllowSMTPConnections = smtp;
         range.AllowPOP3Connections = pop3;
         range.AllowIMAPConnections = imap;
         range.Expires = expires;
         if (expires)
            range.ExpiresTime = DateTime.Now.AddMinutes(30);
         range.Save();

         ClassicAssert.IsNotNull(RangeHolding(IPAddress.Parse(address), name),
            "The range the test stands on was not stored. The ranges: " + RangeNames());
      }

      private hMailServer.SecurityRange RangeHolding(IPAddress address, string namePrefix)
      {
         var ranges = _settings.SecurityRanges;
         for (var i = 0; i < ranges.Count; i++)
         {
            var range = ranges[i];
            if (range.Name.StartsWith(namePrefix) && range.LowerIP == address.ToString() && range.UpperIP == address.ToString())
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

      private void AssertServed(IPAddress from, string when)
      {
         var response = Policy(from);
         ClassicAssert.AreEqual(200, StatusOf(response), "The MTA-STS policy was not served to " + from + " " + when + ": '" + response + "'");
         StringAssert.Contains("version: STSv1", response, "The MTA-STS policy served to " + from + " " + when + " is not one.");
      }

      private void AssertRefused(IPAddress from, string when)
      {
         ClassicAssert.AreEqual("", Policy(from), "A new HTTP connection from " + from + " " + when + " was answered.");
      }

      private string Policy(IPAddress from)
      {
         return Exchange(from, HttpPort, false, Request("GET", "/.well-known/mta-sts.txt", PolicyHost, null, "", true));
      }

      private string DavOverHttps(IPAddress from, string user, string password)
      {
         return Exchange(from, HttpsPort, true, Request("OPTIONS", "/dav/", PolicyHost, Basic(user, password), "", true));
      }

      private static string Basic(string user, string password)
      {
         return "Basic " + Convert.ToBase64String(Encoding.UTF8.GetBytes(user + ":" + password));
      }

      private static string Request(string method, string path, string host, string authorization, string extraHeaders, bool close)
      {
         return method + " " + path + " HTTP/1.1\r\nHost: " + host + "\r\n" +
                (authorization != null ? "Authorization: " + authorization + "\r\n" : "") +
                extraHeaders + (close ? "Connection: close\r\n" : "") + "Content-Length: 0\r\n\r\n";
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
      ///    socket bound to it) to the same address, over TLS when asked, and
      ///    everything read until the server closes. "" when nothing was answered:
      ///    the connection closed or reset at once, or the TLS handshake refused.
      /// </summary>
      private static string Exchange(IPAddress from, int port, bool tls, string request)
      {
         var client = new TcpClient(new IPEndPoint(from, 0));
         try
         {
            client.ReceiveTimeout = 15000;
            client.SendTimeout = 15000;
            client.Connect(from, port);

            Stream stream = client.GetStream();
            if (tls)
            {
               var secure = new SslStream(stream, false, (sender, certificate, chain, errors) => true);
               secure.AuthenticateAsClient("mail.example.test", null, SslProtocols.Tls12 | SslProtocols.Tls13, false);
               stream = secure;
            }

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

      /// <summary>
      ///    A TLS connection kept alive across requests, from the address given to the
      ///    same address: each request's response read by its Content-Length.
      /// </summary>
      private sealed class KeptConnection : IDisposable
      {
         private readonly TcpClient _client;
         private readonly SslStream _stream;

         public KeptConnection(IPAddress from, int port)
         {
            _client = new TcpClient(new IPEndPoint(from, 0));
            _client.ReceiveTimeout = 15000;
            _client.SendTimeout = 15000;
            _client.Connect(from, port);
            _stream = new SslStream(_client.GetStream(), false, (sender, certificate, chain, errors) => true);
            _stream.AuthenticateAsClient("mail.example.test", null, SslProtocols.Tls12 | SslProtocols.Tls13, false);
         }

         public string Send(string request)
         {
            var bytes = Encoding.ASCII.GetBytes(request);
            _stream.Write(bytes, 0, bytes.Length);
            _stream.Flush();

            var head = new StringBuilder();
            var one = new byte[1];
            while (!head.ToString().EndsWith("\r\n\r\n"))
            {
               if (_stream.Read(one, 0, 1) <= 0)
                  return head.ToString();
               head.Append((char) one[0]);
            }

            var length = 0;
            foreach (var line in head.ToString().Split(new[] { "\r\n" }, StringSplitOptions.RemoveEmptyEntries))
            {
               if (line.StartsWith("Content-Length:", StringComparison.OrdinalIgnoreCase))
                  int.TryParse(line.Substring("Content-Length:".Length).Trim(), out length);
            }

            var body = new byte[length];
            var have = 0;
            while (have < length)
            {
               var read = _stream.Read(body, have, length - have);
               if (read <= 0)
                  break;
               have += read;
            }

            return head + Encoding.UTF8.GetString(body, 0, have);
         }

         /// <summary>
         ///    Whether the server keeps the connection: true while nothing arrives on
         ///    it for two seconds, false once it has been closed (a read that ends,
         ///    or is reset). The wait is the socket's, not a read that times out,
         ///    which would leave the TLS stream unusable for the next request.
         /// </summary>
         public bool IsOpen()
         {
            if (!_client.Client.Poll(2000000, SelectMode.SelectRead))
               return true;

            try
            {
               var one = new byte[1];
               return _stream.Read(one, 0, 1) > 0;
            }
            catch (IOException)
            {
               return false;
            }
         }

         public void Dispose()
         {
            _stream.Dispose();
            _client.Close();
         }
      }
   }
}
