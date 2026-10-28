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
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;
using RegressionTests.SSL;

namespace RegressionTests.Security
{
   /// <summary>
   ///    The name of the IP range a run of failed logons is banned with, while a range
   ///    already stands under the name the user name gives.
   ///
   ///    The name is "Auto-ban: " and the user name, made unique with " (1)" and the
   ///    like when a range holds it, in a column of 100 characters with a unique index.
   ///    Up to 6.3.7 the suffix was put after the whole user name and the name then cut
   ///    to 100 after the check for a name in use had asked about it uncut, so that
   ///    with a user name of more than 77 characters a suffix could be cut (from 78 a
   ///    name in use could be chosen again, below 86 by chance, and from 90 every name
   ///    the server could choose was the same once cut): while one ban for that
   ///    user name stood, the unique index refused a later one, HM6099 was reported
   ///    and the address that had earned the ban was not banned - its spent failures
   ///    cleared all the same, so it was given the next set of attempts. The check for
   ///    a name in use compared a literal that SQL Server reads in the database's code
   ///    page, so a user name with characters outside it collided at any length. And a
   ///    user name holding U+0000, which the web services listener's HTTP Basic
   ///    sign-in keeps, was bound on SQL Server and Compact as the part before it -
   ///    suffix and all - and failed the statement on MySQL and PostgreSQL.
   ///
   ///    Each test stands a range of its own under the name a ban for the user name
   ///    takes, at an address of TEST-NET-1 that nothing connects from, then fails
   ///    the threshold's logons with that user name from this machine, and finds this
   ///    machine banned under the next name. Every range the fixture made is deleted
   ///    again when each test ends, and the logon failures are cleared.
   ///
   ///    The logons are POP3's AUTH PLAIN, which carries the user name in UTF-8 as
   ///    written (RFC 4616), and for the user name holding U+0000 - which no mail
   ///    listener passes on - CardDAV's HTTP Basic over HTTPS from this machine's own
   ///    address that is not loopback, since a failed sign-in there from loopback is
   ///    counted against the name alone and bans nothing (on a machine without such an
   ///    address the Linux suite skips that test and the Windows suite fails it); no
   ///    account has these names. Over COM on Windows and
   ///    REST on Linux alike: the Linux suite links this fixture. A build that cannot
   ///    prepare a user name outside ASCII for SASL (the Linux build: HM6450) never
   ///    lets such a name reach the auto-ban, and the second test says so and is
   ///    ignored there; its shape was SQL Server's alone.
   /// </summary>
   [TestFixture]
   public class AutoBanRangeName : TestFixtureBase
   {
      private const string Prefix = "Auto-ban: ";
      private const string StandingAddress = "192.0.2.77";
      private const int Threshold = 3;

      [SetUp]
      public void TurnAutoBanOn()
      {
         _settings.ClearLogonFailureList();
         _settings.AutoBanOnLogonFailure = true;
         _settings.MaxInvalidLogonAttempts = Threshold;
         _settings.MaxInvalidLogonAttemptsWithin = 5;
         _settings.AutoBanMinutes = 3;
      }

      [TearDown]
      public void RemoveTheRangesMade()
      {
         // The standing range and the ban, and any other range named as an auto-ban:
         // TestSetup leaves none before a test.
         var ranges = _settings.SecurityRanges;
         for (var i = ranges.Count - 1; i >= 0; i--)
         {
            var range = ranges[i];
            if (range.Name.StartsWith(Prefix))
               ranges.DeleteByDBID(range.ID);
         }

         _settings.ClearLogonFailureList();
         _settings.AutoBanOnLogonFailure = false;
      }

      [Test]
      [Description("A user name of 120 characters: a range stands under the first 100 characters of its " +
                   "ban's name, and the threshold's failed logons ban this machine under \"Auto-ban: \", " +
                   "86 characters of the user name and \" (1)\".")]
      public void ALongUserNameIsBannedWhileARangeStandsUnderItsName()
      {
         var userName = new string('a', 107) + "@example.test";
         var full = Prefix + userName;

         StandRange(full, full.Substring(0, 100));

         FailLogons(userName);

         AssertBannedAs(Prefix + userName.Substring(0, 86) + " (1)");
      }

