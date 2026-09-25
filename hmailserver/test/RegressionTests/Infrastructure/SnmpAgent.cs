// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Linq;
using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Shared;

namespace RegressionTests.Infrastructure
{
   /// <summary>
   ///    The SNMP agent (Common/Util/SnmpAgent.h, docs/Snmp.md), in the service, on
   ///    a test port, from a manager this file is.
   ///
   ///    THE CLIENT IS THE TEST'S OWN. Its BER, its USM - the SHA-256 password-to-key
   ///    and localisation, HMAC-SHA-256 truncated to 192 bits, AES-128 in CFB mode
   ///    built on the framework's AES block - share no code with the server, which is
   ///    the point: an agent and a client written from one misreading of RFC 3414
   ///    would agree with each other and with nothing else. SnmpTester, inside the
   ///    service, holds the decoder's bounds and the cryptography's test vectors;
   ///    this holds the agent to a second implementation over a real socket.
   ///
   ///    The settings go in over REST, since there is no COM property for them (as
   ///    there is none for the metrics listener), and the agent reads them when it
   ///    starts, so each change is followed by a Reinitialize. SNMP listens on
   ///    10200, bound to 127.0.0.1, answering 127.0.0.1; REST on 10201. Both are
   ///    this fixture's own ports. Everything is switched off again in the teardown.
   /// </summary>
   [TestFixture]
   public class SnmpAgent : TestFixtureBase
   {
      private const int SnmpPort = 10200;
      private const int PreferredRestPort = 10201;
      private const string AdminPassword = "testar";

      private const string User = "fixture-monitor";
      private const string AuthPassword = "Fixture-Auth-Passw0rd";
      private const string PrivPassword = "Fixture-Priv-Passw0rd";
      private const string Community = "fixture-community-9";

      private const string SysDescr = "1.3.6.1.2.1.1.1.0";
      private const string SysObjectId = "1.3.6.1.2.1.1.2.0";
      private const string SysUpTime = "1.3.6.1.2.1.1.3.0";
      private const string MtaTransmitted = "1.3.6.1.2.1.28.1.1.3.1";
      private const string MtaGroupTransmittedSmtp = "1.3.6.1.2.1.28.2.1.5.1.1";
      private const string MtaGroupNameSmtp = "1.3.6.1.2.1.28.2.1.25.1.1";
      private const string UsmStatsNotInTimeWindows = "1.3.6.1.6.3.15.1.1.2.0";
      private const string UsmStatsUnknownEngineIds = "1.3.6.1.6.3.15.1.1.4.0";
      private const string UsmStatsWrongDigests = "1.3.6.1.6.3.15.1.1.5.0";

      private const byte TagGetRequest = 0xA0;
      private const byte TagGetNextRequest = 0xA1;
      private const byte TagResponse = 0xA2;
      private const byte TagGetBulkRequest = 0xA5;
      private const byte TagReport = 0xA8;
      private const byte TagEndOfMibView = 0x82;

      private static readonly IPAddress Outsider = IPAddress.Parse("127.0.0.2");

      private int restPort_;

      // ---- the fixture ---------------------------------------------------------

      // Named rather than overriding: NUnit runs the base [SetUp] first and this one
      // after it, and the [TearDown]s the other way round.
      [SetUp]
      public void StartTheAgent()
      {
         _settings.SetAdministratorPassword(AdminPassword);
         restPort_ = RestListener.Start(PreferredRestPort);
         _application.Reinitialize();

         Put("{\"snmp_port\":" + SnmpPort + ",\"snmp_bind_address\":\"127.0.0.1\",\"snmp_allowed_addresses\":\"127.0.0.1\"," +
             "\"snmp_v3_user\":\"" + User + "\",\"snmp_v3_auth_password\":\"" + AuthPassword + "\",\"snmp_v3_priv_password\":\"" + PrivPassword + "\"," +
             "\"snmp_v2c_enabled\":false,\"snmp_v2c_community\":\"" + Community + "\"," +
             "\"snmp_contact\":\"postmaster@test.example\",\"snmp_location\":\"The test bench\",\"snmp_enabled\":true}");

         _application.Reinitialize();
      }

      [TearDown]
      public void StopTheAgent()
      {
         // Off, and every value back to what a new server has, so that no later
         // fixture meets a listener on 10200 or a user it did not make.
         try
         {
            Put("{\"snmp_enabled\":false,\"snmp_v2c_enabled\":false,\"snmp_port\":161,\"snmp_bind_address\":\"\",\"snmp_allowed_addresses\":\"\"," +
                "\"snmp_v3_user\":\"\",\"snmp_v3_auth_password\":\"\",\"snmp_v3_priv_password\":\"\",\"snmp_v2c_community\":\"\"," +
                "\"snmp_contact\":\"\",\"snmp_location\":\"\"}");
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            Console.WriteLine("The SNMP settings could not be put back: " + fatalCheck.Message);
         }

         RestListener.Stop();
         _application.Reinitialize();
      }

