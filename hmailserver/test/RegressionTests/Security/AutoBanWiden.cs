// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net.Sockets;
using System.Text;
using System.Threading;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.Security
{
   /// <summary>
   ///    An auto-ban widened to its reverse-name neighbours (GitLab work item #55,
   ///    AutoBanWidenByReverseName): the ban is written for the one address at once,
   ///    and then, in the background, its row becomes the contiguous run of the /24
   ///    around it whose reverse names share its registrable domain.
   ///
   ///    What these pin, each beside the case that must come out the other way:
   ///
   ///      * a ban is widened to exactly the run - not across the foreign host
   ///        below it, not across the hole above it - one row, said in the
   ///        application log, and a neighbour in the run is then refused by the
   ///        server itself while the addresses just outside it are not;
   ///      * with the switch off, the same ban stays the one address;
   ///      * a ban whose own reverse name does not resolve back to it is not
   ///        widened, so a scanner cannot name itself after its neighbours;
   ///      * the ban of a network's own address (10.66.20.20, RFC 1918) is not
   ///        widened, though its zone has the shape that widens an Internet one;
   ///      * the run stops before an address on AutoBanNeverBan, before one an
   ///        administrator's IP range tells apart from the banned address, and
   ///        before one whose reverse name the never-ban list by reverse name covers;
   ///      * a neighbour's reverse lookup that is not answered leaves the single
   ///        address;
   ///      * the setting is read and written over REST, and the OpenAPI document
   ///        the Deck builds its form from carries it.
   ///
   ///    Every session is moved by XCLIENT, from a trusted 127.0.0.1, to an address
   ///    of 198.18.0.0/15 (RFC 2544, never routed) - or, for the network's own
   ///    address, of 10.66.0.0/16 - and the suite's own DNS zone
   ///    answers the reverse and forward names. Each test uses a /24 of its own,
   ///    because the Windows resolver keeps an answer - including "no such record" -
   ///    for its time to live, and a neighbour one test named would otherwise be
   ///    what the next test's walk is told. A spam trap hit is the ban: it needs no
   ///    account, and it is the writer that is placed inside RCPT TO, where the
   ///    widening must cost the reply nothing.
   ///
   ///    Everything is made and taken back over REST and raw sockets, with no COM,
   ///    which is why the Linux suite links this fixture. The COM property, the
   ///    Windows Defender Firewall rule for a run and the hook told each address are
   ///    in AutoBanWidenCom, which only the Windows suite runs.
   /// </summary>
   [TestFixture]
   public class AutoBanWiden : TestFixtureBase
   {
      private static int RestPort = 9644;
      private const string AdminPassword = "testar";

      private const string Domain = "widen-scan.test";
      private const string TrapLocalPart = "never-was-real-widen";
      private const string Sender = "harvester@widen-spammer.example";
      private const string RangeName = "widen test net";
      private const string OfficeRangeName = "widen test office";
      private const string TrustedRangeName = "widen test trusted";
      private const string LanRangeName = "widen test lan";
      private const string NotePrefix = "widen-fixture";

      private long banMinutesBefore_;
      private bool widenBefore_;

      [SetUp]
      public void StartRestApiAndXclient()
      {
         _settings.SetAdministratorPassword(AdminPassword);

         RestPort = RestListener.Start(RestPort);

         IniFileSetting.Write("SMTPXClientEnabled", "1");
         IniFileSetting.Write("SMTPXClientTrustedIPs", "127.0.0.1");

         _application.Reinitialize();

         (int status, string body) probe = Http("GET", "/api/v1/status");
         ClassicAssert.AreEqual(200, probe.status, "REST API did not answer /api/v1/status. Body: " + probe.body);

         Dictionary<string, object> settings = Settings();
         banMinutesBefore_ = MiniJson.Number(settings, "spam_trap_ban_minutes");
         widenBefore_ = MiniJson.Flag(settings, "auto_ban_widen_by_reverse_name");

         PutSettings("{\"spam_trap_ban_minutes\":5,\"auto_ban_widen_by_reverse_name\":false}");

         TakeBackOurs();

         // The fixture's nets: SMTP and delivery to this server's domains allowed,
         // the spam tests run - so a trap hit from them is one that may be banned.
         // It covers the banned address and its neighbours alike, so it tells no
         // neighbour apart and stops no walk.
         MakeRange(RangeName, "198.18.0.0", "198.19.255.255", 20);

         DeclareTrap();
      }

      [TearDown]
      public void TakeItBack()
      {
         try
         {
            TakeBackOurs();

            PutSettings("{\"spam_trap_ban_minutes\":" + banMinutesBefore_ +
                        ",\"auto_ban_widen_by_reverse_name\":" + (widenBefore_ ? "true" : "false") + "}");
         }
         finally
         {
            SuiteDns.Reset();
            IniFileSetting.Delete("SMTPXClientEnabled");
            IniFileSetting.Delete("SMTPXClientTrustedIPs");
            RestListener.Stop();
            _application.Reinitialize();
         }
      }

      // ------------------------------------------------------------------ the run

      [Test]
      [Description("A ban is widened to the contiguous run of its /24 whose reverse names share its registrable domain - not across the foreign host below nor the hole above - in one row, said in the application log; a neighbour in the run is refused, the addresses just outside it are not.")]
      public void ABanIsWidenedToTheRunWhoseReverseNamesShareItsDomain()
      {
         const string net = "198.18.55.";
         Widen(true);
         Scanners(net, 17, 23, "mx20.out." + Domain);
         Reverse(net, 16, "mail16.other-isp.test");

         TrapHitFrom(net + "20");

         Dictionary<string, object> range = RunWithin(net + "20", net + "17", net + "23", TimeSpan.FromSeconds(30));
         ClassicAssert.IsNotNull(range, "The ban of " + net + "20 was not widened to " + net + "17-" + net + "23. Its ranges: " + Describe(net));
         StringAssert.StartsWith("Auto-ban: spam trap ", MiniJson.Text(range, "name"), "The widened row is not the ban's own.");
         ClassicAssert.AreEqual(1, AutoBansIn(net).Count, "The run was written as more than one row: " + Describe(net));

         ClassicAssert.IsTrue(LogWithin("Auto-ban: " + net + "20 was widened to " + net + "17-" + net + "23 via " + Domain, TimeSpan.FromSeconds(10)),
            "The application log does not say the ban was widened, before and after.");

         // The run is refused by the server itself; the addresses beside it are not.
         StringAssert.StartsWith("550", XclientReply(net + "18"), "A neighbour inside the widened run was not refused.");
         StringAssert.StartsWith("220", XclientReply(net + "16"), "The foreign host below the run was refused - the run crossed it.");
         StringAssert.StartsWith("220", XclientReply(net + "24"), "The address above the run, with no reverse name, was refused - the run crossed the hole.");
      }

      [Test]
      [Description("The negative control: with AutoBanWidenByReverseName off, the same ban stays the one address and its neighbour is not refused.")]
      public void WithTheSwitchOffABanStaysASingleAddress()
      {
         const string net = "198.18.56.";
         Widen(false);
         Scanners(net, 17, 23, "mx20.off." + Domain);

         TrapHitFrom(net + "20");

         ClassicAssert.IsTrue(StaysSingle(net + "20", TimeSpan.FromSeconds(6)),
            "With the switch off the ban was widened, or not written at all: " + Describe(net));
         StringAssert.StartsWith("220", XclientReply(net + "18"), "With the switch off a neighbour was refused.");
      }

      [Test]
      [Description("A ban whose own reverse name, under the domain its neighbours share, does not resolve back to it is not widened: anybody who holds an address can write its reverse name.")]
      public void ABanWhoseOwnReverseNameDoesNotResolveBackIsNotWidened()
      {
         const string net = "198.18.57.";
         Widen(true);
         Scanners(net, 17, 23, "mx20.forged." + Domain, "203.0.113.20");

         TrapHitFrom(net + "20");

         ClassicAssert.IsTrue(LogWithin("Auto-ban: " + net + "20 stays a single address: its reverse name mx20.forged." + Domain + " does not resolve back to it", TimeSpan.FromSeconds(30)),
            "The application log does not say why a ban whose reverse name does not resolve back was left alone.");
         ClassicAssert.IsTrue(StaysSingle(net + "20", TimeSpan.FromSeconds(1)), "A ban whose reverse name does not resolve back to it was widened: " + Describe(net));
      }

      [Test]
      [Description("The ban of a network's own address - here 10.66.20.20, RFC 1918 - is never widened, though its neighbours are named under one domain and its own name resolves back, as an office's reverse zone names its machines; ABanIsWidenedToTheRunWhoseReverseNamesShareItsDomain is the same zone on an Internet address, widened.")]
      public void ABanOfANetworksOwnAddressIsNotWidened()
      {
         const string net = "10.66.20.";
         Widen(true);
         MakeRange(LanRangeName, "10.66.0.0", "10.66.255.255", 20);
         Scanners(net, 17, 23, "pc20.office." + Domain);

         TrapHitFrom(net + "20");

         ClassicAssert.IsTrue(LogWithin("Auto-ban: " + net + "20 stays a single address: it is a private, shared, loopback or link-local address", TimeSpan.FromSeconds(30)),
            "The application log does not say why the ban of a network's own address was left alone.");
         ClassicAssert.IsTrue(StaysSingle(net + "20", TimeSpan.FromSeconds(6)), "The ban of a network's own address was widened, or not written at all: " + Describe(net));
         StringAssert.StartsWith("220", XclientReply(net + "18"), "A neighbour of a network's own banned address was refused.");
      }

      // ---------------------------------------------------------------- the stops

      [Test]
      [Description("The run stops before an address on AutoBanNeverBan and before one an administrator's IP range tells apart from the banned address, though both are named under the same domain.")]
      public void TheRunStopsAtAutoBanNeverBanAndAtAnAdministratorsRange()
      {
         const string net = "198.18.58.";
         Widen(true);
         Scanners(net, 12, 28, "mx20.stops." + Domain);

         IniFileSetting.Write("AutoBanNeverBan", net + "23");

         try
         {
            MakeRange(OfficeRangeName, net + "10", net + "17", 25);

            TrapHitFrom(net + "20");

            ClassicAssert.IsNotNull(RunWithin(net + "20", net + "18", net + "22", TimeSpan.FromSeconds(30)),
               "The run was not " + net + "18-" + net + "22, stopping above the range that covers .17 and below the AutoBanNeverBan address .23: " + Describe(net));
            ClassicAssert.IsTrue(LogWithin(net + "17 is told apart from it by the IP range '" + OfficeRangeName + "'", TimeSpan.FromSeconds(10)) &&
                                 LogWithin(net + "23 is on AutoBanNeverBan", TimeSpan.FromSeconds(1)),
               "The application log does not say which barrier stopped each direction.");
         }
         finally
         {
            IniFileSetting.Delete("AutoBanNeverBan");
         }
      }

      [Test]
      [Description("A neighbour an administrator's IP range lets in at a priority not below the ban's is not taken into the run: the server's own refusal would not reach it, and the run's firewall rule must not either.")]
      public void ANeighbourAnAdministratorLetsInIsNotTaken()
      {
         const string net = "198.18.64.";
         Widen(true);
         Scanners(net, 17, 23, "mx20.trusted." + Domain);
         MakeRange(TrustedRangeName, net + "0", net + "255", 150);

         TrapHitFrom(net + "20");

         ClassicAssert.IsTrue(LogWithin("Auto-ban: " + net + "20 stays a single address", TimeSpan.FromSeconds(30)) &&
                              LogWithin(net + "19 is let in by the IP range '" + TrustedRangeName + "'", TimeSpan.FromSeconds(1)),
            "The application log does not say the neighbours an administrator lets in kept the ban a single address.");
         ClassicAssert.IsTrue(StaysSingle(net + "20", TimeSpan.FromSeconds(1)), "Neighbours an administrator lets in were taken into the run: " + Describe(net));
      }

      [Test]
      [Description("The run stops before a neighbour whose reverse name the never-ban list by reverse name covers.")]
      public void TheRunStopsAtANeighbourTheNeverBanListCovers()
      {
         const string net = "198.18.59.";
         Widen(true);
         Scanners(net, 17, 23, "mx20.listed." + Domain);
         Reverse(net, 22, "h22.keep." + Domain);
         AddNeverBanName(".keep." + Domain);

         TrapHitFrom(net + "20");

         ClassicAssert.IsNotNull(RunWithin(net + "20", net + "17", net + "21", TimeSpan.FromSeconds(30)),
            "The run crossed, or did not reach, the neighbour the never-ban list covers: " + Describe(net));
      }

      [Test]
      [Description("A neighbour's reverse lookup that is not answered leaves the single address, though the other side of the run was seen.")]
      public void AnUnansweredNeighbourLeavesTheSingleAddress()
      {
         const string net = "198.18.60.";
         Widen(true);

         // .17-.21 and .23 named under the domain, and .22 not named at all: the
         // zone answers a name it holds a record for whatever WithServFail says,
         // so a .22 named with the others would answer and join the run.
         Scanners(net, 17, 21, "mx20.silent." + Domain);
         Reverse(net, 23, "h23." + Domain);
         SuiteDns.Zone.WithServFail(ReverseName(net, 22));

         TrapHitFrom(net + "20");

         ClassicAssert.IsTrue(LogWithin("Auto-ban: " + net + "20 stays a single address: the reverse lookup of " + net + "22 was not answered", TimeSpan.FromSeconds(60)),
            "The application log does not say the walk was given up for a lookup that was not answered.");
         ClassicAssert.IsTrue(StaysSingle(net + "20", TimeSpan.FromSeconds(1)), "A walk with a lookup that was not answered widened the ban: " + Describe(net));
      }

      // ---------------------------------------------------------------- the setting

      [Test]
      [Description("auto_ban_widen_by_reverse_name is read and written over REST, refused when it is not a boolean, and the OpenAPI document the Deck builds its form from carries it.")]
      public void TheSettingIsReadAndWrittenOverRest()
      {
         PutSettings("{\"auto_ban_widen_by_reverse_name\":true}");
         ClassicAssert.IsTrue(MiniJson.Flag(Settings(), "auto_ban_widen_by_reverse_name"), "The setting did not read back on.");

         PutSettings("{\"auto_ban_widen_by_reverse_name\":false}");
         ClassicAssert.IsFalse(MiniJson.Flag(Settings(), "auto_ban_widen_by_reverse_name"), "The setting did not read back off.");

         (int status, string body) refused = Http("PUT", "/api/v1/settings", "{\"auto_ban_widen_by_reverse_name\":\"yes\"}");
         ClassicAssert.AreEqual(400, refused.status, refused.body);
         StringAssert.Contains("must be true or false", refused.body, refused.body);
         ClassicAssert.IsFalse(MiniJson.Flag(Settings(), "auto_ban_widen_by_reverse_name"), "A refused value was stored.");

         (int status, string body) document = Http("GET", "/api/v1/openapi.json");
         ClassicAssert.AreEqual(200, document.status, document.body);
         StringAssert.Contains("auto_ban_widen_by_reverse_name", document.body, "The OpenAPI document does not carry the setting.");
      }

      // ------------------------------------------------------------------- helpers

      private static void Widen(bool on)
      {
         PutSettings("{\"auto_ban_widen_by_reverse_name\":" + (on ? "true" : "false") + "}");
      }

      private static string ReverseName(string net, int lastOctet)
      {
         string[] octets = (net + lastOctet).Split('.');
         return octets[3] + "." + octets[2] + "." + octets[1] + "." + octets[0] + ".in-addr.arpa";
      }

      /// <summary>A PTR for net+lastOctet naming host.</summary>
      private static void Reverse(string net, int lastOctet, string host)
      {
         SuiteDns.Zone.WithRaw(ReverseName(net, lastOctet), 12, DnsWire.Name(host));
      }

      /// <summary>
      ///    A scanner's run: first..last named h&lt;n&gt;.widen-scan.test, except the
      ///    banned address .20, named seedName, whose A record is forwardTo (by
      ///    default the address itself).
      /// </summary>
      private static void Scanners(string net, int first, int last, string seedName, string forwardTo = null)
      {
         for (int octet = first; octet <= last; octet++)
         {
            if (octet == 20)
               continue;

            Reverse(net, octet, "h" + octet + "." + Domain);
         }

         Reverse(net, 20, seedName);
         SuiteDns.Zone.WithA(seedName, forwardTo ?? net + "20");
      }

      private static void AddNeverBanName(string pattern)
      {
         (int status, string body) created = Http("POST", "/api/v1/never-ban-names", "{\"pattern\":\"" + pattern + "\",\"note\":\"" + NotePrefix + "\"}");
         ClassicAssert.AreEqual(201, created.status, created.body);
      }

      private static void MakeRange(string name, string lower, string upper, int priority)
      {
         (int status, string body) range = Http("POST", "/api/v1/ipranges",
            "{\"name\":\"" + name + "\",\"lower\":\"" + lower + "\",\"upper\":\"" + upper + "\",\"priority\":" + priority + "," +
            "\"allow_smtp\":true,\"allow_imap\":false,\"allow_pop3\":false,\"deliver_remote_to_local\":true," +
            "\"require_auth_remote_to_local\":false,\"require_tls_for_auth\":false,\"spam_protection\":true,\"virus_protection\":false}");
         ClassicAssert.AreEqual(201, range.status, range.body);
      }

      private void DeclareTrap()
      {
         (int status, string body) created = Http("POST", "/api/v1/domains/" + _domain.Name + "/spam-traps", "{\"address\":\"" + TrapLocalPart + "\"}");
         ClassicAssert.IsTrue(created.status == 201 || created.status == 200, created.body);
      }

      /// <summary>One SMTP session moved to the address by XCLIENT, addressing the trap.</summary>
      private void TrapHitFrom(string address)
      {
         using (var socket = new TcpConnection())
         {
            ClassicAssert.IsTrue(socket.Connect(25), "Could not connect to SMTP.");
            socket.ReadUntil("220");

            socket.Send("EHLO upstream.widen.test\r\n");
            StringAssert.Contains("XCLIENT", socket.ReadUntil("250 "), "XCLIENT is not offered to 127.0.0.1.");

            socket.Send("XCLIENT ADDR=" + address + "\r\n");
            StringAssert.StartsWith("220", socket.ReadUntil("\r\n"), "XCLIENT to " + address + " was refused before the trap was hit.");

            socket.Send("EHLO client.widen.test\r\n");
            socket.ReadUntil("250 ");

            socket.Send("MAIL FROM:<" + Sender + ">\r\n");
            socket.ReadUntil("250");

            socket.Send("RCPT TO:<" + TrapLocalPart + "@" + _domain.Name + ">\r\n");
            StringAssert.StartsWith("550", socket.ReadUntil("\r\n"), "The trap was not refused.");

            socket.Send("QUIT\r\n");
         }
      }

      /// <summary>What XCLIENT to the address answers: 550 when the IP ranges refuse it.</summary>
      private static string XclientReply(string address)
      {
         using (var socket = new TcpConnection())
         {
            ClassicAssert.IsTrue(socket.Connect(25), "Could not connect to SMTP.");
            socket.ReadUntil("220");

            socket.Send("EHLO upstream.widen.test\r\n");
            socket.ReadUntil("250 ");

            socket.Send("XCLIENT ADDR=" + address + "\r\n");
            string reply = socket.ReadUntil("\r\n");

            try
            {
               socket.Send("QUIT\r\n");
            }
            catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
            {
               // A refused XCLIENT closes the session.
            }

            return reply;
         }
      }

      /// <summary>The auto-ban ranges whose lower address is in the /24.</summary>
      private static List<Dictionary<string, object>> AutoBansIn(string net)
      {
         return Ranges().Where(r => MiniJson.Text(r, "name").StartsWith("Auto-ban: ", StringComparison.Ordinal) &&
                                    MiniJson.Text(r, "lower").StartsWith(net, StringComparison.Ordinal)).ToList();
      }

      private static string Describe(string net)
      {
         return string.Join("; ", AutoBansIn(net).Select(r => MiniJson.Text(r, "lower") + "-" + MiniJson.Text(r, "upper")));
      }

      /// <summary>The ban's row once it reads lower..upper, polled until the deadline; null if it never does.</summary>
      private static Dictionary<string, object> RunWithin(string address, string lower, string upper, TimeSpan deadline)
      {
         DateTime until = DateTime.UtcNow + deadline;
         string net = address.Substring(0, address.LastIndexOf('.') + 1);

         while (true)
         {
            Dictionary<string, object> run = AutoBansIn(net).FirstOrDefault(r => MiniJson.Text(r, "lower") == lower && MiniJson.Text(r, "upper") == upper);
            if (run != null)
               return run;

            if (DateTime.UtcNow > until)
               return null;

            Thread.Sleep(250);
         }
      }

      /// <summary>True when the address is banned as itself alone now, and still is after the wait.</summary>
      private static bool StaysSingle(string address, TimeSpan wait)
      {
         string net = address.Substring(0, address.LastIndexOf('.') + 1);

         Func<bool> single = () =>
         {
            List<Dictionary<string, object>> bans = AutoBansIn(net);
            return bans.Count == 1 && MiniJson.Text(bans[0], "lower") == address && MiniJson.Text(bans[0], "upper") == address;
         };

         if (!single())
            return false;

         Thread.Sleep(wait);
         return single();
      }

      /// <summary>True once the server's default log holds the text, polled until the deadline.</summary>
      private static bool LogWithin(string text, TimeSpan deadline)
      {
         DateTime until = DateTime.UtcNow + deadline;

         while (true)
         {
            if (LogHandler.ReadCurrentDefaultLog().Contains(text))
               return true;

            if (DateTime.UtcNow > until)
               return false;

            Thread.Sleep(500);
         }
      }

      private static List<Dictionary<string, object>> Ranges()
      {
         (int status, string body) = Http("GET", "/api/v1/ipranges");
         ClassicAssert.AreEqual(200, status, body);

         object parsed = MiniJson.Parse(body);
         var list = parsed as List<object> ?? MiniJson.List((Dictionary<string, object>) parsed, "ranges");
         return list.Cast<Dictionary<string, object>>().ToList();
      }

      /// <summary>
      ///    Everything this fixture made, by what it is called: the bans on its nets,
      ///    its two ranges, its trap and the record of its hits, and its rows of the
      ///    never-ban list.
      /// </summary>
      private void TakeBackOurs()
      {
         foreach (Dictionary<string, object> range in Ranges())
         {
            string name = MiniJson.Text(range, "name");
            string lower = MiniJson.Text(range, "lower");
            bool ours = name == RangeName || name == OfficeRangeName || name == TrustedRangeName || name == LanRangeName ||
                        (name.StartsWith("Auto-ban: ", StringComparison.Ordinal) && (lower.StartsWith("198.18.", StringComparison.Ordinal) || lower.StartsWith("198.19.", StringComparison.Ordinal) ||
                                                                                      lower.StartsWith("10.66.", StringComparison.Ordinal)));

            if (ours)
               Http("DELETE", "/api/v1/ipranges/" + MiniJson.Number(range, "id"));
         }

         Http("DELETE", "/api/v1/spam-traps/" + TrapLocalPart + "@" + _domain.Name);
         Http("DELETE", "/api/v1/spam-traps/hits");

         foreach (Dictionary<string, object> row in ((List<object>) MiniJson.Parse(Http("GET", "/api/v1/never-ban-names").body)).Cast<Dictionary<string, object>>())
         {
            if (MiniJson.Text(row, "note").StartsWith(NotePrefix, StringComparison.Ordinal))
               Http("DELETE", "/api/v1/never-ban-names/" + MiniJson.Number(row, "id"));
         }
      }

      private static Dictionary<string, object> Settings()
      {
         (int status, string body) = Http("GET", "/api/v1/settings");
         ClassicAssert.AreEqual(200, status, body);
         return MiniJson.Object(body);
      }

      private static void PutSettings(string body)
      {
         (int status, string answer) = Http("PUT", "/api/v1/settings", body);
         ClassicAssert.AreEqual(200, status, answer);
      }

      private static (int status, string body) Http(string method, string path, string requestBody = null)
      {
         string credentials = Convert.ToBase64String(Encoding.ASCII.GetBytes("Administrator:" + AdminPassword));

         using (var client = new TcpClient())
         {
            Exception last = null;

            for (int attempt = 0; attempt < 25; attempt++)
            {
               try
               {
                  client.Connect(TestPorts.HostAddress, RestPort);
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

            using (NetworkStream stream = client.GetStream())
            using (var memory = new MemoryStream())
            {
               byte[] bodyBytes = requestBody == null ? new byte[0] : Encoding.UTF8.GetBytes(requestBody);

               var head = new StringBuilder();
               head.Append(method + " " + path + " HTTP/1.0\r\n");
               head.Append("Host: 127.0.0.1\r\n");
               head.Append("Authorization: Basic " + credentials + "\r\n");

               if (requestBody != null)
               {
                  head.Append("Content-Type: application/json\r\n");
                  head.Append("Content-Length: " + bodyBytes.Length + "\r\n");
               }

               head.Append("Connection: close\r\n\r\n");

               byte[] headBytes = Encoding.ASCII.GetBytes(head.ToString());
               stream.Write(headBytes, 0, headBytes.Length);

               if (bodyBytes.Length > 0)
                  stream.Write(bodyBytes, 0, bodyBytes.Length);

               byte[] buffer = new byte[8192];
               int read;

               while ((read = stream.Read(buffer, 0, buffer.Length)) > 0)
                  memory.Write(buffer, 0, read);

               string raw = Encoding.UTF8.GetString(memory.ToArray());
               string[] first = raw.Split(new[] { "\r\n" }, StringSplitOptions.None)[0].Split(' ');
               int status = 0;

               if (first.Length >= 2)
                  int.TryParse(first[1], out status);

               int separator = raw.IndexOf("\r\n\r\n", StringComparison.Ordinal);
               return (status, separator >= 0 ? raw.Substring(separator + 4) : "");
            }
         }
      }
   }
}