      [Test]
      [Description("A user name of Cyrillic and Chinese characters, outside the Latin code pages: a range " +
                   "stands under its ban's name, and the threshold's failed logons ban this machine under " +
                   "the name and \" (1)\".")]
      public void AUserNameOutsideTheCodePageIsBannedWhileARangeStandsUnderItsName()
      {
         // Ivan.Petrov in Cyrillic and two Chinese characters, written with escapes so that the source is ASCII.
         var userName = "\u0418\u0432\u0430\u043D.\u041F\u0435\u0442\u0440\u043E\u0432\u7528\u6237@example.test";
         var full = Prefix + userName;

         StandRange(full, full);

         FailLogons(userName);

         AssertBannedAs(full + " (1)");
      }

      [Test]
      [Description("A user name holding U+0000, over the web services listener's HTTP Basic sign-in, which " +
                   "keeps it: a range stands under the name a ban for the part before the U+0000 takes, and " +
                   "the threshold's failed sign-ins from this machine's own address ban it under that name " +
                   "and \" (1)\", with no error reported.")]
      public void AUserNameHoldingANulIsBannedWhileARangeStandsUnderItsName()
      {
         const string before = "nulname@example.test";

         // From this machine's own address that is not loopback, over HTTPS: a
         // failed sign-in from loopback, a proxy's address, is counted against the
         // name alone and bans nothing (Dav::Authenticate).
         var local = TestSetup.GetLocalIpAddress();
         if (local == null || IPAddress.IsLoopback(local))
            Assert.Ignore("This machine has no address other than loopback that the server answers on, and a failed sign-in from loopback bans nothing.");

         StandRange(Prefix + before, Prefix + before);

         StartWebServices();
         try
         {
            for (var i = 1; i <= Threshold; i++)
            {
               var status = DavOptions(local, before + "\0junk" + i, "not-the-password");
               ClassicAssert.AreEqual(401, status, "Failed sign-in " + i + " was not refused.");
            }

            var names = RangeNames();
            ClassicAssert.Contains(Prefix + before + " (1)", names,
               "No range was written under the next name. The ranges: " + string.Join(" | ", names));

            ClassicAssert.AreEqual("", Greeting(local, TestPorts.Pop3),
               "This machine's address " + local + " was given a greeting: it is not banned.");

            // Nothing reports an error, on any engine: a name that holds a U+0000 is
            // refused before anything is looked up with it (LogonName.h), so the
            // sign-in's look-ups of the account and its domain, which on MySQL and
            // PostgreSQL reported HM5033 for it up to 6.3.7, are never made, and the
            // ban's own statements are given the part before it.
            var errors = File.Exists(LogHandler.GetErrorLogFileName()) ? LogHandler.ReadAndDeleteErrorLog() : "";
            var reported = errors.Split(new[] { '\r', '\n' }, StringSplitOptions.RemoveEmptyEntries);

            ClassicAssert.IsEmpty(reported, "An error was reported: " + string.Join(Environment.NewLine, reported));
         }
         finally
         {
            StopWebServices();
         }
      }