      // ---- the tests -----------------------------------------------------------

      [Test]
      [Description("Discovery is answered with a Report carrying the engine ID, boots and time; a request outside the time window with an authenticated Report; then an authPriv Get of sysUpTime and an MTA-MIB counter is answered, authenticated and encrypted.")]
      public void DiscoveryThenAGetOfUptimeAndAMailMibCounter()
      {
         using (var client = new SnmpV3Client(IPAddress.Loopback, SnmpPort, User, AuthPassword, PrivPassword))
         {
            SnmpAnswer report = client.Discover();
            Assert.AreEqual(TagReport, report.PduTag, "Discovery was not answered with a Report.");
            Assert.AreEqual(0, report.Flags & 0x03, "The discovery Report is not noAuthNoPriv.");
            Assert.AreEqual(UsmStatsUnknownEngineIds, report.Bindings[0].Oid);
            Assert.AreEqual(17, client.EngineId.Length, "The engine ID is not the enterprise form's seventeen octets.");
            CollectionAssert.AreEqual(new byte[] { 0x80, 0x00, 0x7E, 0xD9, 0x05 }, client.EngineId.Take(5).ToArray(),
               "The engine ID does not begin with the enterprise form for 32473, format 5.");
            Assert.GreaterOrEqual(client.Boots, 1, "snmpEngineBoots was not counted.");

            // Outside the window: an authenticated Report with the boots and time.
            SnmpAnswer stale = client.Request(TagGetRequest, new[] { SysUpTime }, 1500, bootsOverride: 0, timeOverride: 0);
            Assert.IsNotNull(stale, "A request outside the time window was not answered.");
            Assert.AreEqual(TagReport, stale.PduTag);
            Assert.AreEqual(0x01, stale.Flags & 0x03, "The time-window Report is not authenticated.");
            Assert.AreEqual(UsmStatsNotInTimeWindows, stale.Bindings[0].Oid);
            Assert.AreEqual(client.Boots, stale.Boots, "The time-window Report does not carry the engine's boots.");

            SnmpAnswer answer = client.Request(TagGetRequest, new[] { SysUpTime, MtaTransmitted }, 1500);
            Assert.IsNotNull(answer, "An authPriv Get was not answered.");
            Assert.AreEqual(TagResponse, answer.PduTag);
            Assert.AreEqual(0x03, answer.Flags & 0x03, "The answer is not authPriv.");
            Assert.AreEqual(0, answer.ErrorStatus);
            Assert.AreEqual(2, answer.Bindings.Count);
            Assert.AreEqual(SysUpTime, answer.Bindings[0].Oid);
            Assert.AreEqual(0x43, answer.Bindings[0].Tag, "sysUpTime is not TimeTicks.");
            Assert.AreEqual(MtaTransmitted, answer.Bindings[1].Oid);
            Assert.AreEqual(0x41, answer.Bindings[1].Tag, "mtaTransmittedMessages is not a Counter32.");

            SnmpAnswer name = client.Request(TagGetRequest, new[] { SysDescr, MtaGroupNameSmtp }, 1500);
            Assert.IsNotNull(name);
            StringAssert.StartsWith("hMailServer ", Encoding.UTF8.GetString(name.Bindings[0].Value), "sysDescr does not name the server.");
            Assert.AreEqual("SMTP", Encoding.ASCII.GetString(name.Bindings[1].Value), "The SMTP group is not named SMTP.");

            // sysObjectID is the placeholder arc's .1, as docs/Snmp.md says.
            SnmpAnswer identity = client.Request(TagGetRequest, new[] { SysObjectId }, 1500);
            Assert.IsNotNull(identity, "sysObjectID was not answered.");
            Assert.AreEqual(0x06, identity.Bindings[0].Tag, "sysObjectID is not an OBJECT IDENTIFIER.");
            Assert.AreEqual("1.3.6.1.4.1.32473.1.1", Ber.OidToString(identity.Bindings[0].Value, 0, identity.Bindings[0].Value.Length),
               "sysObjectID is not the one docs/Snmp.md names.");

            // The same request twice: each answer under a salt of its own.
            SnmpAnswer once = client.Request(TagGetRequest, new[] { SysUpTime }, 1500);
            SnmpAnswer again = client.Request(TagGetRequest, new[] { SysUpTime }, 1500);
            Assert.IsNotNull(once);
            Assert.IsNotNull(again);
            Assert.AreEqual(8, once.Salt.Length, "An answer's msgPrivacyParameters is not 8 octets.");
            CollectionAssert.AreNotEqual(once.Salt, again.Salt, "Two answers were encrypted under one salt.");
         }
      }

      [Test]
      [Description("mtaTransmittedMessages and the SMTP group's transmitted count go up when a message is delivered.")]
      public void TheTransmittedCounterMovesWhenAMessageIsDelivered()
      {
         string address = "snmp-reader@" + _domain.Name;
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, address, "test");

