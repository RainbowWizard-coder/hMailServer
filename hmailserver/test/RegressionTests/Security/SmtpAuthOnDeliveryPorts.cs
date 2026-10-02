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
   ///    AUTH on a delivery port (GitLab work item #54): an SMTP listener's
   ///    authentication is On, Off or Honeypot. Off does not offer AUTH, Honeypot
   ///    offers it exactly as On does, and on both an AUTH is answered 502 5.5.1
   ///    without the account being looked up or the password evaluated, and the
   ///    first one of a session bans the address - one range per address while it
   ///    lasts - unless the address is this computer's, on AutoBanNeverBan, or the
   ///    ban minutes are 0.
   ///
   ///    What these pin, each beside the case that comes out the other way: the
   ///    honeypot's EHLO is the On listener's; the right password is refused on a
   ///    honeypot and taken on the On listener; Off offers no AUTH where On does;
   ///    the session goes on as inbound delivery after the refusal; a ban is in
   ///    force (a new session from the address is refused at XCLIENT) where a
   ///    never-banned or loopback address is not banned; the line the fail2ban
   ///    filter matches names the port, the mechanism and the listener and never
   ///    the argument; two sessions and several AUTH lines are one range; and the
   ///    port route carries the mode and refuses it on a port that is not SMTP.
   ///
   ///    The suite reaches the server from 127.0.0.1, which is never banned here. So
   ///    every session that is to be banned is moved to an address of
   ///    198.51.100.0/24 (RFC 5737) with XCLIENT, from a trusted 127.0.0.1, and an
   ///    IP range for that /24 is made that lets SMTP deliver to this server's
   ///    domains without TLS for authentication. The two listeners - a honeypot
   ///    and an Off one - are made over REST on ports of their own and bound by a
   ///    reinitialise, so that port 25, which the rest of the suite signs in on,
   ///    stays On. Everything is REST and raw sockets, with no COM, which is why the
   ///    Linux suite links this fixture.
   /// </summary>
   [TestFixture]
   public class SmtpAuthOnDeliveryPorts : TestFixtureBase
   {
      private static int RestPort = 9649;
      private const string AdminPassword = "testar";

      private const int HoneypotPort = 2554;
      private const int OffPort = 2556;
      private const int ImapProbePort = 2558;

      private const string Net = "198.51.100.";
      private const string NeverBanned = "198.51.100.45";
      private const string RangeName = "AUTH on delivery ports test net";
      private const string AccountPassword = "Delivery-Port-Secret-9";
      private const int BanMinutes = 5;

      private long minutesToBanBefore_;
      private string accountAddress_;

      [SetUp]
      public void ListenersRangeAndXclient()
      {
         _settings.SetAdministratorPassword(AdminPassword);

         RestPort = RestListener.Start(RestPort);

         IniFileSetting.Write("SMTPXClientEnabled", "1");
         IniFileSetting.Write("SMTPXClientTrustedIPs", "127.0.0.1");
         IniFileSetting.Write("AutoBanNeverBan", NeverBanned);

         _application.Reinitialize();

         (int status, string body) probe = Http("GET", "/api/v1/status");
         ClassicAssert.AreEqual(200, probe.status, "REST API did not answer /api/v1/status. Body: " + probe.body);

         minutesToBanBefore_ = MiniJson.Number(Settings(), "minutes_to_ban");
         PutSettings("{\"minutes_to_ban\":" + BanMinutes + "}");

         TakeBackOurs();

         accountAddress_ = "delivery-port@" + _domain.Name;
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, accountAddress_, AccountPassword);

         (int status, string body) range = Http("POST", "/api/v1/ipranges",
            "{\"name\":\"" + RangeName + "\",\"lower\":\"" + Net + "0\",\"upper\":\"" + Net + "255\",\"priority\":20," +
            "\"allow_smtp\":true,\"allow_imap\":false,\"allow_pop3\":false,\"deliver_remote_to_local\":true," +
            "\"require_auth_remote_to_local\":false,\"require_tls_for_auth\":false,\"spam_protection\":false,\"virus_protection\":false}");
         ClassicAssert.AreEqual(201, range.status, range.body);

         CreatePort(HoneypotPort, "honeypot");
         CreatePort(OffPort, "off");

         // The listeners are built when the services start.
         _application.Reinitialize();
         WaitForListener(HoneypotPort);
         WaitForListener(OffPort);
      }

      [TearDown]
      public void TakeItBack()
      {
         try
         {
            TakeBackOurs();
            PutSettings("{\"minutes_to_ban\":" + minutesToBanBefore_ + "}");
         }
         finally
         {
            IniFileSetting.Delete("SMTPXClientEnabled");
            IniFileSetting.Delete("SMTPXClientTrustedIPs");
            IniFileSetting.Delete("AutoBanNeverBan");
            RestListener.Stop();
            _application.Reinitialize();
         }
      }

      // ------------------------------------------------------------------ honeypot

      [Test]
      [Description("A honeypot offers AUTH exactly as the On listener does; the account's own password is refused there with 502 5.5.1, the session goes on as inbound delivery, the address is banned for the ban minutes under one range, and the line the fail2ban filter matches names the port, the mechanism and the listener and never the argument. Negative control: the same password on the On listener, from another address, is taken and bans nothing.")]
      public void AHoneypotOffersAuthAsOnDoesAndBansTheFirstAuth()
      {
         string address = Net + "31";
         string credential = Plain(accountAddress_, AccountPassword);

         // LOGIN, since PLAIN is offered only where AllowSMTPAuthPlain is on, and the
         // suite leaves it off; the honeypot refuses either without looking.
         string onEhlo;
         using (TcpConnection on = Open(25, Net + "32", out onEhlo))
         {
            on.Send("AUTH LOGIN\r\n");
            StringAssert.StartsWith("334", on.ReadUntil("\r\n"));
            on.Send(Convert.ToBase64String(Encoding.UTF8.GetBytes(accountAddress_)) + "\r\n");
            StringAssert.StartsWith("334", on.ReadUntil("\r\n"));
            on.Send(Convert.ToBase64String(Encoding.UTF8.GetBytes(AccountPassword)) + "\r\n");
            StringAssert.StartsWith("235", on.ReadUntil("\r\n"), "The On listener takes the account's password.");
            on.Send("QUIT\r\n");
         }

         ClassicAssert.AreEqual(0, AutoBans(Net + "32").Count, "A logon on the On listener banned its address.");

         using (TcpConnection honeypot = Open(HoneypotPort, address, out string honeypotEhlo))
         {
            ClassicAssert.IsNotNull(AuthLine(honeypotEhlo), "The honeypot does not offer AUTH: " + honeypotEhlo);
            ClassicAssert.AreEqual(AuthLine(onEhlo), AuthLine(honeypotEhlo), "The honeypot's AUTH offer differs from the On listener's.");

            honeypot.Send("AUTH PLAIN " + credential + "\r\n");
            StringAssert.StartsWith("502 5.5.1 AUTH not permitted on this port.", honeypot.ReadUntil("\r\n"));

            // Not authenticated, and still a delivery port: mail for this server's
            // own account is taken from the unauthenticated session.
            honeypot.Send("MAIL FROM:<someone@delivery-port.example>\r\n");
            StringAssert.StartsWith("250", honeypot.ReadUntil("\r\n"));
            honeypot.Send("RCPT TO:<" + accountAddress_ + ">\r\n");
            StringAssert.StartsWith("250", honeypot.ReadUntil("\r\n"), "Inbound delivery stopped after the refused AUTH.");

            honeypot.Send("QUIT\r\n");
         }

         List<Dictionary<string, object>> bans = AutoBans(address);
         ClassicAssert.AreEqual(1, bans.Count, "One range for the address.");
         ClassicAssert.AreEqual("Auto-ban: AUTH on a delivery port " + address, MiniJson.Text(bans[0], "name"));

         ClassicAssert.IsTrue(XclientRefused(address), "A new session from the banned address was let in.");

         string line = "Auto-ban: " + address + " is banned for " + BanMinutes + " minutes after AUTH on a delivery port (port " +
                       HoneypotPort + ", mechanism PLAIN, listener honeypot)";
         ClassicAssert.IsTrue(LogWithin(line, TimeSpan.FromSeconds(10)), "The ban's line was not written: " + line);
         ClassicAssert.IsFalse(LogHandler.DefaultLogContains(credential), "The AUTH argument reached the log.");
         ClassicAssert.IsFalse(LogHandler.DefaultLogContains(AccountPassword), "The password reached the log.");
      }

      // ------------------------------------------------------------------ off

      [Test]
      [Description("An Off listener names AUTH neither in EHLO nor in HELP, where the On listener and the honeypot name it; an AUTH sent anyway is answered 502 5.5.1 and bans the address.")]
      public void AnOffListenerOffersNoAuthAndBansTheFirstAuth()
      {
         string address = Net + "33";

         using (TcpConnection on = Open(25, Net + "34", out string onEhlo))
         {
            ClassicAssert.IsNotNull(AuthLine(onEhlo), "Negative control: the On listener offers AUTH.");
            on.Send("QUIT\r\n");
         }

         using (TcpConnection honeypot = Open(HoneypotPort, Net + "34", out string unused))
         {
            honeypot.Send("HELP\r\n");
            StringAssert.Contains(" AUTH", honeypot.ReadUntil("\r\n"), "The honeypot's HELP names AUTH, as On's does.");
            honeypot.Send("QUIT\r\n");
         }

         using (TcpConnection off = Open(OffPort, address, out string offEhlo))
         {
            ClassicAssert.IsNull(AuthLine(offEhlo), "The Off listener offers AUTH: " + offEhlo);

            off.Send("HELP\r\n");
            StringAssert.DoesNotContain(" AUTH", off.ReadUntil("\r\n"), "The Off listener's HELP names AUTH.");

            off.Send("AUTH LOGIN\r\n");
            StringAssert.StartsWith("502 5.5.1 AUTH not permitted on this port.", off.ReadUntil("\r\n"));
            off.Send("QUIT\r\n");
         }

         ClassicAssert.AreEqual(1, AutoBans(address).Count, "The AUTH on the Off listener did not ban the address.");
         ClassicAssert.AreEqual(0, AutoBans(Net + "34").Count, "An address that sent no AUTH was banned.");
         ClassicAssert.IsTrue(LogWithin("Auto-ban: " + address + " is banned for " + BanMinutes + " minutes after AUTH on a delivery port (port " +
                                        OffPort + ", mechanism LOGIN, listener off)", TimeSpan.FromSeconds(10)));
      }

      // ------------------------------------------------------------------ one ban

      [Test]
      [Description("Several AUTH lines in one session and an AUTH from a second session of the same address, open before the ban, are one range: each is answered 502, the second session's is written as banned already, and no second range is made under another name.")]
      public void ASprayOfAuthIsOneBan()
      {
         string address = Net + "36";

         using (TcpConnection first = Open(HoneypotPort, address, out string unused1))
         using (TcpConnection second = Open(HoneypotPort, address, out string unused2))
         {
            foreach (string command in new[] { "AUTH PLAIN " + Plain("a@b.example", "guess-1"), "AUTH LOGIN", "AUTH CRAM-MD5", "AUTH PLAIN" })
            {
               first.Send(command + "\r\n");
               StringAssert.StartsWith("502 5.5.1", first.ReadUntil("\r\n"), command);
            }

            second.Send("AUTH PLAIN " + Plain("a@b.example", "guess-2") + "\r\n");
            StringAssert.StartsWith("502 5.5.1", second.ReadUntil("\r\n"));

            first.Send("QUIT\r\n");
            second.Send("QUIT\r\n");
         }

         List<Dictionary<string, object>> bans = AutoBans(address);
         ClassicAssert.AreEqual(1, bans.Count, "More than one range for one address: " + string.Join(", ", bans.Select(b => MiniJson.Text(b, "name"))));
         ClassicAssert.AreEqual("Auto-ban: AUTH on a delivery port " + address, MiniJson.Text(bans[0], "name"));
         ClassicAssert.IsTrue(LogWithin("AUTH on a delivery port: " + address + " sent AUTH to port " + HoneypotPort +
                                        " (mechanism PLAIN, listener honeypot); refused, banned already.", TimeSpan.FromSeconds(10)),
            "The second session's AUTH was not judged as banned already.");
      }

      // ------------------------------------------------------------------ never banned

      [Test]
      [Description("An address on AutoBanNeverBan and this computer's own address are refused 502 and not banned, and each can open a new session afterwards; the refusal is written in a line the fail2ban filter does not match. Negative control: an unlisted address is banned in the same way.")]
      public void NeverBannedAndLoopbackAddressesAreRefusedButNotBanned()
      {
         using (TcpConnection listed = Open(HoneypotPort, NeverBanned, out string unused))
         {
            listed.Send("AUTH LOGIN\r\n");
            StringAssert.StartsWith("502 5.5.1", listed.ReadUntil("\r\n"));
            listed.Send("QUIT\r\n");
         }

         ClassicAssert.AreEqual(0, AutoBans(NeverBanned).Count, "An address on AutoBanNeverBan was banned.");
         ClassicAssert.IsFalse(XclientRefused(NeverBanned), "An address on AutoBanNeverBan cannot connect again.");
         ClassicAssert.IsTrue(LogWithin("AUTH on a delivery port: " + NeverBanned + " sent AUTH to port " + HoneypotPort +
                                        " (mechanism LOGIN, listener honeypot); refused, not banned: AutoBanNeverBan lists it.", TimeSpan.FromSeconds(10)));

         using (TcpConnection local = Open(HoneypotPort, null, out string unused))
         {
            local.Send("AUTH LOGIN\r\n");
            StringAssert.StartsWith("502 5.5.1", local.ReadUntil("\r\n"));
            local.Send("QUIT\r\n");
         }

         ClassicAssert.IsFalse(Ranges().Any(r => MiniJson.Text(r, "name").StartsWith("Auto-ban: AUTH on a delivery port 127.", StringComparison.Ordinal)),
            "This computer's own address was banned.");
         ClassicAssert.IsTrue(LogWithin("listener honeypot); refused, not banned: it is this computer's loopback address.", TimeSpan.FromSeconds(10)));

         using (TcpConnection again = Open(HoneypotPort, null, out string ehlo))
         {
            StringAssert.Contains("250", ehlo, "This computer cannot connect again.");
            again.Send("QUIT\r\n");
         }

         string unlisted = Net + "46";
         using (TcpConnection control = Open(HoneypotPort, unlisted, out string unused))
         {
            control.Send("AUTH LOGIN\r\n");
            StringAssert.StartsWith("502 5.5.1", control.ReadUntil("\r\n"));
            control.Send("QUIT\r\n");
         }

         ClassicAssert.AreEqual(1, AutoBans(unlisted).Count, "Negative control: an unlisted address was not banned.");
      }

      [Test]
      [Description("With the ban minutes at 0 an AUTH is still refused, and nothing is banned. Negative control: at five minutes the next address is banned.")]
      public void WithNoBanMinutesAuthIsRefusedAndNotBanned()
      {
         PutSettings("{\"minutes_to_ban\":0}");

         string address = Net + "47";
         using (TcpConnection session = Open(HoneypotPort, address, out string unused))
         {
            session.Send("AUTH PLAIN\r\n");
            StringAssert.StartsWith("502 5.5.1", session.ReadUntil("\r\n"));
            session.Send("QUIT\r\n");
         }

         ClassicAssert.AreEqual(0, AutoBans(address).Count, "An address was banned for 0 minutes.");
         ClassicAssert.IsTrue(LogWithin("AUTH on a delivery port: " + address + " sent AUTH to port " + HoneypotPort +
                                        " (mechanism PLAIN, listener honeypot); refused, not banned: AutoBanMinutes is 0.", TimeSpan.FromSeconds(10)));

         PutSettings("{\"minutes_to_ban\":" + BanMinutes + "}");

         string control = Net + "48";
         using (TcpConnection session = Open(HoneypotPort, control, out string unused))
         {
            session.Send("AUTH PLAIN\r\n");
            StringAssert.StartsWith("502 5.5.1", session.ReadUntil("\r\n"));
            session.Send("QUIT\r\n");
         }

         ClassicAssert.AreEqual(1, AutoBans(control).Count, "Negative control: at five minutes the address was not banned.");
      }

      [Test]
      [Description("The mechanism reaches the log only as a SASL mechanism name: a word that is not one is written as (not a mechanism name) and an AUTH with none as (none); what the client sent in its place is not written.")]
      public void TheMechanismIsWrittenOnlyAsAMechanismName()
      {
         string odd = Net + "49";
         using (TcpConnection session = Open(HoneypotPort, odd, out string unused))
         {
            session.Send("AUTH X<nbsp>\"evil\" c2VjcmV0\r\n");
            StringAssert.StartsWith("502 5.5.1", session.ReadUntil("\r\n"));
            session.Send("QUIT\r\n");
         }

         string bare = Net + "50";
         using (TcpConnection session = Open(OffPort, bare, out string unused))
         {
            session.Send("AUTH\r\n");
            StringAssert.StartsWith("502 5.5.1", session.ReadUntil("\r\n"));
            session.Send("QUIT\r\n");
         }

         ClassicAssert.IsTrue(LogWithin("Auto-ban: " + odd + " is banned for " + BanMinutes + " minutes after AUTH on a delivery port (port " +
                                        HoneypotPort + ", mechanism (not a mechanism name), listener honeypot)", TimeSpan.FromSeconds(10)));
         ClassicAssert.IsTrue(LogWithin("Auto-ban: " + bare + " is banned for " + BanMinutes + " minutes after AUTH on a delivery port (port " +
                                        OffPort + ", mechanism (none), listener off)", TimeSpan.FromSeconds(10)));

         string log = LogHandler.ReadCurrentDefaultLog();
         foreach (string line in log.Split('\n').Where(l => l.Contains("AUTH on a delivery port")))
            StringAssert.DoesNotContain("<nbsp>", line, "The client's word reached the ban's line.");
      }

      // ------------------------------------------------------------------ the route

      [Test]
      [Description("GET /api/v1/ports carries smtp_authentication for every port; a word that is not on, off or honeypot is refused, and so is off or honeypot on a port that is not SMTP. Negative control: an IMAP port with on is taken.")]
      public void ThePortRouteCarriesTheModeAndRefusesItOffSmtp()
      {
         List<Dictionary<string, object>> ports = Ports();
         ClassicAssert.AreEqual("honeypot", MiniJson.Text(PortByNumber(ports, HoneypotPort), "smtp_authentication"));
         ClassicAssert.AreEqual("off", MiniJson.Text(PortByNumber(ports, OffPort), "smtp_authentication"));
         ClassicAssert.AreEqual("on", MiniJson.Text(PortByNumber(ports, TestPorts.Smtp), "smtp_authentication"), "Port 25, which the suite signs in on, is On.");

         long honeypotId = MiniJson.Number(PortByNumber(ports, HoneypotPort), "id");
         (int status, string body) bogus = Http("PUT", "/api/v1/ports/" + honeypotId,
            "{\"protocol\":\"smtp\",\"address\":\"0.0.0.0\",\"port\":" + HoneypotPort + ",\"smtp_authentication\":\"maybe\"}");
         ClassicAssert.AreEqual(400, bogus.status, bogus.body);
         StringAssert.Contains("smtp_authentication must be on, off or honeypot", bogus.body);
         ClassicAssert.AreEqual("honeypot", MiniJson.Text(PortByNumber(Ports(), HoneypotPort), "smtp_authentication"), "A refused PUT changed the port.");

         (int status, string body) imapHoneypot = Http("POST", "/api/v1/ports",
            "{\"protocol\":\"imap\",\"address\":\"0.0.0.0\",\"port\":" + ImapProbePort + ",\"smtp_authentication\":\"honeypot\"}");
         ClassicAssert.AreEqual(400, imapHoneypot.status, imapHoneypot.body);
         StringAssert.Contains("SMTP authentication can be Off or Honeypot on an SMTP port only.", imapHoneypot.body);

         (int status, string body) imapOn = Http("POST", "/api/v1/ports",
            "{\"protocol\":\"imap\",\"address\":\"0.0.0.0\",\"port\":" + ImapProbePort + ",\"smtp_authentication\":\"on\"}");
         ClassicAssert.AreEqual(201, imapOn.status, "Negative control: " + imapOn.body);
         ClassicAssert.AreEqual("on", MiniJson.Text(MiniJson.Object(imapOn.body), "smtp_authentication"));
         Http("DELETE", "/api/v1/ports/" + MiniJson.Number(MiniJson.Object(imapOn.body), "id"));

         long offId = MiniJson.Number(PortByNumber(ports, OffPort), "id");
         (int status, string body) toImap = Http("PUT", "/api/v1/ports/" + offId,
            "{\"protocol\":\"imap\",\"address\":\"0.0.0.0\",\"port\":" + OffPort + ",\"smtp_authentication\":\"off\"}");
         ClassicAssert.AreEqual(400, toImap.status, toImap.body);
         ClassicAssert.AreEqual("smtp", MiniJson.Text(PortByNumber(Ports(), OffPort), "protocol"), "A refused PUT changed the port.");
      }

      // ------------------------------------------------------------------ helpers

      /// <summary>
      ///    A session to the port: EHLO from 127.0.0.1 and, when an address is
      ///    given, XCLIENT to it and EHLO again. The EHLO reply the client ends with
      ///    is handed back.
      /// </summary>
      private static TcpConnection Open(int port, string address, out string ehlo)
      {
         var socket = new TcpConnection();
         ClassicAssert.IsTrue(socket.Connect(port), "Could not connect to port " + port + ".");
         StringAssert.StartsWith("220", socket.ReadUntil("\r\n"));

         socket.Send("EHLO upstream.delivery-port.test\r\n");
         ehlo = socket.ReadUntil("\r\n250 ");

         if (address != null)
         {
            StringAssert.Contains("XCLIENT", ehlo, "XCLIENT is not offered to 127.0.0.1.");
            socket.Send("XCLIENT ADDR=" + address + "\r\n");
            StringAssert.StartsWith("220", socket.ReadUntil("\r\n"), "XCLIENT to " + address + " was refused.");

            socket.Send("EHLO client.delivery-port.test\r\n");
            ehlo = socket.ReadUntil("\r\n250 ");
         }

         return socket;
      }

      /// <summary>True when a session moved to the address is refused at XCLIENT, as a banned address is.</summary>
      private static bool XclientRefused(string address)
      {
         using (var socket = new TcpConnection())
         {
            ClassicAssert.IsTrue(socket.Connect(25), "Could not connect to SMTP.");
            socket.ReadUntil("220");
            socket.Send("EHLO upstream.delivery-port.test\r\n");
            socket.ReadUntil("\r\n250 ");
            socket.Send("XCLIENT ADDR=" + address + "\r\n");
            string reply = socket.ReadUntil("\r\n");
            socket.Send("QUIT\r\n");
            return reply.StartsWith("550", StringComparison.Ordinal);
         }
      }

      /// <summary>The 250-AUTH or 250 AUTH line of an EHLO reply, or null.</summary>
      private static string AuthLine(string ehlo)
      {
         return ehlo.Split(new[] { "\r\n" }, StringSplitOptions.RemoveEmptyEntries)
                    .Select(l => l.Trim())
                    .FirstOrDefault(l => l.StartsWith("250-AUTH", StringComparison.Ordinal) || l.StartsWith("250 AUTH", StringComparison.Ordinal));
      }

      private static string Plain(string user, string password)
      {
         return Convert.ToBase64String(Encoding.UTF8.GetBytes("\0" + user + "\0" + password));
      }

      private static bool LogWithin(string text, TimeSpan deadline)
      {
         DateTime until = DateTime.UtcNow + deadline;

         while (true)
         {
            if (LogHandler.DefaultLogContains(text))
               return true;

            if (DateTime.UtcNow > until)
               return false;

            Thread.Sleep(250);
         }
      }

      private static void CreatePort(int number, string mode)
      {
         (int status, string body) created = Http("POST", "/api/v1/ports",
            "{\"protocol\":\"smtp\",\"address\":\"0.0.0.0\",\"port\":" + number + ",\"smtp_authentication\":\"" + mode + "\"}");
         ClassicAssert.AreEqual(201, created.status, created.body);
         ClassicAssert.AreEqual(mode, MiniJson.Text(MiniJson.Object(created.body), "smtp_authentication"), created.body);
      }

      private static void WaitForListener(int port)
      {
         DateTime until = DateTime.UtcNow + TimeSpan.FromSeconds(20);

         while (true)
         {
            using (var probe = new TcpClient())
            {
               try
               {
                  probe.Connect(TestPorts.HostAddress, port);
                  return;
               }
               catch (SocketException)
               {
                  if (DateTime.UtcNow > until)
                     throw;
               }
            }

            Thread.Sleep(250);
         }
      }

      private static List<Dictionary<string, object>> Ports()
      {
         (int status, string body) = Http("GET", "/api/v1/ports");
         ClassicAssert.AreEqual(200, status, body);

         object parsed = MiniJson.Parse(body);
         var list = parsed as List<object> ?? MiniJson.List((Dictionary<string, object>) parsed, "ports");
         return list.Cast<Dictionary<string, object>>().ToList();
      }

      private static Dictionary<string, object> PortByNumber(List<Dictionary<string, object>> ports, int number)
      {
         Dictionary<string, object> port = ports.FirstOrDefault(p => MiniJson.Number(p, "port") == number && MiniJson.Text(p, "protocol") == "smtp");
         ClassicAssert.IsNotNull(port, "No SMTP port " + number + " is listed.");
         return port;
      }

      /// <summary>The auto-ban ranges covering exactly that address.</summary>
      private static List<Dictionary<string, object>> AutoBans(string address)
      {
         return Ranges().Where(r => MiniJson.Text(r, "name").StartsWith("Auto-ban: ", StringComparison.Ordinal) &&
                                    MiniJson.Text(r, "lower") == address && MiniJson.Text(r, "upper") == address).ToList();
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
      ///    Everything this fixture made: its range, the bans on its net and on this
      ///    computer, and its listeners.
      /// </summary>
      private static void TakeBackOurs()
      {
         foreach (Dictionary<string, object> range in Ranges())
         {
            string name = MiniJson.Text(range, "name");
            bool ours = name == RangeName ||
                        (name.StartsWith("Auto-ban: ", StringComparison.Ordinal) && MiniJson.Text(range, "lower").StartsWith(Net, StringComparison.Ordinal)) ||
                        name.StartsWith("Auto-ban: AUTH on a delivery port 127.", StringComparison.Ordinal);

            if (ours)
               Http("DELETE", "/api/v1/ipranges/" + MiniJson.Number(range, "id"));
         }

         foreach (Dictionary<string, object> port in Ports())
         {
            long number = MiniJson.Number(port, "port");
            if (number == HoneypotPort || number == OffPort || number == ImapProbePort)
               Http("DELETE", "/api/v1/ports/" + MiniJson.Number(port, "id"));
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
