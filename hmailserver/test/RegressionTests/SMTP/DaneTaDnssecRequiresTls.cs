// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Text.RegularExpressions;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.SMTP
{
   /// <summary>
   ///    A recipient domain whose MX RRset is DNSSEC-validated, and whose MX host
   ///    publishes a DNSSEC-validated TLSA RRset of DANE-TA records only - "2 1 1",
   ///    the trust anchor's public key by SHA-256, the common shape - and offers no
   ///    STARTTLS.
   ///
   ///    Until 6.3.6 the usable-record filter kept DANE-EE (usage 3) alone and threw
   ///    every usage-2 record away, and a validated RRset none of whose records was
   ///    kept was taken for no records at all: TLS was not required, and an attacker
   ///    who took the STARTTLS offer away had the message in the clear (RFC 7672
   ///    sections 2.2 and 3.1.3 have DANE-TA for SMTP). Now DANE-TA records are
   ///    used, through OpenSSL's own DANE, and they require TLS to the host: the
   ///    delivery is deferred, naming the records, and is a TLS-RPT failure.
   ///
   ///    DaneRequiredTlsDeferral proves the deferral with DNSSEC validation off and
   ///    a DANE-EE record taken as published, and DaneIndeterminateLookups records
   ///    of a usage this server does not use. This fixture is the validated path
   ///    for DANE-TA: the chain is the suite's (SuiteDns.Root, the server's trust
   ///    anchor for the run), a top-level name of the fixture's own signed beneath
   ///    it and the recipient's zone beneath that, and the server says it found
   ///    DNSSEC-validated records. The top-level name is fresh for each run, as
   ///    DaneIndeterminateLookups' is: the resolver keeps a zone's keys for an hour,
   ///    and a server that outlives one run must not meet another run's keys under
   ///    the same name. Nothing here changes the trust anchor or restarts the
   ///    server.
   ///
   ///    The MX names a host at 127.0.0.1 where the suite's SMTP simulator answers
   ///    without offering STARTTLS. An MX host is always reached on 25, and this
   ///    server will not deliver to a port it listens on itself, so where it listens
   ///    on 25 (the Windows bench) its SMTP port moves aside for the test and the
   ///    message is submitted there. The suite runs at no retries, so the deferral
   ///    comes back as a bounce that says "Tried 1 time(s)", which a permanent
   ///    refusal never says.
   /// </summary>
   [TestFixture]
   public class DaneTaDnssecRequiresTls : TestFixtureBase
   {
      private const uint Ttl = 60;

      private readonly string _tldName = "danta-" + Guid.NewGuid().ToString("N").Substring(0, 8);
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
      public void ForgetTheZone()
      {
         SuiteDns.Reset();
         PublishTld();
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

      /// <summary>An NSEC at name, signed by the zone, listing the types given and neither NS nor DS: its answer to the DS query for name, which the resolver's walk to the TLSA name asks.</summary>
      private static void NotACut(DnssecZone signer, string name, params int[] types)
      {
         var listed = new List<int>(types) { DnsWire.TypeRrsig, DnsWire.TypeNsec };
         byte[] nsec = DnsWire.Nsec("zz." + name, listed);
         SuiteDns.Zone.WithDenial(name, DnsWire.TypeDs, name, DnsWire.TypeNsec, nsec)
                      .WithDenial(name, DnsWire.TypeDs, name, DnsWire.TypeRrsig, signer.Sign(name, DnsWire.TypeNsec, Ttl, new[] { nsec }));
      }

      [Test]
      [Description("A host whose DNSSEC-validated TLSA RRset holds DANE-TA (2 1 1) records only, named by a DNSSEC-validated MX RRset, and that offers no STARTTLS, is not sent the message in the clear: the delivery is deferred (retried), naming the records, and is a TLS-RPT failure.")]
      public void AValidatedDaneTaRecordRequiresTlsAndDefersWhenItCannotBeHad()
      {
         // The recipient's zone, signed and delegated under the fixture's top-level
         // name: its key, the DS the top-level name signs for it, its MX RRset
         // (signed), the MX host's address, the proofs that the names on the way to
         // the TLSA record are no zone cuts, and a TLSA record for the host's port
         // 25 - DANE-TA (2), the subject public key (1), SHA-256 (1), 32 octets -
         // signed by the zone.
         using (var domain = new DnssecZone("net2." + _tldName))
         {
            string remoteDomain = domain.Name;
            string mailHost = "mail." + remoteDomain;
            string tlsaOwner = "_25._tcp." + mailHost;
            FakeDnsServer zone = SuiteDns.Zone;

            zone.WithRaw(remoteDomain, DnsWire.TypeDs, domain.DsRdata)
                .WithRrsig(remoteDomain, DnsWire.TypeDs, _tld.Sign(remoteDomain, DnsWire.TypeDs, Ttl, new[] { domain.DsRdata }));
            PublishKeys(domain);

            byte[] mx = DnsWire.Mx(10, mailHost);
            zone.WithRaw(remoteDomain, DnsWire.TypeMx, mx)
                .WithRrsig(remoteDomain, DnsWire.TypeMx, domain.Sign(remoteDomain, DnsWire.TypeMx, Ttl, new[] { mx }));
            zone.WithA(mailHost, "127.0.0.1");

            NotACut(domain, mailHost, DnsWire.TypeA);
            NotACut(domain, "_tcp." + mailHost);
            NotACut(domain, tlsaOwner, DnsWire.TypeTlsa);

            var key = new byte[32];
            for (int i = 0; i < key.Length; i++)
               key[i] = (byte) (0xB0 + i);
            byte[] tlsa = DnsWire.Tlsa(2, 1, 1, key);
            zone.WithRaw(tlsaOwner, DnsWire.TypeTlsa, tlsa)
                .WithRrsig(tlsaOwner, DnsWire.TypeTlsa, domain.Sign(tlsaOwner, DnsWire.TypeTlsa, Ttl, new[] { tlsa }));

            string log;
            string bounce = DeliverToTheHost(remoteDomain, out log);

            // The records were validated, all the way from the trust anchor, and the
            // DANE-TA one was used: not the legacy mode's "(unvalidated)", and not a
            // set none of whose records is usable.
            StringAssert.Contains("DANE: the MX RRset for " + remoteDomain + " is DNSSEC-validated", log,
               "The MX RRset did not validate from the suite's trust anchor, so this test proves nothing about DANE.");
            StringAssert.Contains("DNSSEC-validated DANE TLSA records found for " + mailHost, log,
               "The DANE-TA record was not taken as a usable, DNSSEC-validated TLSA record.");

            StringAssert.Contains("the TLSA records of " + mailHost + " require TLS to it, with a certificate they match", bounce,
               "The failure does not name the DANE-TA records that required TLS.");
            StringAssert.Contains("NOT been sent in the clear", bounce, "The failure does not say the message was withheld.");
            StringAssert.Contains("Tried 1 time(s)", bounce,
               "The refusal was permanent: a host whose TLSA records require TLS and that offers no STARTTLS must be deferred (RFC 7672 section 2.2).");

            StringAssert.Contains("TLS-RPT: a failed session with " + remoteDomain + " recorded: starttls-not-supported at " + mailHost + " (policy tlsa)", log,
               "The session was not recorded as a TLS-RPT failure of type starttls-not-supported.");
            StringAssert.DoesNotContain("TLS-RPT: a successful session with " + remoteDomain, log,
               "A session that had no TLS was counted as a successful one.");
         }
      }

      // A message to the zone's MX host, which offers no STARTTLS, and the bounce
      // that comes back at no retries, with the debug log of the attempt. Asserts
      // that the host was never sent MAIL FROM: the message did not travel in the
      // clear.
      private string DeliverToTheHost(string remoteDomain, out string log)
      {
         Account sender = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "dane-ta-sender@example.test", "test");

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

         var host = new SmtpServerSimulator(1, 25, eConnectionSecurity.eCSNone);
         host.ServerSupportsEhlo = true;
         try
         {
            host.AddRecipientResult(new Dictionary<string, int> { ["someone@" + remoteDomain] = 250 });
            host.StartListen();

            LogHandler.DeleteCurrentDefaultLog();

            new SmtpClientSimulator(false, submissionPort).Send(sender.Address, "someone@" + remoteDomain, "DANE-TA without STARTTLS", "Must not travel in the clear.");

            CustomAsserts.AssertRecipientsInDeliveryQueue(0);

            host.WaitForCompletion();
            Assert.IsEmpty(host.MailFromCommand, "MAIL FROM was sent to a host whose DANE-TA records require TLS of it, over a session without it.");
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

         string bounce = Decoded(Pop3ClientSimulator.AssertGetFirstMessageText(sender.Address, "test"));
         log = LogHandler.ReadCurrentDefaultLog();
         return bounce;
      }

      // A bounce's text as written, its quoted-printable soft breaks and escapes undone.
      private static string Decoded(string raw)
      {
         string joined = raw.Replace("=\r\n", "").Replace("=\n", "");
         return Regex.Replace(joined, "=([0-9A-F]{2})", match => ((char) Convert.ToInt32(match.Groups[1].Value, 16)).ToString());
      }
   }
}