      [Test]
      [Description("Sessions from one address that were open before it was banned - a burst of parallel " +
                   "connections, every one of which counted its failure before the first was banned - each " +
                   "cross the threshold: no second range is written while the auto-ban in force bans the " +
                   "address at least as long as a new ban would, and one is once the ban in force ends sooner.")]
      public void SessionsOpenBeforeABanDoNotBanTheAddressAgainWhileTheBanLasts()
      {
         const string userName = "burst@example.test";

         // Six sessions greeted before the ban, as a burst's are.
         var sessions = new List<Pop3ClientSimulator>();
         for (var i = 0; i < 2 * Threshold; i++)
         {
            var session = new Pop3ClientSimulator();
            string greeting;
            ClassicAssert.IsTrue(session.Connect() && session.ReceiveBanner(out greeting), "Session " + (i + 1) + " was not greeted.");
            sessions.Add(session);
         }

         try
         {
            var standing = StandRange(Prefix + "standing burst ban", Prefix + "standing burst ban", "127.0.0.1");

            for (var i = 0; i < Threshold; i++)
            {
               string reply;
               ClassicAssert.IsFalse(sessions[i].LogonOnOpenSession(userName, "not-the-password", out reply),
                  "Failed logon " + (i + 1) + " was not refused: " + reply);
            }

            var names = RangeNames();
            ClassicAssert.IsFalse(names.Contains(Prefix + userName),
               "A second range was written for an address a ban in force bans for longer: " + string.Join(" | ", names));

            // The control: the ban in force ends before a new one would.
            standing.ExpiresTime = System.DateTime.Now.AddSeconds(50);
            standing.Save();

            for (var i = Threshold; i < 2 * Threshold; i++)
            {
               string reply;
               ClassicAssert.IsFalse(sessions[i].LogonOnOpenSession(userName, "not-the-password", out reply),
                  "Failed logon " + (i + 1) + " was not refused: " + reply);
            }

            names = RangeNames();
            ClassicAssert.Contains(Prefix + userName, names,
               "No range was written for an address whose ban in force ends before a new one would: " + string.Join(" | ", names));

            CustomAsserts.AssertNoReportedError();
         }
         finally
         {
            foreach (var session in sessions)
               session.Disconnect();
         }
      }

      /// <summary>
      ///    The web services listener on every address over HTTPS alone, with the
      ///    suite's example certificate, and back once it answers a TLS handshake
      ///    over loopback.
      /// </summary>
      private void StartWebServices()
      {
         IniFileSetting.Write("WebServicesBindAddress", "0.0.0.0");
         IniFileSetting.Write("WebServicesHttpPort", "0");
         IniFileSetting.Write("WebServicesHttpsPort", WebServicesPort.ToString());
         IniFileSetting.Write("WebServicesCertificateFile", Paths.Combine(SslSetup.GetSslCertPath(), "example.crt"));
         IniFileSetting.Write("WebServicesPrivateKeyFile", Paths.Combine(SslSetup.GetSslCertPath(), "example.key"));
         IniFileSetting.Write("WebServicesTrustedProxies", "");
         _application.Reinitialize();

         // An empty credential, which is refused before any logon is asked.
         var deadline = DateTime.UtcNow.AddSeconds(20);
         while (DavOptions(IPAddress.Loopback, "", "") == -1 && DateTime.UtcNow < deadline)
            System.Threading.Thread.Sleep(200);
      }

      private void StopWebServices()
      {
         IniFileSetting.Write("WebServicesHttpsPort", "0");
         IniFileSetting.Write("WebServicesCertificateFile", "");
         IniFileSetting.Write("WebServicesPrivateKeyFile", "");
         IniFileSetting.Write("WebServicesBindAddress", "127.0.0.1");
         _application.Reinitialize();
      }

      private const int WebServicesPort = 9177;

