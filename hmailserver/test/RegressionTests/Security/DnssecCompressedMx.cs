// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.Security
{
   /// <summary>
   ///    Work item 80: the validating resolver's verdict on a signed MX RRset whose
   ///    exchange arrives compressed, as every public resolver sends one - the
   ///    suite's DNS server writes an MX record's exchange as a pointer to the
   ///    question wherever their last labels agree (FakeDnsServer.CompressExchange_).
   ///    The RRSIG signs the exchange written out in full and in lowercase (RFC 4034
   ///    section 6.2), and until the fix the resolver checked it over the octets as
   ///    they arrived: every such RRset was bogus (2), and with DANE enforcement on
   ///    no host of the domain was tried. live.com's exchange,
   ///    live-com.olc.protection.outlook.com, shares nothing with the question but
   ///    its top-level name, and was compressed all the same.
   ///    <para />
   ///    The chain is the suite's (SuiteDns.Root, the server's trust anchor for the
   ///    run), a top-level name of the fixture's own signed beneath it, and one child
   ///    zone per test, each with its own name because the resolver remembers a
   ///    zone's keys for an hour. Windows only: Diagnostics.DnssecChainStatus is a
   ///    COM call. DaneIndeterminateLookups delivers through the same shape on both
   ///    platforms.
   /// </summary>
   [TestFixture]
   public class DnssecCompressedMx : TestFixtureBase
   {
      private const int Secure = 0;
      private const int Bogus = 2;
      private const uint Ttl = 60;

      private readonly string _tldName = "wi80-" + Guid.NewGuid().ToString("N").Substring(0, 8);
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
      [Description("Work item 80: a signed MX RRset whose exchange arrives compressed against the question - mail.<domain> as one label and a pointer to the domain - is secure (0). Until the fix it was bogus (2).")]
      public void AnExchangeCompressedAgainstTheQuestionIsSecure()
      {
         using (DnssecZone child = Child("compressed"))
         {
            byte[] mx = DnsWire.Mx(10, "mail." + child.Name);
            Publish(child, mx, mx);

            ClassicAssert.AreEqual(Secure, _application.Diagnostics.DnssecChainStatus(child.Name, "MX"),
               "A signed MX RRset whose exchange arrived compressed was not secure (2 is bogus).");
         }
      }

      [Test]
      [Description("Work item 80, live.com's shape: an exchange in another zone that shares only the top-level name with the domain arrives as its own labels and a pointer to the question's last label, and the RRset is secure (0).")]
      public void AnExchangeSharingOnlyTheTopLevelNameIsSecure()
      {
         using (DnssecZone child = Child("live"))
         {
            byte[] mx = DnsWire.Mx(2, "live-" + child.Name.Split('.')[0] + ".olc.protection.outlook." + _tldName);
            Publish(child, mx, mx);

            ClassicAssert.AreEqual(Secure, _application.Diagnostics.DnssecChainStatus(child.Name, "MX"),
               "A signed MX RRset whose exchange shares only the top-level name, compressed against it, was not secure.");
         }
      }

      [Test]
      [Description("An exchange the zone spells in mixed case is signed in lowercase (RFC 4034 section 6.2) and arrives as spelled, and compressed: the RRset is secure (0).")]
      public void AnExchangeInMixedCaseIsSecure()
      {
         using (DnssecZone child = Child("mixedcase"))
         {
            byte[] signedForm = DnsWire.Mx(10, "mail." + child.Name);
            byte[] served = new byte[] { 0, 10 }.Concat(NameAsSpelled("MaIl." + child.Name.ToUpperInvariant())).ToArray();
            Publish(child, served, signedForm);

            ClassicAssert.AreEqual(Secure, _application.Diagnostics.DnssecChainStatus(child.Name, "MX"),
               "A signed MX RRset whose exchange arrived in mixed case was not secure.");
         }
      }

      [Test]
      [Description("The control: an exchange other than the one the zone signed is bogus (2), compressed or not, and the debug log says which RRset, which key tag and which check failed.")]
      public void AnExchangeChangedOnTheWayIsBogusAndTheLogSaysWhy()
      {
         using (DnssecZone child = Child("changed"))
         {
            Publish(child, DnsWire.Mx(10, "mail." + child.Name), DnsWire.Mx(10, "mx." + child.Name));

            ClassicAssert.AreEqual(Bogus, _application.Diagnostics.DnssecChainStatus(child.Name, "MX"),
               "An MX RRset whose exchange is not the one signed was not bogus.");

            string expected = "DNSSEC: the MX lookup for " + child.Name + " is bogus: the MX RRset of " + child.Name +
                              " does not validate: the RRSIG by " + child.Name + " with key tag " + child.KeyTag +
                              " (algorithm 13) does not match the records as received";

            // The log, which may be written a moment after the call returns.
            string log = string.Empty;
            for (int attempt = 0; attempt < 25 && !log.Contains(expected); attempt++)
            {
               if (attempt > 0)
                  Thread.Sleep(200);
               log = LogHandler.ReadCurrentDefaultLog();
            }

            StringAssert.Contains(expected, log, "The debug log does not say which RRset, key tag and check failed.");
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

      /// <summary>A zone under the fixture's top-level name: its DS there, signed, and its keys.</summary>
      private DnssecZone Child(string label)
      {
         var child = new DnssecZone(label + "." + _tldName);
         SuiteDns.Zone.WithRaw(child.Name, DnsWire.TypeDs, child.DsRdata)
                      .WithRrsig(child.Name, DnsWire.TypeDs, _tld.Sign(child.Name, DnsWire.TypeDs, Ttl, new[] { child.DsRdata }));
         PublishKeys(child);
         return child;
      }

      /// <summary>The MX record the zone serves, and the RRSIG the zone made over the form it signed.</summary>
      private static void Publish(DnssecZone child, byte[] served, byte[] signedForm)
      {
         SuiteDns.Zone.WithRaw(child.Name, DnsWire.TypeMx, served)
                      .WithRrsig(child.Name, DnsWire.TypeMx, child.Sign(child.Name, DnsWire.TypeMx, Ttl, new[] { signedForm }));
      }

      /// <summary>A name in wire form with its letters as given - DnsWire.Name lowercases them.</summary>
      private static byte[] NameAsSpelled(string name)
      {
         var wire = new List<byte>();
         foreach (string label in name.TrimEnd('.').Split('.'))
         {
            byte[] bytes = Encoding.ASCII.GetBytes(label);
            wire.Add((byte) bytes.Length);
            wire.AddRange(bytes);
         }
         wire.Add(0);
         return wire.ToArray();
      }
   }
}
