// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Threading;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.Security
{
   /// <summary>
   ///    The validating resolver when the root's own keys do not validate: the trust
   ///    anchor is a root whose DNSKEY comes back without a signature, as a resolver
   ///    that strips signatures hands it back. Since 6.3.6 an MX or TLSA answer without
   ///    signatures is taken for unsigned only when the name is proven to lie below an
   ///    unsigned delegation, and that proof starts at the root's keys; without them the
   ///    lookup is indeterminate (3) - never insecure (1), which would be the downgrade
   ///    the second review of 6.3.6 found (NET636B-1), and never bogus (2), which would
   ///    report every recipient domain to TLS reporting as failing DNSSEC for a fault
   ///    of this server's own resolver. The application log says why.
   ///    <para />
   ///    Windows only: Diagnostics.DnssecChainStatus is a COM call, and a trust anchor
   ///    takes a restart to replace the root's keys the resolver keeps for an hour.
   /// </summary>
   [TestFixture]
   public class DnssecRootKeysUnverifiable : TestFixtureBase
   {
      private const int Indeterminate = 3;

      private DnssecZone _root;

      [OneTimeSetUp]
      public void AnchorARootWhoseKeysComeUnsigned()
      {
         _root = new DnssecZone("");

         // The root's key with no signature over it. A record the zone holds for the
         // root's DNSKEY wins over the suite's own root (FakeDnsServer.RootSigner).
         SuiteDns.Zone.WithRaw(_root.Name, DnsWire.TypeDnskey, _root.DnskeyRdata);

         ServerIniFile.SetSetting("DnssecTrustAnchors", _root.TrustAnchor);
         RestartServerAndReacquireCom();
      }

      [OneTimeTearDown]
      public void BackToTheSuitesRoot()
      {
         // The suite's zone and its anchor, not none: the rest of the run validates
         // against SuiteDns.Root.
         SuiteDns.Reset();
         ServerIniFile.SetSetting("DnssecTrustAnchors", SuiteDns.TrustAnchor);
         RestartServerAndReacquireCom();
         _root.Dispose();
      }

      [Test]
      [Description("An MX answer without signatures, while the root's keys come back without theirs, is indeterminate (3): neither insecure, which until 6.3.6 every such answer was, nor bogus, which as 6.3.6 was first written reported the recipient domain to TLS reporting for a fault of this server's resolver. The application log names the cause and the setting.")]
      public void AnUnsignedMxAnswerIsIndeterminateWhileTheRootsKeysDoNotValidate()
      {
         string domain = "plain.rootless" + Guid.NewGuid().ToString("N").Substring(0, 8);
         SuiteDns.Zone.WithMx(domain, 10, "mail." + domain)
                      .WithA("mail." + domain, "127.0.0.1");

         int status = _application.Diagnostics.DnssecChainStatus(domain, "MX");
         ClassicAssert.AreEqual(Indeterminate, status,
            "An unsigned MX answer, with the root's keys unverifiable, was not indeterminate (1 insecure, 2 bogus).");

         // The application log, which may be written a moment after the call returns.
         string log = string.Empty;
         for (int attempt = 0; attempt < 25; attempt++)
         {
            log = LogHandler.ReadCurrentDefaultLog();
            if (log.Contains("DNSSEC: the root zone's keys did not validate"))
               break;
            Thread.Sleep(200);
         }

         StringAssert.Contains("DNSSEC: the root zone's keys did not validate", log,
            "The application log does not say that the root's keys did not validate.");
         StringAssert.Contains("DaneEnforcementEnabled=0", log, "The application log does not name the setting.");
      }
   }
}