         using (var client = new SnmpV3Client(IPAddress.Loopback, SnmpPort, User, AuthPassword, PrivPassword))
         {
            client.Discover();

            SnmpAnswer before = client.Request(TagGetRequest, new[] { MtaTransmitted, MtaGroupTransmittedSmtp }, 1500);
            Assert.IsNotNull(before, "The counters could not be read before the delivery.");
            ulong transmittedBefore = Ber.ReadUnsigned(before.Bindings[0].Value);
            ulong groupBefore = Ber.ReadUnsigned(before.Bindings[1].Value);

            SmtpClientSimulator.StaticSend("snmp-sender@example.org", address, "Counted", "A message the SNMP agent counts.");
            Pop3ClientSimulator.AssertMessageCount(address, "test", 1);

            // The counter moves when the delivery pass ends, a moment after the
            // message is in the mailbox.
            ulong transmittedAfter = transmittedBefore;
            ulong groupAfter = groupBefore;
            var clock = Stopwatch.StartNew();

            while (clock.ElapsedMilliseconds < 10000)
            {
               SnmpAnswer after = client.Request(TagGetRequest, new[] { MtaTransmitted, MtaGroupTransmittedSmtp }, 1500);
               Assert.IsNotNull(after, "The counters could not be read after the delivery.");
               transmittedAfter = Ber.ReadUnsigned(after.Bindings[0].Value);
               groupAfter = Ber.ReadUnsigned(after.Bindings[1].Value);

               if (transmittedAfter > transmittedBefore && groupAfter > groupBefore)
                  break;

               Thread.Sleep(200);
            }

            Assert.Greater(transmittedAfter, transmittedBefore, "mtaTransmittedMessages did not move when a message was delivered.");
            Assert.Greater(groupAfter, groupBefore, "The SMTP group's mtaGroupTransmittedMessages did not move when a message was delivered.");
         }
      }

      [Test]
      [Description("GetNext walks the mail MIB in order and leaves it; GetBulk answers within the msgMaxSize a request names and within the repetition cap.")]
      public void GetNextWalksInOrderAndGetBulkIsBounded()
      {
         using (var client = new SnmpV3Client(IPAddress.Loopback, SnmpPort, User, AuthPassword, PrivPassword))
         {
            client.Discover();

            string cursor = "1.3.6.1.2.1.28";
            var seen = new List<string>();

            for (int step = 0; step < 200; step++)
            {
               SnmpAnswer answer = client.Request(TagGetNextRequest, new[] { cursor }, 1500);
               Assert.IsNotNull(answer, "A GetNext was not answered.");
               Assert.AreEqual(1, answer.Bindings.Count);

               string next = answer.Bindings[0].Oid;
               if (answer.Bindings[0].Tag == TagEndOfMibView || !next.StartsWith("1.3.6.1.2.1.28.", StringComparison.Ordinal))
                  break;

               Assert.Greater(CompareOids(next, cursor), 0, "GetNext went from " + cursor + " to " + next + ", which is not forward.");
               seen.Add(next);
               cursor = next;
            }

            CollectionAssert.Contains(seen, MtaTransmitted, "The walk did not pass mtaTransmittedMessages.");
            CollectionAssert.Contains(seen, MtaGroupNameSmtp, "The walk did not pass the SMTP group's name.");

            // max-repetitions far past the cap, in 484 octets.
            SnmpAnswer small = client.Request(TagGetBulkRequest, new[] { "1.3.6.1" }, 484, second: 0, third: 100000);
            Assert.IsNotNull(small, "A GetBulk in 484 octets was not answered.");
            Assert.LessOrEqual(client.LastResponseLength, 484, "A GetBulk answer was longer than the msgMaxSize the request named.");
            Assert.AreEqual(0, small.ErrorStatus);
            Assert.Greater(small.Bindings.Count, 0, "A GetBulk in 484 octets answered no bindings.");

            // Room for all of them: still no more than the agent's cap of 64.
            SnmpAnswer large = client.Request(TagGetBulkRequest, new[] { "1.3.6.1" }, 60000, second: 0, third: 100000);
            Assert.IsNotNull(large, "A large GetBulk was not answered.");
            Assert.LessOrEqual(large.Bindings.Count, 64, "GetBulk's max-repetitions was not held to 64.");
            Assert.Greater(large.Bindings.Count, small.Bindings.Count, "A GetBulk with room answered no more than one without.");
         }
      }

      [Test]
      [Description("A request made with the wrong authentication password is answered with a usmStatsWrongDigests Report and no data.")]
      public void AWrongPasswordIsAnsweredWithAReportAndNoData()
      {
         using (var impostor = new SnmpV3Client(IPAddress.Loopback, SnmpPort, User, "Not-The-Auth-Passw0rd", PrivPassword))
         {
            impostor.Discover();

            SnmpAnswer answer = impostor.Request(TagGetRequest, new[] { SysUpTime, MtaTransmitted }, 1500);
            Assert.IsNotNull(answer, "A wrong password was not answered with a Report.");
            Assert.AreEqual(TagReport, answer.PduTag, "A wrong password was answered with something other than a Report.");
            Assert.AreEqual(0, answer.Flags & 0x03, "The wrong-digest Report is not noAuthNoPriv.");
            Assert.AreEqual(1, answer.Bindings.Count);
            Assert.AreEqual(UsmStatsWrongDigests, answer.Bindings[0].Oid, "The Report is not usmStatsWrongDigests.");
         }
      }

