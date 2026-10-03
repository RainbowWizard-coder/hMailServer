// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Linq;
using System.Security.Cryptography;
using System.Text.RegularExpressions;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.SMTP
{
   /// <summary>
   ///    DANE against lookups that do not come back whole (RFC 7672 section 2.1.1): a
   ///    domain in a signed zone whose MX host's TLSA lookup answers SERVFAIL, answers
   ///    NOERROR with nothing and no proof that nothing is there, or answers records
   ///    whose signatures were taken off, and a domain whose MX answer came without
   ///    its signatures. Each is deferred, and nothing reaches the host - which here
   ///    offers no STARTTLS, so a message that got there went in the clear. Until
   ///    6.3.6 each of those lookups read as "not signed", and the message went.
   ///    <para />
   ///    And what must still go: a TLSA lookup the zone proves empty, by an NSEC
   ///    (NODATA) or an NSEC3 chain (NXDOMAIN), is a host with no TLSA records; a
   ///    domain under a top-level name the suite's root proves unsigned is an
   ///    ordinary delivery; a zone whose DS names only an algorithm the resolver does
   ///    not follow is unsigned (RFC 4035 section 5.2), where until 6.3.6 it was bogus
   ///    and its mail held. A host whose validated TLSA records are all of a usage
   ///    this server does not use requires TLS without authentication - but not when
   ///    this server's own outbound security is STARTTLS (required) with the
   ///    certificate checked, where until the fix the certificate was forgiven.
   ///    <para />
   ///    The chain is the suite's (SuiteDns.Root, the server's trust anchor for the
   ///    run), a top-level name of the fixture's own signed beneath it, and one child
   ///    zone per test, each with its own name because the resolver remembers a
   ///    zone's keys for an hour. The MX host is 127.0.0.1, where the suite's SMTP
   ///    simulator listens on 25 (an MX host is reached on 25); where the server
   ///    itself listens on 25 (the Windows bench) its SMTP port moves aside for the
   ///    test. The suite runs at no retries, so a deferral comes back as a bounce
   ///    that says "Tried 1 time(s)".
   /// </summary>
   [TestFixture]
   public class DaneIndeterminateLookups : TestFixtureBase
   {
      private const uint Ttl = 60;

      // The fixture's top-level name, fresh for each run: the resolver keeps a
      // zone's keys for an hour, and a server that outlives one run must not meet
      // another run's keys under the same name.
      private readonly string _tldName = "dane-" + Guid.NewGuid().ToString("N").Substring(0, 8);
      private DnssecZone _tld;

      [OneTimeSetUp]
      public void SignTheTopLevelName()
      {
         _tld = new DnssecZone(_tldName);
         PublishTld();
      }

      [OneTimeTearDown]
      public void DropTheTopLevelName()
      {
         // The zone as the suite seeds it: the next fixture meets none of this one's names.
         SuiteDns.Reset();
         _tld.Dispose();
      }

      [TearDown]
      public void ForgetTheZones()
      {
         SuiteDns.Reset();
         PublishTld();
      }

      [Test]
      [Description("A signed zone's MX host whose TLSA lookup answers SERVFAIL is not used, and the message is deferred: RFC 7672 section 2.1.1 has a failed TLSA lookup treated as an unreachable host. Until 6.3.6 the failure read as no TLSA records and the message went in the clear to a host without STARTTLS.")]
      public void ATlsaLookupAnsweringServfailIsDeferred()
      {
         using (DnssecZone child = Child("servfail"))
         {
            SuiteDns.Zone.WithServFail(TlsaName(child), DnsWire.TypeTlsa);
            AssertDeferredUnsent(child, "TLSA records");
         }
      }

      [Test]
      [Description("A signed zone's MX host whose TLSA lookup answers NOERROR with nothing, and no NSEC or NSEC3 proof that nothing is there, is not used, and the message is deferred. Until 6.3.6 such an answer - which anybody on the way can make - was no TLSA records.")]
      public void AnEmptyTlsaAnswerWithoutAProofIsDeferred()
      {
         using (DnssecZone child = Child("empty"))
            AssertDeferredUnsent(child, "TLSA records");
      }

      [Test]
      [Description("A signed zone's MX host whose TLSA records arrive with their signatures taken off is not used, and the message is deferred. Until 6.3.6 records without a signature were read as unsigned, and the message went in the clear.")]
      public void TlsaRecordsWithoutTheirSignaturesAreDeferred()
      {
         using (DnssecZone child = Child("stripped"))
         {
            SuiteDns.Zone.WithRaw(TlsaName(child), DnsWire.TypeTlsa, DaneEeRecord());
            AssertDeferredUnsent(child, "TLSA records");
         }
      }

      [Test]
      [Description("A signed domain whose MX records arrive without their signatures is deferred, and no host it names is tried: the lookup is indeterminate. Until 6.3.6 it was unsigned, and the message went in the clear to whatever host the answer named.")]
      public void MxRecordsWithoutTheirSignaturesAreDeferred()
      {
         using (DnssecZone child = Child("mxstripped", signMx: false))
            AssertDeferredUnsent(child, "MX records");
      }

      [Test]
      [Description("The control: a signed zone that proves with an NSEC that its MX host has no TLSA records (NODATA) is delivered to as a host without DANE.")]
      public void ATlsaLookupProvenEmptyByAnNsecIsDelivered()
      {
         using (DnssecZone child = Child("nodata"))
         {
            string name = TlsaName(child);
            byte[] nsec = DnsWire.Nsec("zz." + name, new[] { DnsWire.TypeRrsig, DnsWire.TypeNsec });
            SuiteDns.Zone.WithDenial(name, DnsWire.TypeTlsa, name, DnsWire.TypeNsec, nsec)
                         .WithDenial(name, DnsWire.TypeTlsa, name, DnsWire.TypeRrsig, child.Sign(name, DnsWire.TypeNsec, Ttl, new[] { nsec }));

            AssertDelivered(child.Name);
         }
      }

      [Test]
      [Description("The control: a signed zone with an NSEC3 chain that proves its MX host's TLSA name does not exist (NXDOMAIN: the closest encloser, the next closer name and the wildcard) is delivered to as a host without DANE.")]
      public void ATlsaLookupProvenNonexistentByAnNsec3ChainIsDelivered()
      {
         using (DnssecZone child = ChildWithNsec3("nsec3"))
            AssertDelivered(child.Name);
      }

      [Test]
      [Description("The control: a domain under a top-level name the root proves unsigned, which is most of the internet, is an ordinary delivery.")]
      public void ADomainProvenUnsignedIsDelivered()
      {
         string domain = "plain." + _tldName + "-unsigned";
         SuiteDns.Zone.WithMx(domain, 10, "mail." + domain)
                      .WithA("mail." + domain, "127.0.0.1");

         AssertDelivered(domain);
      }

      [Test]
      [Description("The control: a domain under a top-level name the root proves it does not have - a private name, as company.local is - lies outside the signed tree and is an ordinary delivery. The proof is the root's NXDOMAIN for the top-level name's DS, with the NSEC covering the name and the one covering the root's wildcard.")]
      public void ADomainUnderATopLevelNameTheRootDoesNotHaveIsDelivered()
      {
         string tld = "zz" + _tldName.Replace("-", "");
         string domain = "corp." + tld;

         // The root's own NSECs: its apex (covering "*", so no wildcard answers), and one
         // covering the top-level name, both signed by the suite's root.
         byte[] apex = DnsWire.Nsec("aaa", new[] { DnsWire.TypeNs, DnsWire.TypeSoa, DnsWire.TypeRrsig, DnsWire.TypeNsec, DnsWire.TypeDnskey });
         byte[] cover = DnsWire.Nsec("zzzzzzzzzzzzzzzzzzzzzzzz", new[] { DnsWire.TypeNs, DnsWire.TypeRrsig, DnsWire.TypeNsec });
         SuiteDns.Zone.WithNxDomain(tld)
                      .WithDenial(tld, DnsWire.TypeDs, "", DnsWire.TypeNsec, apex)
                      .WithDenial(tld, DnsWire.TypeDs, "", DnsWire.TypeRrsig, SuiteDns.Root.Sign("", DnsWire.TypeNsec, Ttl, new[] { apex }))
                      .WithDenial(tld, DnsWire.TypeDs, "zy", DnsWire.TypeNsec, cover)
                      .WithDenial(tld, DnsWire.TypeDs, "zy", DnsWire.TypeRrsig, SuiteDns.Root.Sign("zy", DnsWire.TypeNsec, Ttl, new[] { cover }))
                      .WithMx(domain, 10, "mail." + domain)
                      .WithA("mail." + domain, "127.0.0.1");

         AssertDelivered(domain);
      }

      [Test]
      [Description("A zone whose validated DS record names only an algorithm the resolver does not verify (RSA/SHA-1, 7) is unsigned, as RFC 4035 section 5.2 says, and its mail is delivered. Until 6.3.6 it was bogus, and every message to it was deferred until it bounced.")]
      public void AZoneSignedWithAnAlgorithmTheResolverDoesNotFollowIsDelivered()
      {
         string child = "sha1." + _tldName;
         string host = "mail." + child;

         // A DNSKEY of algorithm 7, and the DS of it the parent signs.
         byte[] key = new byte[] { 0x01, 0x01, 3, 7 }.Concat(RandomBytes(132)).ToArray();
         ushort keyTag = DnssecZone.ComputeKeyTag(key);
         byte[] digest;
         using (SHA256 sha256 = SHA256.Create())
            digest = sha256.ComputeHash(DnsWire.Name(child).Concat(key).ToArray());
         byte[] ds = new[] { (byte) (keyTag >> 8), (byte) (keyTag & 0xFF), (byte) 7, (byte) 2 }.Concat(digest).ToArray();

         // The MX RRset, signed by that key: a signature this resolver cannot check.
         byte[] mx = DnsWire.Mx(10, host);
         byte[] rrsig = Rrsig(DnsWire.TypeMx, 7, child, keyTag, child, RandomBytes(128));

         SuiteDns.Zone.WithRaw(child, DnsWire.TypeDs, ds)
                      .WithRrsig(child, DnsWire.TypeDs, _tld.Sign(child, DnsWire.TypeDs, Ttl, new[] { ds }))
                      .WithRaw(child, DnsWire.TypeDnskey, key)
                      .WithRaw(child, DnsWire.TypeMx, mx)
                      .WithRrsig(child, DnsWire.TypeMx, rrsig)
                      .WithA(host, "127.0.0.1");

         AssertDelivered(child);
      }

      [Test]
      [Description("A host whose validated TLSA records are all of a usage this server does not use (PKIX-EE, 1 1 1), delivered to with this server's own outbound security at STARTTLS (required) and the remote certificate checked, presents a certificate for another name: the delivery is deferred and nothing is sent. The records ask for TLS without authentication; they do not take away the check the administrator asked for, which until the fix they did.")]
      public void UnusableTlsaRecordsKeepTheCertificateCheckOfARequiredStartTls()
      {
         using (DnssecZone child = Child("pkix"))
         {
            PublishSignedTlsa(child, DnsWire.Tlsa(1, 1, 1, RandomBytes(32)));

            eConnectionSecurity before = _settings.SMTPConnectionSecurity;
            try
            {
               _settings.SMTPConnectionSecurity = eConnectionSecurity.eCSSTARTTLSRequired;
               _settings.VerifyRemoteSslCertificate = true;

               Account sender;
               SmtpServerSimulator host;
               Deliver(child.Name, eConnectionSecurity.eCSSTARTTLSRequired, false, out sender, out host);

               Assert.IsEmpty(host.MailFromCommand,
                  "MAIL FROM was sent to a host whose certificate names another host, under STARTTLS (required) with the certificate checked.");
               StringAssert.Contains("Tried 1 time(s)", Bounce(sender), "The refusal was not a deferral.");
            }
            finally
            {
               _settings.SMTPConnectionSecurity = before;
               _settings.VerifyRemoteSslCertificate = false;
            }
         }
      }

      [Test]
      [Description("The control: the same host with this server's outbound security at opportunistic STARTTLS is delivered to over TLS without its certificate being judged, as RFC 7672 section 2.2 has a validated RRset of no usable record require.")]
      public void UnusableTlsaRecordsStillWaiveTheCheckOfOpportunisticStartTls()
      {
         using (DnssecZone child = Child("pkixoptional"))
         {
            PublishSignedTlsa(child, DnsWire.Tlsa(1, 1, 1, RandomBytes(32)));

            eConnectionSecurity before = _settings.SMTPConnectionSecurity;
            try
            {
               _settings.SMTPConnectionSecurity = eConnectionSecurity.eCSSTARTTLSOptional;
               _settings.VerifyRemoteSslCertificate = true;

               Account sender;
               SmtpServerSimulator host;
               Deliver(child.Name, eConnectionSecurity.eCSSTARTTLSRequired, true, out sender, out host);

               StringAssert.Contains("Must travel", host.MessageData, "The message did not arrive.");
            }
            finally
            {
               _settings.SMTPConnectionSecurity = before;
               _settings.VerifyRemoteSslCertificate = false;
            }
         }
      }

      private void PublishTld()
      {
         SuiteDns.Zone.WithRaw(_tld.Name, DnsWire.TypeDs, _tld.DsRdata)
                      .WithRrsig(_tld.Name, DnsWire.TypeDs, SuiteDns.Root.Sign(_tld.Name, DnsWire.TypeDs, Ttl, new[] { _tld.DsRdata }));
         PublishKeys(_tld);
      }

      private static void PublishKeys(DnssecZone signer)
      {
         SuiteDns.Zone.WithRaw(signer.Name, DnsWire.TypeDnskey, signer.DnskeyRdata)
                      .WithRrsig(signer.Name, DnsWire.TypeDnskey, signer.Sign(signer.Name, DnsWire.TypeDnskey, Ttl, new[] { signer.DnskeyRdata }));
      }

      private static string MailHost(DnssecZone child)
      {
         return "mail." + child.Name;
      }

      private static string TlsaName(DnssecZone child)
      {
         return "_25._tcp." + MailHost(child);
      }

      private static byte[] DaneEeRecord()
      {
         return DnsWire.Tlsa(3, 1, 1, RandomBytes(32));
      }

      private static void PublishSignedTlsa(DnssecZone child, byte[] tlsa)
      {
         SuiteDns.Zone.WithRaw(TlsaName(child), DnsWire.TypeTlsa, tlsa)
                      .WithRrsig(TlsaName(child), DnsWire.TypeTlsa, child.Sign(TlsaName(child), DnsWire.TypeTlsa, Ttl, new[] { tlsa }));
      }

      /// <summary>
      ///    A zone under the fixture's top-level name: its DS there, its keys, an MX
      ///    RRset naming mail.&lt;zone&gt; (signed unless signMx is false), that host's
      ///    address, and the zone's answers to the DS queries the resolver's walk from
      ///    the root asks on the way to the host's TLSA name - that mail.&lt;zone&gt;,
      ///    _tcp.mail.&lt;zone&gt; and _25._tcp.mail.&lt;zone&gt; are no zone cuts.
      /// </summary>
      private DnssecZone Child(string label, bool signMx = true)
      {
         var child = new DnssecZone(label + "." + _tldName);
         FakeDnsServer zone = SuiteDns.Zone;

         zone.WithRaw(child.Name, DnsWire.TypeDs, child.DsRdata)
             .WithRrsig(child.Name, DnsWire.TypeDs, _tld.Sign(child.Name, DnsWire.TypeDs, Ttl, new[] { child.DsRdata }));
         PublishKeys(child);

         byte[] mx = DnsWire.Mx(10, MailHost(child));
         zone.WithRaw(child.Name, DnsWire.TypeMx, mx);
         if (signMx)
            zone.WithRrsig(child.Name, DnsWire.TypeMx, child.Sign(child.Name, DnsWire.TypeMx, Ttl, new[] { mx }));
         zone.WithA(MailHost(child), "127.0.0.1");

         NotACut(child, MailHost(child), DnsWire.TypeA);
         NotACut(child, "_tcp." + MailHost(child));
         NotACut(child, TlsaName(child));
         return child;
      }

      /// <summary>An NSEC at name, signed by the zone, listing the types given and neither NS nor DS: its answer to the DS query for name.</summary>
      private static void NotACut(DnssecZone signer, string name, params int[] types)
      {
         var listed = new List<int>(types) { DnsWire.TypeRrsig, DnsWire.TypeNsec };
         byte[] nsec = DnsWire.Nsec("zz." + name, listed);
         SuiteDns.Zone.WithDenial(name, DnsWire.TypeDs, name, DnsWire.TypeNsec, nsec)
                      .WithDenial(name, DnsWire.TypeDs, name, DnsWire.TypeRrsig, signer.Sign(name, DnsWire.TypeNsec, Ttl, new[] { nsec }));
      }

      /// <summary>
      ///    A zone as Child makes one, denying with NSEC3 instead (SHA-1, two
      ///    iterations, a salt): the chain holds its apex and mail.&lt;zone&gt;, so
      ///    _tcp.mail.&lt;zone&gt; and the TLSA name under it do not exist, and their
      ///    lookups answer NXDOMAIN with the chain.
      /// </summary>
      private DnssecZone ChildWithNsec3(string label)
      {
         var child = new DnssecZone(label + "." + _tldName);
         FakeDnsServer zone = SuiteDns.Zone;

         zone.WithRaw(child.Name, DnsWire.TypeDs, child.DsRdata)
             .WithRrsig(child.Name, DnsWire.TypeDs, _tld.Sign(child.Name, DnsWire.TypeDs, Ttl, new[] { child.DsRdata }));
         PublishKeys(child);

         byte[] mx = DnsWire.Mx(10, MailHost(child));
         zone.WithRaw(child.Name, DnsWire.TypeMx, mx)
             .WithRrsig(child.Name, DnsWire.TypeMx, child.Sign(child.Name, DnsWire.TypeMx, Ttl, new[] { mx }))
             .WithA(MailHost(child), "127.0.0.1");

         // The chain: the apex and the host, each pointing at the other.
         byte[] salt = { 0xAB, 0xCD };
         const ushort iterations = 2;
         byte[] apexHash = DnsWire.Nsec3Hash(child.Name, salt, iterations);
         byte[] hostHash = DnsWire.Nsec3Hash(MailHost(child), salt, iterations);
         var records = new List<KeyValuePair<string, byte[]>>
         {
            Nsec3Record(child, apexHash, hostHash, salt, iterations,
               new[] { DnsWire.TypeNs, DnsWire.TypeSoa, DnsWire.TypeMx, DnsWire.TypeRrsig, DnsWire.TypeDnskey, 51 }),
            Nsec3Record(child, hostHash, apexHash, salt, iterations, new[] { DnsWire.TypeA, DnsWire.TypeRrsig })
         };

         string tcp = "_tcp." + MailHost(child);
         zone.WithNxDomain(tcp).WithNxDomain(TlsaName(child));

         foreach (var record in records)
         {
            byte[] rrsig = child.Sign(record.Key, DnsWire.TypeNsec3, Ttl, new[] { record.Value });
            foreach (var (queried, type) in new[] { (MailHost(child), DnsWire.TypeDs), (tcp, DnsWire.TypeDs), (TlsaName(child), DnsWire.TypeDs), (TlsaName(child), DnsWire.TypeTlsa) })
            {
               zone.WithDenial(queried, type, record.Key, DnsWire.TypeNsec3, record.Value)
                   .WithDenial(queried, type, record.Key, DnsWire.TypeRrsig, rrsig);
            }
         }

         return child;
      }

      private static KeyValuePair<string, byte[]> Nsec3Record(DnssecZone zone, byte[] ownerHash, byte[] nextHash, byte[] salt, ushort iterations, int[] types)
      {
         string owner = DnsWire.Base32Hex(ownerHash).ToLowerInvariant() + "." + zone.Name;
         return new KeyValuePair<string, byte[]>(owner, DnsWire.Nsec3(false, iterations, salt, nextHash, types));
      }

      /// <summary>An RRSIG's rdata, by hand: for a signature of an algorithm DnssecZone does not sign with.</summary>
      private static byte[] Rrsig(int coveredType, byte algorithm, string owner, ushort keyTag, string signer, byte[] signature)
      {
         uint inception = (uint) (DateTime.UtcNow.AddHours(-1) - new DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind.Utc)).TotalSeconds;
         uint expiration = inception + 2 * 86400;
         var rdata = new List<byte>
         {
            (byte) (coveredType >> 8), (byte) (coveredType & 0xFF), algorithm, (byte) DnsWire.LabelCount(owner),
            0, 0, 0, (byte) Ttl,
            (byte) (expiration >> 24), (byte) (expiration >> 16), (byte) (expiration >> 8), (byte) expiration,
            (byte) (inception >> 24), (byte) (inception >> 16), (byte) (inception >> 8), (byte) inception,
            (byte) (keyTag >> 8), (byte) (keyTag & 0xFF)
         };
         rdata.AddRange(DnsWire.Name(signer));
         rdata.AddRange(signature);
         return rdata.ToArray();
      }

      private static byte[] RandomBytes(int count)
      {
         var bytes = new byte[count];
         using (var random = RandomNumberGenerator.Create())
            random.GetBytes(bytes);
         return bytes;
      }

      // records: "TLSA records" or "MX records", the lookup the deferral names.
      private void AssertDeferredUnsent(DnssecZone child, string records)
      {
         Account sender;
         SmtpServerSimulator host;
         Deliver(child.Name, eConnectionSecurity.eCSNone, false, out sender, out host);

         Assert.IsEmpty(host.MailFromCommand, "MAIL FROM was sent to the host, which offers no STARTTLS: the message went in the clear.");
         ClassicAssert.AreEqual(0, host.NumberOfConnectedClients,
            "The host was connected to: a lookup that could not tell whether DANE applies must leave it unused (RFC 7672 section 2.1.1).");

         string bounce = Bounce(sender);
         StringAssert.Contains("Tried 1 time(s)", bounce, "The refusal was not a deferral.");
         StringAssert.Contains(records, bounce, "The deferral does not name the lookup.");
         StringAssert.Contains(child.Name, bounce, "The deferral does not name the domain.");
         StringAssert.Contains("could not be completed", bounce, "The deferral does not say why.");
         StringAssert.Contains("has NOT been sent", bounce, "The deferral does not say the message was withheld.");
      }

      private static string Bounce(Account sender)
      {
         return Decoded(Pop3ClientSimulator.AssertGetFirstMessageText(sender.Address, "test"));
      }

      private void AssertDelivered(string domain)
      {
         Account sender;
         SmtpServerSimulator host;
         Deliver(domain, eConnectionSecurity.eCSNone, true, out sender, out host);

         StringAssert.Contains("Must travel", host.MessageData, "The message did not arrive.");
      }

      /// <summary>
      ///    Sends a message to someone@domain through the server, whose MX host is the
      ///    simulator on 25 with the security given, and waits until the queue is empty
      ///    - and, when expectDelivery is true, until the simulator has the message.
      /// </summary>
      private void Deliver(string domain, eConnectionSecurity hostSecurity, bool expectDelivery, out Account sender, out SmtpServerSimulator host)
      {
         sender = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "dane-" + Guid.NewGuid().ToString("N").Substring(0, 6) + "@example.test", "test");

         // An MX host is always reached on 25, and this server will not deliver to a
         // port it listens on itself: where it listens on 25, that port moves aside.
         int submissionPort = TestPorts.Smtp;
         TCPIPPort moved = null;
         if (TestPorts.Smtp == 25)
         {
            submissionPort = TestSetup.GetNextFreePort();
            TCPIPPorts ports = _settings.TCPIPPorts;
            for (int i = 0; i < ports.Count; i++)
            {
               TCPIPPort port = ports[i];
               if (port.PortNumber == 25 && port.Protocol == eSessionType.eSTSMTP)
                  moved = port;
            }
            Assert.IsNotNull(moved, "No SMTP port 25 to move aside.");
            moved.PortNumber = submissionPort;
            moved.Save();
            _application.Stop();
            _application.Start();
         }

         host = new SmtpServerSimulator(1, 25, hostSecurity);
         try
         {
            if (hostSecurity != eConnectionSecurity.eCSNone)
               host.SetCertificate(RegressionTests.SSL.SslSetup.GetCertificate());
            host.AddRecipientResult(new Dictionary<string, int> { ["someone@" + domain] = 250 });
            host.StartListen();

            new SmtpClientSimulator(false, submissionPort).Send(sender.Address, "someone@" + domain, "DANE", "Must travel only as DANE allows.");

            CustomAsserts.AssertRecipientsInDeliveryQueue(0);

            if (expectDelivery)
               host.WaitForCompletion();
         }
         finally
         {
            host.Dispose();
            if (moved != null)
            {
               moved.PortNumber = 25;
               moved.Save();
               _application.Stop();
               _application.Start();
            }
         }
      }

      // A bounce's text as written, its quoted-printable soft breaks and escapes undone.
      private static string Decoded(string raw)
      {
         string joined = raw.Replace("=\r\n", "").Replace("=\n", "");
         return Regex.Replace(joined, "=([0-9A-F]{2})", match => ((char) Convert.ToInt32(match.Groups[1].Value, 16)).ToString());
      }
   }
}