      /// <summary>
      ///    OPTIONS /dav/ with HTTP Basic over TLS, from the address given to the same
      ///    address, the user name and password in UTF-8 as given; the status the
      ///    listener answers, -1 when it answers none.
      /// </summary>
      private static int DavOptions(IPAddress from, string user, string password)
      {
         var authorization = "Basic " + Convert.ToBase64String(Encoding.UTF8.GetBytes(user + ":" + password));
         using (var client = new TcpClient(new IPEndPoint(from, 0)))
         {
            try
            {
               client.ReceiveTimeout = 15000;
               client.SendTimeout = 15000;
               client.Connect(from, WebServicesPort);
               using (var stream = new SslStream(client.GetStream(), false, (sender, certificate, chain, errors) => true))
               using (var memory = new MemoryStream())
               {
                  stream.AuthenticateAsClient("mail.example.test", null, SslProtocols.Tls12 | SslProtocols.Tls13, false);

                  var head = "OPTIONS /dav/ HTTP/1.0\r\nHost: mail.example.test\r\nAuthorization: " + authorization +
                             "\r\nConnection: close\r\n\r\n";
                  var bytes = Encoding.ASCII.GetBytes(head);
                  stream.Write(bytes, 0, bytes.Length);
                  var buffer = new byte[8192];
                  int read;
                  try
                  {
                     while ((read = stream.Read(buffer, 0, buffer.Length)) > 0)
                        memory.Write(buffer, 0, read);
                  }
                  catch (IOException)
                  {
                  }

                  var raw = Encoding.UTF8.GetString(memory.ToArray());
                  int status;
                  if (!raw.StartsWith("HTTP/") || raw.IndexOf(' ') < 0 || raw.Length < raw.IndexOf(' ') + 4 ||
                      !int.TryParse(raw.Substring(raw.IndexOf(' ') + 1, 3), out status))
                     return -1;
                  return status;
               }
            }
            catch (IOException)
            {
               return -1;
            }
            catch (SocketException)
            {
               return -1;
            }
            catch (AuthenticationException)
            {
               return -1;
            }
         }
      }

      /// <summary>
      ///    The first line a listener sends to a new connection from the address
      ///    given to the same address; "" when it closes or resets without one.
      /// </summary>
      private static string Greeting(IPAddress from, int port)
      {
         using (var client = new TcpClient(new IPEndPoint(from, 0)))
         {
            try
            {
               client.ReceiveTimeout = 15000;
               client.Connect(from, port);

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
         }
      }

      /// <summary>
      ///    The range a ban for the user name stood under, read back as the server
      ///    stored it - what the test stands on, checked before anything else.
      /// </summary>
      private hMailServer.SecurityRange StandRange(string name, string expectedStoredName, string address = StandingAddress)
      {
         var range = _settings.SecurityRanges.Add();
         range.Name = name;
         range.LowerIP = address;
         range.UpperIP = address;
         range.Priority = 100;

         // Refusing every connection, as an auto-ban's range does.
         range.AllowSMTPConnections = false;
         range.AllowPOP3Connections = false;
         range.AllowIMAPConnections = false;

         range.Expires = true;
         range.ExpiresTime = System.DateTime.Now.AddMinutes(30);
         range.Save();

         ClassicAssert.Contains(expectedStoredName, RangeNames(),
            "The range the test stands on is not stored under the name a ban for the user name takes.");

         return range;
      }

      private void FailLogons(string userName)
      {
         for (var i = 1; i <= Threshold; i++)
         {
            var simulator = new Pop3ClientSimulator();
            string reply;
            ClassicAssert.IsFalse(simulator.ConnectAndLogonPlain(userName, "not-the-password", out reply),
               "Failed logon " + i + " was not refused: " + reply);
            simulator.Disconnect();

            // A build without Unicode normalisation refuses a credential outside ASCII
            // before the password is looked at (SASLprep, HM6450): such a name never
            // reaches the auto-ban there, which is what the Linux build does today, and
            // on its engines the check for a name in use was never a code page's.
            if (i == 1 && reply.Contains("Invalid SASL PLAIN response"))
            {
               CustomAsserts.AssertReportedError("HM6450");
               Assert.Ignore("This build cannot prepare a user name outside ASCII for SASL (HM6450), so no such name reaches the auto-ban: " + reply.Trim());
            }
         }
      }

      private void AssertBannedAs(string expectedName)
      {
         var names = RangeNames();

         ClassicAssert.Contains(expectedName, names,
            "No range was written under the next name. The ranges: " + string.Join(" | ", names));

         ClassicAssert.AreEqual("", new Pop3ClientSimulator().GetWelcomeMessage(),
            "This machine was given a greeting: it is not banned.");

         CustomAsserts.AssertNoReportedError();
      }

      private List<string> RangeNames()
      {
         var names = new List<string>();
         var ranges = _settings.SecurityRanges;
         for (var i = 0; i < ranges.Count; i++)
            names.Add(ranges[i].Name);
         return names;
      }
   }
}