      [Test]
      [Description("A datagram from an address outside the allow list is not answered, and is answered once a CIDR range names it.")]
      public void AnAddressOutsideTheAllowListIsNotAnswered()
      {
         using (var outsider = new SnmpV3Client(Outsider, SnmpPort, User, AuthPassword, PrivPassword))
         {
            Assert.IsNull(outsider.TryDiscover(1500), "127.0.0.2 is not in the allow list and was answered.");
         }

         // The control: the same address, named, is answered - so the silence
         // above was the list and not the address.
         Put("{\"snmp_allowed_addresses\":\"127.0.0.1, 127.0.0.2/32\"}");
         _application.Reinitialize();

         using (var outsider = new SnmpV3Client(Outsider, SnmpPort, User, AuthPassword, PrivPassword))
         {
            Assert.IsNotNull(outsider.TryDiscover(3000), "127.0.0.2, named by a range, was not answered.");
            SnmpAnswer answer = outsider.Request(TagGetRequest, new[] { SysUpTime }, 1500);
            Assert.IsNotNull(answer);
            Assert.AreEqual(TagResponse, answer.PduTag);
         }
      }

      [Test]
      [Description("SNMPv2c is not answered while SnmpV2cEnabled is off, is answered with the right community when it is on, and not with a wrong one.")]
      public void SnmpV2cIsRefusedWhileOffAndAnsweredWhenOn()
      {
         Assert.IsNull(V2cGet(Community, SysDescr, 1500), "SNMPv2c was answered while SnmpV2cEnabled is off.");

         Put("{\"snmp_v2c_enabled\":true}");
         _application.Reinitialize();

         SnmpAnswer answer = V2cGet(Community, SysDescr, 3000);
         Assert.IsNotNull(answer, "SNMPv2c with the right community was not answered.");
         Assert.AreEqual(TagResponse, answer.PduTag);
         Assert.AreEqual(SysDescr, answer.Bindings[0].Oid);
         StringAssert.StartsWith("hMailServer ", Encoding.UTF8.GetString(answer.Bindings[0].Value));

         Assert.IsNull(V2cGet("public", SysDescr, 1500), "SNMPv2c with a wrong community was answered.");
      }

      [Test]
      [Description("The REST keys refuse what the agent could not use, and the two passwords and the community are never read back.")]
      public void TheSettingsAreRefusedWhenWrongAndTheSecretsNeverReadBack()
      {
         AssertRefused("{\"snmp_bind_address\":\"not-an-address\"}", "SnmpBindAddress");
         AssertRefused("{\"snmp_allowed_addresses\":\"127.0.0.1, 10.0.0.0/33\"}", "SnmpAllowedAddresses");
         AssertRefused("{\"snmp_v3_auth_password\":\"short\"}", "snmp_v3_auth_password");
         AssertRefused("{\"snmp_v3_user\":\"has a space\"}", "SnmpV3User");
         AssertRefused("{\"snmp_port\":70000}", "snmp_port");

         // sysContact and sysLocation are DisplayStrings: one line of printable
         // ASCII. A letter outside ASCII, and a line break, are each refused.
         AssertRefused("{\"snmp_contact\":\"caf\u00e9 postmaster\"}", "snmp_contact");
         AssertRefused("{\"snmp_location\":\"Rack 4\\nLondon\"}", "snmp_location");

         (int status, string body) settings = RestRequest.Send("127.0.0.1", restPort_, "GET", "/api/v1/settings", Admin(), null);
         Assert.AreEqual(200, settings.status, settings.body);
         StringAssert.Contains("\"snmp_enabled\":true", settings.body);
         StringAssert.Contains("\"snmp_allowed_addresses\":\"127.0.0.1\"", settings.body, "A refused PUT changed something.");
         StringAssert.DoesNotContain("snmp_v3_auth_password", settings.body, "The authentication password's key was read back.");
         StringAssert.DoesNotContain("snmp_v3_priv_password", settings.body, "The privacy password's key was read back.");
         StringAssert.DoesNotContain("snmp_v2c_community", settings.body, "The community's key was read back.");
         StringAssert.DoesNotContain(AuthPassword, settings.body);
         StringAssert.DoesNotContain(PrivPassword, settings.body);
         StringAssert.DoesNotContain(Community, settings.body);
      }

      // ---- REST ----------------------------------------------------------------

      private static string Admin()
      {
         return "Basic " + Convert.ToBase64String(Encoding.ASCII.GetBytes("Administrator:" + AdminPassword));
      }

      private void Put(string json)
      {
         (int status, string body) answer = RestRequest.Send("127.0.0.1", restPort_, "PUT", "/api/v1/settings", Admin(), json);
         Assert.AreEqual(200, answer.status, "PUT /api/v1/settings " + json + " answered " + answer.body);
      }

      private void AssertRefused(string json, string named)
      {
         (int status, string body) answer = RestRequest.Send("127.0.0.1", restPort_, "PUT", "/api/v1/settings", Admin(), json);
         Assert.AreEqual(400, answer.status, "PUT /api/v1/settings " + json + " was not refused: " + answer.body);
         StringAssert.Contains(named, answer.body, "The refusal does not name what was wrong.");
      }

      // ---- SNMPv2c ---------------------------------------------------------------

      private static SnmpAnswer V2cGet(string community, string oid, int timeoutMilliseconds)
      {
         byte[] request = Ber.Sequence(Ber.Integer(1), Ber.Octets(Encoding.ASCII.GetBytes(community)),
                                       Pdu(TagGetRequest, 77, 0, 0, new[] { oid }));

         byte[] response = Exchange(IPAddress.Loopback, SnmpPort, request, timeoutMilliseconds);
         if (response == null)
            return null;

         List<Ber.Node> top = Ber.Children(response, 0, response.Length);
         List<Ber.Node> body = Ber.Children(response, top[0].ValueOffset, top[0].Length);
         Assert.AreEqual(1, Ber.ReadInteger(response, body[0]), "The SNMPv2c answer is not version 1.");
         Assert.AreEqual(community, Encoding.ASCII.GetString(response, body[1].ValueOffset, body[1].Length));

         var answer = new SnmpAnswer();
         ReadPdu(response, body[2], answer);
         return answer;
      }

      // ---- the manager ---------------------------------------------------------

      private static byte[] Pdu(byte tag, int requestId, int second, int third, IEnumerable<string> names)
      {
         byte[][] bindings = names.Select(name => Ber.Sequence(Ber.Oid(name), new byte[] { 0x05, 0x00 })).ToArray();
         return Ber.Tlv(tag, Ber.Concat(Ber.Integer(requestId), Ber.Integer(second), Ber.Integer(third), Ber.Sequence(bindings)));
      }

      private static byte[] Exchange(IPAddress source, int port, byte[] request, int timeoutMilliseconds)
      {
         using (var udp = new UdpClient(new IPEndPoint(source, 0)))
         {
            udp.Client.ReceiveTimeout = timeoutMilliseconds;
            udp.Send(request, request.Length, new IPEndPoint(IPAddress.Loopback, port));

            try
            {
               var from = new IPEndPoint(IPAddress.Any, 0);
               return udp.Receive(ref from);
            }
            catch (SocketException)
            {
               return null;
            }
         }
      }

      private static void ReadPdu(byte[] buffer, Ber.Node pdu, SnmpAnswer answer)
      {
         answer.PduTag = pdu.Tag;

         List<Ber.Node> fields = Ber.Children(buffer, pdu.ValueOffset, pdu.Length);
         answer.RequestId = Ber.ReadInteger(buffer, fields[0]);
         answer.ErrorStatus = Ber.ReadInteger(buffer, fields[1]);
         answer.ErrorIndex = Ber.ReadInteger(buffer, fields[2]);

         foreach (Ber.Node binding in Ber.Children(buffer, fields[3].ValueOffset, fields[3].Length))
         {
            List<Ber.Node> parts = Ber.Children(buffer, binding.ValueOffset, binding.Length);
            answer.Bindings.Add(new SnmpBinding
            {
               Oid = Ber.OidToString(buffer, parts[0].ValueOffset, parts[0].Length),
               Tag = parts[1].Tag,
               Value = buffer.Skip(parts[1].ValueOffset).Take(parts[1].Length).ToArray()
            });
         }
      }

      private static int CompareOids(string left, string right)
      {
         uint[] a = left.Split('.').Select(uint.Parse).ToArray();
         uint[] b = right.Split('.').Select(uint.Parse).ToArray();

         for (int i = 0; i < Math.Min(a.Length, b.Length); i++)
         {
            if (a[i] != b[i])
               return a[i] < b[i] ? -1 : 1;
         }

         return a.Length.CompareTo(b.Length);
      }

      private sealed class SnmpBinding
      {
         public string Oid;
         public byte Tag;
         public byte[] Value;
      }

      private sealed class SnmpAnswer
      {
         public byte Flags;
         public byte[] EngineId = new byte[0];
         public long Boots;
         public long Time;
         public byte PduTag;
         public long RequestId;
         public long ErrorStatus;
         public long ErrorIndex;
         public byte[] Salt = new byte[0];
         public readonly List<SnmpBinding> Bindings = new List<SnmpBinding>();
      }

      /// <summary>
      ///    An SNMPv3 manager at authPriv with HMAC-SHA-256 and AES-128: RFC 3414
      ///    section 4's discovery, then requests in the engine's time window as the
      ///    discovery Report gave it and a stopwatch has moved it on since.
      /// </summary>
      private sealed class SnmpV3Client : IDisposable
      {
         private readonly IPAddress source_;
         private readonly int port_;
         private readonly string user_;
         private readonly string authPassword_;
         private readonly string privPassword_;
         private readonly Stopwatch clock_ = new Stopwatch();

         private byte[] authKey_;
         private byte[] privKey_;
         private int messageId_ = 5000;
         private ulong salt_ = 0x0102030405060708UL;
         private long timeAtDiscovery_;

         public byte[] EngineId { get; private set; }
         public long Boots { get; private set; }
         public int LastResponseLength { get; private set; }

         public SnmpV3Client(IPAddress source, int port, string user, string authPassword, string privPassword)
         {
            source_ = source;
            port_ = port;
            user_ = user;
            authPassword_ = authPassword;
            privPassword_ = privPassword;
            EngineId = new byte[0];
         }

         public void Dispose()
         {
         }

         public SnmpAnswer Discover()
         {
            SnmpAnswer report = TryDiscover(3000);
            Assert.IsNotNull(report, "The agent did not answer discovery.");
            return report;
         }

         public SnmpAnswer TryDiscover(int timeoutMilliseconds)
         {
            byte[] probe = Build(0x04, Pdu(TagGetRequest, 1, 0, 0, new string[0]), 1500, new byte[0], 0, 0, "");
            byte[] response = Exchange(source_, port_, probe, timeoutMilliseconds);
            if (response == null)
               return null;

            SnmpAnswer report = Parse(response);
            EngineId = report.EngineId;
            Boots = report.Boots;
            timeAtDiscovery_ = report.Time;
            clock_.Restart();

            authKey_ = Localise(PasswordToKey(authPassword_), EngineId);
            privKey_ = Localise(PasswordToKey(privPassword_), EngineId).Take(16).ToArray();
            return report;
         }

         public SnmpAnswer Request(byte tag, string[] names, int maxSize, int second = 0, int third = 0,
                                   long? bootsOverride = null, long? timeOverride = null)
         {
            long boots = bootsOverride ?? Boots;
            long time = timeOverride ?? (timeAtDiscovery_ + (long) clock_.Elapsed.TotalSeconds);

            byte[] request = Build(0x07, Pdu(tag, messageId_ + 1, second, third, names), maxSize, EngineId, boots, time, user_);
            byte[] response = Exchange(source_, port_, request, 3000);
            if (response == null)
               return null;

            LastResponseLength = response.Length;
            return Parse(response);
         }

         private byte[] Build(byte flags, byte[] pdu, int maxSize, byte[] engineId, long boots, long time, string user)
         {
            messageId_++;

            byte[] scoped = Ber.Sequence(Ber.Octets(engineId), Ber.Octets(new byte[0]), pdu);
            byte[] privParameters = new byte[0];
            byte[] data = scoped;

            if ((flags & 0x02) != 0)
            {
               salt_++;
               privParameters = Enumerable.Range(0, 8).Select(i => (byte) ((salt_ >> (8 * (7 - i))) & 0xFF)).ToArray();
               data = Ber.Octets(AesCfb(privKey_, Iv(boots, time, privParameters), scoped, true));
            }

            byte[] prefix = Ber.Concat(Ber.Octets(engineId), Ber.Integer(boots), Ber.Integer(time), Ber.Octets(Encoding.ASCII.GetBytes(user)));
            byte[] usmContent = Ber.Concat(prefix, Ber.Octets(new byte[(flags & 0x01) != 0 ? 24 : 0]), Ber.Octets(privParameters));
            byte[] usm = Ber.Tlv(0x30, usmContent);
            byte[] securityParameters = Ber.Octets(usm);
            byte[] header = Ber.Sequence(Ber.Integer(messageId_), Ber.Integer(maxSize), Ber.Octets(new[] { flags }), Ber.Integer(3));
            byte[] version = Ber.Integer(3);
            byte[] content = Ber.Concat(version, header, securityParameters, data);
            byte[] message = Ber.Tlv(0x30, content);

            if ((flags & 0x01) != 0)
            {
               // Where the 24 octets sit, from the lengths this method built rather
               // than by searching: every header in front of them, then their own
               // tag and length.
               int offset = (message.Length - content.Length) + version.Length + header.Length +
                            (securityParameters.Length - usm.Length) + (usm.Length - usmContent.Length) + prefix.Length + 2;
               Array.Copy(Digest(authKey_, message), 0, message, offset, 24);
            }

            return message;
         }

         private SnmpAnswer Parse(byte[] message)
         {
            var answer = new SnmpAnswer();

            List<Ber.Node> top = Ber.Children(message, 0, message.Length);
            Assert.AreEqual(1, top.Count, "The answer is not one SEQUENCE.");
            List<Ber.Node> body = Ber.Children(message, top[0].ValueOffset, top[0].Length);
            Assert.AreEqual(3, Ber.ReadInteger(message, body[0]), "The answer is not SNMPv3.");

            List<Ber.Node> header = Ber.Children(message, body[1].ValueOffset, body[1].Length);
            answer.Flags = message[header[2].ValueOffset];

            List<Ber.Node> usmOuter = Ber.Children(message, body[2].ValueOffset, body[2].Length);
            List<Ber.Node> usm = Ber.Children(message, usmOuter[0].ValueOffset, usmOuter[0].Length);
            answer.EngineId = message.Skip(usm[0].ValueOffset).Take(usm[0].Length).ToArray();
            answer.Boots = Ber.ReadInteger(message, usm[1]);
            answer.Time = Ber.ReadInteger(message, usm[2]);
            Ber.Node auth = usm[4];
            Ber.Node priv = usm[5];
            answer.Salt = message.Skip(priv.ValueOffset).Take(priv.Length).ToArray();

            if ((answer.Flags & 0x01) != 0)
            {
               Assert.AreEqual(24, auth.Length, "The answer's authentication parameters are not 24 octets.");
               byte[] copy = (byte[]) message.Clone();
               Array.Clear(copy, auth.ValueOffset, 24);
               CollectionAssert.AreEqual(Digest(authKey_, copy), message.Skip(auth.ValueOffset).Take(24).ToArray(),
                  "The answer's digest is not HMAC-SHA-256-192 with the user's key.");
            }

            byte[] scopedBuffer = message;
            Ber.Node scoped = body[3];

            if ((answer.Flags & 0x02) != 0)
            {
               Assert.AreEqual(0x04, scoped.Tag, "An encrypted answer's data is not an OCTET STRING.");
               Assert.AreEqual(8, priv.Length, "The answer's salt is not 8 octets.");
               byte[] encrypted = message.Skip(scoped.ValueOffset).Take(scoped.Length).ToArray();
               byte[] salt = message.Skip(priv.ValueOffset).Take(8).ToArray();
               scopedBuffer = AesCfb(privKey_, Iv(answer.Boots, answer.Time, salt), encrypted, false);
               scoped = Ber.Children(scopedBuffer, 0, scopedBuffer.Length)[0];
            }

            Assert.AreEqual(0x30, scoped.Tag, "The scoped PDU is not a SEQUENCE.");
            List<Ber.Node> scopedParts = Ber.Children(scopedBuffer, scoped.ValueOffset, scoped.Length);
            ReadPdu(scopedBuffer, scopedParts[2], answer);
            return answer;
         }

         private static byte[] Iv(long boots, long time, byte[] salt)
         {
            var iv = new byte[16];
            for (int i = 0; i < 4; i++)
            {
               iv[i] = (byte) ((boots >> (8 * (3 - i))) & 0xFF);
               iv[4 + i] = (byte) ((time >> (8 * (3 - i))) & 0xFF);
            }

            Array.Copy(salt, 0, iv, 8, 8);
            return iv;
         }

         // RFC 3414 A.2 with SHA-256 (RFC 7860 9.3): a megabyte of the password,
         // hashed; then H(Ku || engineID || Ku).
         private static byte[] PasswordToKey(string password)
         {
            byte[] bytes = Encoding.ASCII.GetBytes(password);

            using (SHA256 sha = SHA256.Create())
            {
               var chunk = new byte[64];
               int index = 0;

               for (int done = 0; done < 1048576; done += 64)
               {
                  for (int i = 0; i < 64; i++)
                     chunk[i] = bytes[index++ % bytes.Length];

                  sha.TransformBlock(chunk, 0, 64, null, 0);
               }

               sha.TransformFinalBlock(new byte[0], 0, 0);
               return sha.Hash;
            }
         }

         private static byte[] Localise(byte[] key, byte[] engineId)
         {
            using (SHA256 sha = SHA256.Create())
               return sha.ComputeHash(Ber.Concat(key, engineId, key));
         }

         private static byte[] Digest(byte[] key, byte[] message)
         {
            using (var hmac = new HMACSHA256(key))
               return hmac.ComputeHash(message).Take(24).ToArray();
         }

         // CFB with a 128-bit feedback, made from the AES block itself so that
         // nothing about the framework's own CFB (and its padding) is assumed:
         // each keystream block is AES of the previous ciphertext block, the IV
         // first.
         private static byte[] AesCfb(byte[] key, byte[] iv, byte[] input, bool encrypt)
         {
            using (Aes aes = Aes.Create())
            {
               aes.Mode = CipherMode.ECB;
               aes.Padding = PaddingMode.None;
               aes.Key = key;

               using (ICryptoTransform block = aes.CreateEncryptor())
               {
                  var output = new byte[input.Length];
                  var feedback = (byte[]) iv.Clone();
                  var keystream = new byte[16];

                  for (int offset = 0; offset < input.Length; offset += 16)
                  {
                     block.TransformBlock(feedback, 0, 16, keystream, 0);
                     int count = Math.Min(16, input.Length - offset);

                     for (int i = 0; i < count; i++)
                        output[offset + i] = (byte) (input[offset + i] ^ keystream[i]);

                     if (count == 16)
                        Array.Copy(encrypt ? output : input, offset, feedback, 0, 16);
                  }

                  return output;
               }
            }
         }
      }

      /// <summary>The BER this file speaks: definite lengths, single-octet tags.</summary>
      private static class Ber
      {
         public sealed class Node
         {
            public byte Tag;
            public int ValueOffset;
            public int Length;
         }

         public static byte[] Concat(params byte[][] parts)
         {
            return parts.SelectMany(part => part).ToArray();
         }

         public static byte[] Tlv(byte tag, byte[] content)
         {
            var output = new List<byte> { tag };

            if (content.Length < 0x80)
            {
               output.Add((byte) content.Length);
            }
            else
            {
               var octets = new List<byte>();
               for (int length = content.Length; length > 0; length >>= 8)
                  octets.Insert(0, (byte) (length & 0xFF));

               output.Add((byte) (0x80 | octets.Count));
               output.AddRange(octets);
            }

            output.AddRange(content);
            return output.ToArray();
         }

         public static byte[] Sequence(params byte[][] items)
         {
            return Tlv(0x30, Concat(items));
         }

         public static byte[] Octets(byte[] value)
         {
            return Tlv(0x04, value);
         }

         public static byte[] Integer(long value)
         {
            var octets = new List<byte>();
            for (int i = 7; i >= 0; i--)
               octets.Add((byte) ((value >> (8 * i)) & 0xFF));

            while (octets.Count > 1 &&
                   ((octets[0] == 0x00 && (octets[1] & 0x80) == 0) || (octets[0] == 0xFF && (octets[1] & 0x80) != 0)))
               octets.RemoveAt(0);

            return Tlv(0x02, octets.ToArray());
         }

         public static byte[] Oid(string dotted)
         {
            ulong[] arcs = dotted.Split('.').Select(ulong.Parse).ToArray();
            var content = new List<byte>();

            AppendArc(content, arcs[0] * 40UL + arcs[1]);
            for (int i = 2; i < arcs.Length; i++)
               AppendArc(content, arcs[i]);

            return Tlv(0x06, content.ToArray());
         }

         private static void AppendArc(List<byte> output, ulong value)
         {
            var groups = new Stack<byte>();
            groups.Push((byte) (value & 0x7F));
            value >>= 7;

            while (value > 0)
            {
               groups.Push((byte) (0x80 | (value & 0x7F)));
               value >>= 7;
            }

            output.AddRange(groups);
         }

         public static string OidToString(byte[] buffer, int offset, int length)
         {
            var arcs = new List<ulong>();
            ulong current = 0;
            bool first = true;

            for (int i = offset; i < offset + length; i++)
            {
               current = (current << 7) | (ulong) (buffer[i] & 0x7F);

               if ((buffer[i] & 0x80) != 0)
                  continue;

               if (first)
               {
                  ulong top = current < 40 ? 0UL : current < 80 ? 1UL : 2UL;
                  arcs.Add(top);
                  arcs.Add(current - top * 40UL);
                  first = false;
               }
               else
               {
                  arcs.Add(current);
               }

               current = 0;
            }

            return string.Join(".", arcs);
         }

         public static long ReadInteger(byte[] buffer, Node node)
         {
            long value = (buffer[node.ValueOffset] & 0x80) != 0 ? -1L : 0L;
            for (int i = 0; i < node.Length; i++)
               value = (value << 8) | buffer[node.ValueOffset + i];
            return value;
         }

         public static ulong ReadUnsigned(byte[] value)
         {
            ulong result = 0;
            foreach (byte octet in value)
               result = (result << 8) | octet;
            return result;
         }

         // The elements one after another in [offset, offset + length).
         public static List<Node> Children(byte[] buffer, int offset, int length)
         {
            var nodes = new List<Node>();
            int position = offset;
            int end = offset + length;

            while (position < end)
            {
               Assert.LessOrEqual(position + 2, end, "An element's header runs past its parent.");
               byte tag = buffer[position];
               int first = buffer[position + 1];
               int header = 2;
               int contentLength = first;

               if (first >= 0x80)
               {
                  int count = first & 0x7F;
                  Assert.IsTrue(count >= 1 && count <= 4, "An indefinite or oversized length.");
                  contentLength = 0;
                  for (int i = 0; i < count; i++)
                     contentLength = (contentLength << 8) | buffer[position + 2 + i];
                  header = 2 + count;
               }

               Assert.LessOrEqual(position + header + contentLength, end, "An element runs past its parent.");
               nodes.Add(new Node { Tag = tag, ValueOffset = position + header, Length = contentLength });
               position += header + contentLength;
            }

            return nodes;
         }
      }
   }
}
