// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Linq;
using System.Net;
using System.Net.Sockets;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.AntiSpam
{
   /// <summary>
   ///    What the spam tests make of a DNS lookup that failed rather than answered
   ///    (upstream PR 640).
   ///
   ///    An address lookup asks A and AAAA. Before 6.3.7 it reported success when
   ///    either query succeeded, and an AAAA query answered "no AAAA records" - the
   ///    answer of every IPv4-only host, and of every name a DNS blacklist does not
   ///    list - succeeds. So an A query that failed was hidden behind it, and the
   ///    lookup said "this name has no addresses": the HELO test scored the message
   ///    for a name it could not look up, a DNS blacklist and SURBL read the failure
   ///    as "not listed", and the PTR and MX-record tests scored what they could
   ///    not look up as well. A lookup now fails unless addresses were found or
   ///    every query was answered; a failure is not scored; the DNS blacklists and
   ///    SURBL ask the A record alone - the only record a list answers with (RFC
   ///    5782), so that an AAAA query cannot fail a lookup the list answered - and
   ///    ask a query that failed once more, past the DNS cache; SURBL passes over a
   ///    name it could not look up and asks the rest, where one failure used to end
   ///    the list for the message; and a blacklist says in the log that it could
   ///    not check the client. Delivery and the Control Panel's MX query make the
   ///    same address lookup of a domain's mail servers, and say that it failed.
   ///
   ///    The suite's own zone (SuiteDns) serves every name, so each case is the
   ///    answer it describes - a SERVFAIL, a timeout, NXDOMAIN, NODATA - and not
   ///    whatever a resolver on the internet said that day. NXDOMAIN and NODATA
   ///    stay "no addresses" and are still scored; they are here so that the fix
   ///    is not mistaken for a test that stopped scoring anything. A name that
   ///    does not exist is no alias, and is not asked about as one: the NXDOMAIN
   ///    cases whose CNAME query would fail show that one failed query of a
   ///    question that need not be asked does not make the answer unknown.
   ///
   ///    The HELO, PTR and MX-record tests connect from the machine's own address,
   ///    because the HELO test passes over a client in the loopback range.
   /// </summary>
   [TestFixture]
   public class DnsLookupFailures : TestFixtureBase
   {
      private const string Zone = "dnsfail.test";
      private const string HeloReason = "The host name specified in HELO does not match IP address.";
      private const string PtrReason = "No valid rDNS/PTR record found.";
      private const string SurblZone = "multi.surbl.org";

      private hMailServer.AntiSpam _antiSpam;

      [OneTimeSetUp]
      public void ShortenTheTimeout()
      {
         // On Windows a query that times out waits DNSQueryTimeout, and the DNS
         // blacklists ask one that fails twice: two seconds rather than ten keep
         // the Windows timeout cases short. Read where it is used, so no restart.
         // The Linux resolver does not read it: there a query waits five seconds
         // for each DNS server it asks (DnssecResolver), so the timeout cases of
         // the Linux suite wait that long whatever this says.
         ServerIniFile.SetSetting("DNSQueryTimeout", "2");
      }

      [OneTimeTearDown]
      public void PutTheTimeoutBack()
      {
         ServerIniFile.SetSetting("DNSQueryTimeout", null);
      }

      [SetUp]
      public void MarkAnythingScored()
      {
         _antiSpam = _settings.AntiSpam;
         _antiSpam.SpamMarkThreshold = 1;
         _antiSpam.SpamDeleteThreshold = 1000;
         _antiSpam.AddHeaderReason = true;
         _antiSpam.AddHeaderSpam = true;

         SuiteDns.Zone.ClearQueries();
      }

      [TearDown]
      public void ForgetTheNames()
      {
         SuiteDns.Reset();
      }

      // ------------------------------------------------------------------ HELO

      [Test]
      [Description("A HELO host whose A query fails and whose AAAA query answers NODATA - every IPv4-only host's answer - is not scored: the lookup failed.")]
      public void HeloHostWhoseAQueryFailsIsNotScored()
      {
         string host = "helo-a-servfail." + Zone;
         SuiteDns.Zone.WithServFail(host, FakeDnsServer.TypeA);

         ArmHelo_();
         string message = SendWithHelo_(host, "helo-a-servfail");

         AssertNotScored_(message, HeloReason, "a HELO host whose A query failed beside an AAAA query answered NODATA");
         AssertAsked_(host, FakeDnsServer.TypeA, FakeDnsServer.TypeAaaa);
      }

      [Test]
      [Description("A HELO host whose AAAA query fails and whose A query answers NODATA is not scored.")]
      public void HeloHostWhoseAaaaQueryFailsIsNotScored()
      {
         string host = "helo-aaaa-servfail." + Zone;
         SuiteDns.Zone.WithServFail(host, FakeDnsServer.TypeAaaa);

         ArmHelo_();
         string message = SendWithHelo_(host, "helo-aaaa-servfail");

         AssertNotScored_(message, HeloReason, "a HELO host whose AAAA query failed beside an A query answered NODATA");
      }

      [Test]
      [Description("A HELO host whose A query times out and whose AAAA query answers NODATA is not scored.")]
      public void HeloHostWhoseAQueryTimesOutIsNotScored()
      {
         string host = "helo-a-silent." + Zone;
         SuiteDns.Zone.WithSilence(host, FakeDnsServer.TypeA);

         ArmHelo_();
         string message = SendWithHelo_(host, "helo-a-silent");

         AssertNotScored_(message, HeloReason, "a HELO host whose A query timed out beside an AAAA query answered NODATA");
      }

      [Test]
      [Description("A HELO host with NODATA for A and AAAA whose CNAME query fails is not scored: whether it is an alias is not known.")]
      public void HeloHostWhoseCnameQueryFailsIsNotScored()
      {
         string host = "helo-cname-servfail." + Zone;
         SuiteDns.Zone.WithServFail(host, FakeDnsServer.TypeCname);

         ArmHelo_();
         string message = SendWithHelo_(host, "helo-cname-servfail");

         AssertNotScored_(message, HeloReason, "a HELO host with NODATA for A and AAAA whose CNAME query failed");
      }

      [Test]
      [Description("A HELO host whose every query fails is not scored, as before.")]
      public void HeloHostWhoseEveryQueryFailsIsNotScored()
      {
         string host = "helo-servfail." + Zone;
         SuiteDns.Zone.WithServFail(host);

         ArmHelo_();
         string message = SendWithHelo_(host, "helo-servfail");

         AssertNotScored_(message, HeloReason, "a HELO host whose every query answered SERVFAIL");
      }

      [Test]
      [Description("A HELO host that answers NODATA for A and AAAA has no address, and is scored, as before.")]
      public void HeloHostWithNoAddressIsScored()
      {
         string host = "helo-nodata." + Zone;

         ArmHelo_();
         string message = SendWithHelo_(host, "helo-nodata");

         AssertScored_(message, HeloReason, "a HELO host answered NODATA for A and AAAA");
      }

      [Test]
      [Description("A HELO host that does not exist (NXDOMAIN) has no address, and is scored, as before.")]
      public void HeloHostThatDoesNotExistIsScored()
      {
         string host = "helo-nxdomain." + Zone;
         SuiteDns.Zone.WithNxDomain(host);

         ArmHelo_();
         string message = SendWithHelo_(host, "helo-nxdomain");

         AssertScored_(message, HeloReason, "a HELO host answered NXDOMAIN");
      }

      [Test]
      [Description("A HELO host that does not exist (NXDOMAIN for A and AAAA) is scored whatever a CNAME query would meet: a name that does not exist is no alias, and is not asked about as one.")]
      public void HeloHostThatDoesNotExistIsScoredWhateverItsCnameQueryWouldMeet()
      {
         // Asked, a CNAME query that failed made the lookup one that did not
         // complete, and the HELO host went unscored: one lost packet sufficed.
         string host = "helo-nxdomain-cname-servfail." + Zone;
         SuiteDns.Zone.WithNxDomain(host)
            .WithServFail(host, FakeDnsServer.TypeCname);

         ArmHelo_();
         string message = SendWithHelo_(host, "helo-nxdomain-cname");

         AssertScored_(message, HeloReason, "a HELO host answered NXDOMAIN for A and AAAA, whose CNAME query would have failed");
         AssertAsked_(host, FakeDnsServer.TypeA, FakeDnsServer.TypeAaaa);
         ClassicAssert.AreEqual(0, AskedOf_(host, FakeDnsServer.TypeCname),
            "A HELO host that does not exist was asked whether it is an alias. Asked: " + string.Join(", ", SuiteDns.Zone.Queries));
      }

      [Test]
      [Description("A HELO host that resolves to the client's own address is not scored, as before.")]
      public void HeloHostThatResolvesToTheClientIsNotScored()
      {
         string host = "helo-matches." + Zone;
         SuiteDns.Zone.WithA(host, TestSetup.GetLocalIpAddress().ToString())
            .WithServFail(host, FakeDnsServer.TypeAaaa);

         ArmHelo_();
         string message = SendWithHelo_(host, "helo-matches");

         AssertNotScored_(message, HeloReason, "a HELO host whose A record is the client's address, with a failed AAAA query");
      }

      // ------------------------------------------------------------------ PTR

      [Test]
      [Description("A client whose reverse name's A query fails is not scored by the PTR test: the forward lookup failed.")]
      public void ReverseNameWhoseLookupFailsIsNotScored()
      {
         string reverseName = "ptr-a-servfail." + Zone;
         SuiteDns.Zone
            .WithPtr(ReverseLookupName_(TestSetup.GetLocalIpAddress()), reverseName)
            .WithServFail(reverseName, FakeDnsServer.TypeA);

         _antiSpam.CheckPTR = true;
         _antiSpam.CheckPTRScore = 5;

         string message = SendWithHelo_("client.example.test", "ptr-a-servfail");

         AssertNotScored_(message, PtrReason, "a reverse name whose A query failed beside an AAAA query answered NODATA");
         AssertAsked_(ReverseLookupName_(TestSetup.GetLocalIpAddress()), FakeDnsServer.TypePtr);
      }

      [Test]
      [Description("A client whose reverse name has no address is scored by the PTR test, as before.")]
      public void ReverseNameWithNoAddressIsScored()
      {
         string reverseName = "ptr-nodata." + Zone;
         SuiteDns.Zone.WithPtr(ReverseLookupName_(TestSetup.GetLocalIpAddress()), reverseName);

         _antiSpam.CheckPTR = true;
         _antiSpam.CheckPTRScore = 5;

         string message = SendWithHelo_("client.example.test", "ptr-nodata");

         AssertScored_(message, PtrReason, "a reverse name answered NODATA for A and AAAA");
      }

      // ------------------------------------------------------------------ MX records

      [Test]
      [Description("A sender's domain with no MX record whose CNAME query fails is not scored by the MX-record test: whether it is an alias is not known.")]
      public void SenderDomainWhoseCnameQueryFailsIsNotScoredForMx()
      {
         string domain = "mx-cname-servfail." + Zone;
         SuiteDns.Zone.WithServFail(domain, FakeDnsServer.TypeCname);

         _antiSpam.UseMXChecks = true;
         _antiSpam.UseMXChecksScore = 5;

         string message = SendWithHelo_("client.example.test", "mx-cname-servfail", "sender@" + domain);

         AssertNotScored_(message, "X-hMailServer-Reason-", "a sender's domain with no MX whose CNAME query failed");
      }

      [Test]
      [Description("A sender's domain with no MX record anywhere is scored by the MX-record test, as before.")]
      public void SenderDomainWithNoMxIsScored()
      {
         string domain = "mx-nodata." + Zone;

         _antiSpam.UseMXChecks = true;
         _antiSpam.UseMXChecksScore = 5;

         string message = SendWithHelo_("client.example.test", "mx-nodata", "sender@" + domain);

         AssertScored_(message, "X-hMailServer-Reason-", "a sender's domain answered NODATA for MX and CNAME, and so did its registrable domain");
      }

      [Test]
      [Description("A sender's domain that does not exist (NXDOMAIN) has no MX record, and is scored by the MX-record test whatever a CNAME query would meet: it is not asked.")]
      public void SenderDomainThatDoesNotExistIsScoredForMx()
      {
         string domain = "mx-nxdomain-cname-servfail." + Zone;
         SuiteDns.Zone.WithNxDomain(domain)
            .WithServFail(domain, FakeDnsServer.TypeCname);

         _antiSpam.UseMXChecks = true;
         _antiSpam.UseMXChecksScore = 5;

         string message = SendWithHelo_("client.example.test", "mx-nxdomain-cname", "sender@" + domain);

         AssertScored_(message, "Sender domain does not have any MX records.",
            "a sender's domain answered NXDOMAIN, whose CNAME query would have failed, and its registrable domain NODATA");
         AssertAsked_(domain, FakeDnsServer.TypeMx);
         ClassicAssert.AreEqual(0, AskedOf_(domain, FakeDnsServer.TypeCname),
            "A sender's domain that does not exist was asked whether it is an alias. Asked: " + string.Join(", ", SuiteDns.Zone.Queries));
      }

      // ------------------------------------------------------------------ DKIM

      [Test]
      [Description("A DKIM signature whose key does not exist (NXDOMAIN) fails permanently - no key for signature, RFC 6376 section 6.1.2 - whatever a CNAME query of the key's name would meet: it is not asked.")]
      public void DkimKeyThatDoesNotExistIsAPermanentFailure()
      {
         // Asked, a CNAME query that failed made the key's lookup one that did
         // not complete: a TempFail, and the message accepted without a verdict.
         // The key is looked up before the body hash is checked, so the
         // signature's placeholder hashes are never reached.
         const string selector = "dkim-nxdomain";
         string keyName = selector + "._domainkey." + Zone;
         SuiteDns.Zone.WithNxDomain(keyName)
            .WithServFail(keyName, FakeDnsServer.TypeCname);

         _antiSpam.DKIMVerificationEnabled = true;
         _antiSpam.DKIMVerificationFailureScore = 5;

         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "dkim-nxdomain@example.test", "test");
         string raw = "DKIM-Signature: v=1; a=rsa-sha256; c=relaxed/relaxed; d=" + Zone + ";\r\n" +
                      "\ts=" + selector + "; h=from:subject;\r\n" +
                      "\tbh=AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=; b=AAAAAAAAAAAA\r\n" +
                      "From: <" + account.Address + ">\r\n" +
                      "To: <" + account.Address + ">\r\n" +
                      "Subject: DNS lookup failures\r\n" +
                      "\r\n" +
                      "A message.\r\n";
         SmtpClientSimulator.StaticSendRaw(account.Address, account.Address, raw);
         string message = Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test");

         AssertScored_(message, "Rejected by DKIM.", "a DKIM signature whose key's name answered NXDOMAIN, whose CNAME query would have failed");
         AssertAsked_(keyName, FakeDnsServer.TypeTxt);
         ClassicAssert.AreEqual(0, AskedOf_(keyName, FakeDnsServer.TypeCname),
            "A DKIM key's name that does not exist was asked whether it is an alias. Asked: " + string.Join(", ", SuiteDns.Zone.Queries));
      }

      // ------------------------------------------------------------------ DNS blacklists

      [Test]
      [Description("A DNS blacklist lookup whose A query fails once is asked again, and the listing it then answers counts.")]
      public void BlacklistLookupThatFailsOnceIsAskedAgain()
      {
         string checkHost = ArmBlacklist_("dnsbl-flaky." + Zone);
         SuiteDns.Zone.WithA(checkHost, "127.0.0.2")
            .WithServFailFirst(checkHost, FakeDnsServer.TypeA, 1);

         string message = SendFromLoopback_("dnsbl-flaky");

         AssertScored_(message, "Listed by dnsbl-flaky", "a blacklist that failed the first A query and listed the client in the second");

         var asked = AskedOf_(checkHost, FakeDnsServer.TypeA);
         ClassicAssert.AreEqual(2, asked, "The A query was not asked exactly twice. Asked: " + string.Join(", ", SuiteDns.Zone.Queries));
      }

      [Test]
      [Description("A DNS blacklist lookup that fails twice is not a listing, and the TCP/IP log says the client could not be checked rather than that the list has no address for it.")]
      public void BlacklistLookupThatFailsIsLoggedAndNotAListing()
      {
         string checkHost = ArmBlacklist_("dnsbl-down." + Zone);
         SuiteDns.Zone.WithServFail(checkHost, FakeDnsServer.TypeA);

         string message = SendFromLoopback_("dnsbl-down");

         AssertNotScored_(message, "Listed by dnsbl-down", "a blacklist whose A query failed twice");

         string log = LogHandler.ReadCurrentDefaultLog();
         StringAssert.Contains("DNS lookup failed: " + checkHost + ". The client could not be checked against this blacklist.", log);
         StringAssert.DoesNotContain(checkHost + ", 0 addresses found", log,
            "A lookup that failed was logged as a list that answered with no address.");
         ClassicAssert.AreEqual(2, AskedOf_(checkHost, FakeDnsServer.TypeA),
            "A lookup that failed was not asked exactly twice. Asked: " + string.Join(", ", SuiteDns.Zone.Queries));
      }

      [Test]
      [Description("A DNS blacklist lookup whose A query times out is asked again, and then logged as not checked.")]
      public void BlacklistLookupThatTimesOutIsLoggedAndNotAListing()
      {
         string checkHost = ArmBlacklist_("dnsbl-silent." + Zone);
         SuiteDns.Zone.WithSilence(checkHost, FakeDnsServer.TypeA);

         string message = SendFromLoopback_("dnsbl-silent");

         AssertNotScored_(message, "Listed by dnsbl-silent", "a blacklist whose A query timed out");
         StringAssert.Contains("DNS lookup failed: " + checkHost + ".", LogHandler.ReadCurrentDefaultLog());
      }

      [Test]
      [Description("A DNS blacklist that answers NXDOMAIN - the address is not listed - is asked once and logged as an answer, as before.")]
      public void BlacklistThatAnswersNotListedIsAskedOnce()
      {
         string checkHost = ArmBlacklist_("dnsbl-clean." + Zone);
         SuiteDns.Zone.WithNxDomain(checkHost);

         string message = SendFromLoopback_("dnsbl-clean");

         AssertNotScored_(message, "Listed by dnsbl-clean", "a blacklist that answered NXDOMAIN");
         StringAssert.Contains(checkHost + ", 0 addresses found: (none), Match: False", LogHandler.ReadCurrentDefaultLog());
         ClassicAssert.AreEqual(1, AskedOf_(checkHost, FakeDnsServer.TypeA),
            "An answered lookup was asked again. Asked: " + string.Join(", ", SuiteDns.Zone.Queries));
      }

      [Test]
      [Description("A DNS blacklist is asked for the A record alone, the only record a list answers with (RFC 5782): no AAAA query is made, so one that would have timed out neither fails the lookup nor delays it, and the list's NXDOMAIN is its answer.")]
      public void BlacklistIsAskedForTheARecordAlone()
      {
         string checkHost = ArmBlacklist_("dnsbl-aonly." + Zone);
         SuiteDns.Zone.WithNxDomain(checkHost)
            .WithSilence(checkHost, FakeDnsServer.TypeAaaa);

         string message = SendFromLoopback_("dnsbl-aonly");

         AssertNotScored_(message, "Listed by dnsbl-aonly", "a blacklist that answered NXDOMAIN");

         string log = LogHandler.ReadCurrentDefaultLog();
         StringAssert.Contains(checkHost + ", 0 addresses found: (none), Match: False", log,
            "The list's NXDOMAIN was not read as its answer.");
         StringAssert.DoesNotContain("DNS lookup failed: " + checkHost, log,
            "A lookup the list answered failed on an AAAA query, which holds no verdict.");
         ClassicAssert.AreEqual(1, AskedOf_(checkHost, FakeDnsServer.TypeA),
            "The A query was not asked exactly once. Asked: " + string.Join(", ", SuiteDns.Zone.Queries));
         ClassicAssert.AreEqual(0, AskedOf_(checkHost, FakeDnsServer.TypeAaaa),
            "A DNS blacklist was asked for AAAA records, which no list answers with. Asked: " + string.Join(", ", SuiteDns.Zone.Queries));
      }

      // ------------------------------------------------------------------ SURBL

      [Test]
      [Description("SURBL passes over the names it could not look up and asks the rest: a listed link after twenty that fail is found.")]
      public void SurblPassesOverNamesItCouldNotLookUp()
      {
         var surblServer = ArmSurbl_();
         try
         {
            // More names that fail than SURBL asks at once (LookupsAtOnce, 16), all
            // before the listed one: when a failed lookup ended the list, every
            // lookup begun with the first one failed, and the listed name was
            // never asked.
            var body = new System.Text.StringBuilder("Links:");
            for (int i = 1; i <= 20; i++)
            {
               string failing = "surbl-fail-" + i.ToString("00") + ".test";
               SuiteDns.Zone.WithServFail(failing + "." + SurblZone);
               body.Append(" http://" + failing + "/");
            }

            body.Append(" and http://surbl-org-permanent-test-point.com/ last");

            string message = SendFromLoopback_("surbl-pass-over", body.ToString());

            AssertScored_(message, "X-hMailServer-Spam", "a listed link after twenty whose lookups failed");
            ClassicAssert.IsTrue(SuiteDns.Zone.Queries.Contains(FakeDnsServer.TypeA + "/" + SuiteDns.SurblTestPoint),
               "The listed link was never asked about. Asked: " + string.Join(", ", SuiteDns.Zone.Queries));
         }
         finally
         {
            surblServer.Active = false;
            surblServer.Save();
         }
      }

      [Test]
      [Description("A SURBL lookup whose A query fails once is asked again, and the listing it then answers counts.")]
      public void SurblLookupThatFailsOnceIsAskedAgain()
      {
         var surblServer = ArmSurbl_();
         try
         {
            string lookupName = "surbl-flaky.test." + SurblZone;
            SuiteDns.Zone.WithA(lookupName, "127.0.0.2")
               .WithServFailFirst(lookupName, FakeDnsServer.TypeA, 1);

            string message = SendFromLoopback_("surbl-flaky", "A link: http://surbl-flaky.test/ here");

            AssertScored_(message, "X-hMailServer-Spam", "a SURBL lookup that failed the first A query and listed the name in the second");
            ClassicAssert.AreEqual(2, AskedOf_(lookupName, FakeDnsServer.TypeA),
               "The A query was not asked exactly twice. Asked: " + string.Join(", ", SuiteDns.Zone.Queries));
         }
         finally
         {
            surblServer.Active = false;
            surblServer.Save();
         }
      }

      [Test]
      [Description("SURBL is asked for the A record alone, the only record a list answers with (RFC 5782): no AAAA query is made, so one that would have failed does not make the list's NXDOMAIN a lookup to ask again.")]
      public void SurblIsAskedForTheARecordAlone()
      {
         var surblServer = ArmSurbl_();
         try
         {
            string lookupName = "surbl-aonly.test." + SurblZone;
            SuiteDns.Zone.WithNxDomain(lookupName)
               .WithServFail(lookupName, FakeDnsServer.TypeAaaa);

            string message = SendFromLoopback_("surbl-aonly", "A link: http://surbl-aonly.test/ here");

            AssertNotScored_(message, "X-hMailServer-Spam", "a link the list answered NXDOMAIN for");
            ClassicAssert.AreEqual(1, AskedOf_(lookupName, FakeDnsServer.TypeA),
               "The A query was not asked exactly once: the list's answer was taken for a lookup that failed. Asked: " +
               string.Join(", ", SuiteDns.Zone.Queries));
            ClassicAssert.AreEqual(0, AskedOf_(lookupName, FakeDnsServer.TypeAaaa),
               "SURBL was asked for AAAA records, which no list answers with. Asked: " + string.Join(", ", SuiteDns.Zone.Queries));
         }
         finally
         {
            surblServer.Active = false;
            surblServer.Save();
         }
      }

      // ------------------------------------------------------------------ delivery

      [Test]
      [Description("A recipient's mail server whose A query fails beside an AAAA query answered NODATA is a DNS query that failed, and the notice says so, rather than that the domain has no mail servers.")]
      public void DeliveryToAnExchangeWhoseLookupFailsSaysTheLookupFailed()
      {
         // Delivery is retried either way (ExternalDelivery::HandleNoRecipientServers_);
         // what changes is what the sender is told. The suite delivers with no
         // retries, so the notice comes at once.
         string domain = "deliver-a-servfail." + Zone;
         string exchange = "mx." + domain;
         SuiteDns.Zone.WithMx(domain, 10, exchange)
            .WithServFail(exchange, FakeDnsServer.TypeA);

         string notice = DeliverToAndReadTheNotice_("deliver-a-servfail", "someone@" + domain);

         // The status in the message/delivery-status part, which is 7bit: the
         // sentences are in the quoted-printable text part, cut where it wraps.
         // 4.4.4 goes with "The DNS query has failed.", 4.1.2 with "No mail
         // servers appear to exists for the recipient's address."
         StringAssert.Contains("Status: 4.4.4", notice,
            "An exchange whose A query failed was not reported as a DNS query that failed.");
         StringAssert.DoesNotContain("Status: 4.1.2", notice,
            "An exchange whose A query failed was reported as a domain with no mail servers - the AAAA query's NODATA was read as the exchange's whole answer.");
         AssertAsked_(exchange, FakeDnsServer.TypeA, FakeDnsServer.TypeAaaa);
      }

      [Test]
      [Description("A recipient's mail server with no address at all - NODATA for A and AAAA - is still a domain with no mail servers, as before.")]
      public void DeliveryToAnExchangeWithNoAddressSaysThereAreNoMailServers()
      {
         string domain = "deliver-nodata." + Zone;
         SuiteDns.Zone.WithMx(domain, 10, "mx." + domain);

         string notice = DeliverToAndReadTheNotice_("deliver-nodata", "someone@" + domain);

         StringAssert.Contains("Status: 4.1.2", notice,
            "An exchange that answered NODATA for A and AAAA was not reported as a domain with no mail servers.");
      }

      [Test]
      [Description("A recipient's domain that does not exist (NXDOMAIN) has no mail servers, and the notice says so whatever a CNAME query of the name would meet: the address lookup made in place of an MX asks none.")]
      public void DeliveryToADomainThatDoesNotExistSaysThereAreNoMailServers()
      {
         // With no MX the domain's own addresses are its mail servers (RFC 5321
         // section 5.1), and that address lookup asked the CNAME too: failed, it
         // made the domain's lookup one that did not complete (4.4.4).
         string domain = "deliver-nxdomain-cname-servfail." + Zone;
         SuiteDns.Zone.WithNxDomain(domain)
            .WithServFail(domain, FakeDnsServer.TypeCname);

         string notice = DeliverToAndReadTheNotice_("deliver-nxdomain-cname", "someone@" + domain);

         StringAssert.Contains("Status: 4.1.2", notice,
            "A recipient's domain that does not exist was not reported as a domain with no mail servers.");
         StringAssert.DoesNotContain("Status: 4.4.4", notice,
            "A recipient's domain that does not exist was reported as a DNS query that failed - the CNAME query, asked of a name that does not exist, decided the answer.");
         AssertAsked_(domain, FakeDnsServer.TypeMx, FakeDnsServer.TypeA, FakeDnsServer.TypeAaaa);
      }

      [Test]
      [Description("A route's host whose A query fails beside an AAAA query answered NODATA is a DNS query that failed, and the notice says so, rather than that the relay server could not be found.")]
      public void DeliveryThroughARouteWhoseHostLookupFailsSaysTheLookupFailed()
      {
         // A route's host, as the SMTP relay server's, is looked up as an address
         // (ExternalDelivery::ResolveRecipientServers_'s fixed hosts), and the
         // notice for a host with no address is "The host specified as SMTP relay
         // server could not be found" (4.3.5), for a lookup that failed "The DNS
         // query has failed" (4.4.4). Retried either way.
         string domain = "deliver-route-a-servfail." + Zone;
         string relayHost = "relay." + domain;
         SuiteDns.Zone.WithServFail(relayHost, FakeDnsServer.TypeA);
         AddRoute_(domain, relayHost);

         string notice = DeliverToAndReadTheNotice_("deliver-route-a-servfail", "someone@" + domain);

         StringAssert.Contains("Status: 4.4.4", notice,
            "A route's host whose A query failed was not reported as a DNS query that failed.");
         StringAssert.DoesNotContain("Status: 4.3.5", notice,
            "A route's host whose A query failed was reported as a relay server that could not be found - the AAAA query's NODATA was read as the host's whole answer.");
         AssertAsked_(relayHost, FakeDnsServer.TypeA, FakeDnsServer.TypeAaaa);
      }

      [Test]
      [Description("A route's host with no address at all - NODATA for A and AAAA - is still a relay server that could not be found, as before.")]
      public void DeliveryThroughARouteWhoseHostHasNoAddressSaysItCouldNotBeFound()
      {
         string domain = "deliver-route-nodata." + Zone;
         AddRoute_(domain, "relay." + domain);

         string notice = DeliverToAndReadTheNotice_("deliver-route-nodata", "someone@" + domain);

         StringAssert.Contains("Status: 4.3.5", notice,
            "A route's host that answered NODATA for A and AAAA was not reported as a relay server that could not be found.");
      }

      // ------------------------------------------------------------------ the MX query

      [Test]
      [Description("The Control Panel's MX query (Utilities.ResolveMXRecords) of a domain whose exchange's A query fails beside an AAAA query answered NODATA says that the lookup failed, rather than that the domain has no mail servers.")]
      public void MxQueryOfAnExchangeWhoseLookupFailsSaysTheLookupFailed()
      {
         // The lookup delivery makes (DNSResolver::GetEmailServers): a lookup that
         // failed with no server found is an error, "The DNS lookup failed", and
         // one that answered with none an empty answer, which the Control Panel
         // shows as "The server found no mail servers". The Linux suite's shim
         // has no route for this method and skips both cases - which is why the
         // call is not made inside Assert.Throws, which would take the skip for
         // the wrong exception.
         string domain = "mxtool-a-servfail." + Zone;
         string exchange = "mx." + domain;
         SuiteDns.Zone.WithMx(domain, 10, exchange)
            .WithServFail(exchange, FakeDnsServer.TypeA);

         string servers = null;
         System.Runtime.InteropServices.COMException failure = null;
         try
         {
            servers = _application.Utilities.ResolveMXRecords(domain);
         }
         catch (System.Runtime.InteropServices.COMException lookupFailure)
         {
            failure = lookupFailure;
         }

         ClassicAssert.IsNotNull(failure,
            "An exchange whose A query failed was reported as a domain with no mail servers - the AAAA query's NODATA was read as the exchange's whole answer. The lookup answered: \"" + servers + "\"");
         StringAssert.Contains("The DNS lookup failed", failure.Message,
            "The lookup failed, but not as a DNS lookup failure: " + failure.Message);
         AssertAsked_(exchange, FakeDnsServer.TypeA, FakeDnsServer.TypeAaaa);
      }

      [Test]
      [Description("The MX query of a domain whose exchange has no address at all - NODATA for A and AAAA - still answers with no mail servers, as before.")]
      public void MxQueryOfAnExchangeWithNoAddressFindsNoMailServers()
      {
         string domain = "mxtool-nodata." + Zone;
         SuiteDns.Zone.WithMx(domain, 10, "mx." + domain);

         string servers = _application.Utilities.ResolveMXRecords(domain);

         ClassicAssert.IsTrue(string.IsNullOrEmpty(servers),
            "An exchange that answered NODATA for A and AAAA was not reported as a domain with no mail servers: " + servers);
      }

      // ------------------------------------------------------------------ helpers

      // Sends from a new local account to recipient, waits for the queue to empty,
      // and reads the notice that came back to the sender.
      private string DeliverToAndReadTheNotice_(string local, string recipient)
      {
         var sender = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, local + "@example.test", "test");
         SmtpClientSimulator.StaticSend(sender.Address, recipient, "DNS lookup failures", "A message.");
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         return Pop3ClientSimulator.AssertGetFirstMessageText(sender.Address, "test");
      }

      // A route for domain to relayHost, tried once: the notice comes at once.
      // PerformBasicSetup removes every route before the next test.
      private void AddRoute_(string domain, string relayHost)
      {
         var route = _settings.Routes.Add();
         route.DomainName = domain;
         route.TargetSMTPHost = relayHost;
         route.TargetSMTPPort = 25;
         route.NumberOfTries = 0;
         route.MinutesBetweenTry = 5;
         route.Save();
      }

      private void ArmHelo_()
      {
         _antiSpam.CheckHostInHelo = true;
         _antiSpam.CheckHostInHeloScore = 5;
      }

      private string ArmBlacklist_(string listZone)
      {
         var list = _antiSpam.DNSBlackLists.Add();
         list.DNSHost = listZone;
         list.ExpectedResult = "127.0.0.2";
         list.RejectMessage = "Listed by " + listZone.Split('.')[0];
         list.Score = 5;
         list.Active = true;
         list.Save();

         // The client is 127.0.0.1 (SendFromLoopback_), which a list is asked
         // about reversed.
         return "1.0.0.127." + listZone;
      }

      private hMailServer.SURBLServer ArmSurbl_()
      {
         var surblServer = _antiSpam.SURBLServers[0];
         surblServer.Active = true;
         surblServer.Score = 5;
         surblServer.ExpectedResult = "";
         surblServer.Save();
         return surblServer;
      }

      // A message from 127.0.0.1, read back from the mailbox it was delivered to.
      private string SendFromLoopback_(string local, string body = "A message.")
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, local + "@example.test", "test");
         new SmtpClientSimulator().Send(account.Address, account.Address, "DNS lookup failures", body);
         return Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test");
      }

      // A message from the machine's own address, with the HELO given, read back
      // from the mailbox it was delivered to. The sender is not one of this
      // server's: from an address outside the loopback range a local sender is
      // asked to authenticate (530), and the tests are of a stranger's mail.
      private string SendWithHelo_(string helo, string local, string sender = null)
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, local + "@example.test", "test");
         string from = sender ?? "someone@sender." + Zone;

         using (var connection = new TcpConnection())
         {
            ClassicAssert.IsTrue(connection.Connect(TestSetup.GetLocalIpAddress(), TestPorts.Smtp),
               "Could not connect to the server at " + TestSetup.GetLocalIpAddress());

            ExpectReply_(connection.Receive(), "220");
            ExpectReply_(connection.SendAndReceive("HELO " + helo + "\r\n"), "250");
            ExpectReply_(connection.SendAndReceive("MAIL FROM:<" + from + ">\r\n"), "250");
            ExpectReply_(connection.SendAndReceive("RCPT TO:<" + account.Address + ">\r\n"), "250");
            ExpectReply_(connection.SendAndReceive("DATA\r\n"), "354");
            ExpectReply_(connection.SendAndReceive("From: " + from + "\r\nTo: " + account.Address +
                                                   "\r\nSubject: DNS lookup failures\r\n\r\nA message.\r\n.\r\n"), "250");
            connection.SendAndReceive("QUIT\r\n");
         }

         return Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test");
      }

      private static void ExpectReply_(string reply, string code)
      {
         ClassicAssert.IsTrue(reply != null && reply.StartsWith(code), "Expected " + code + ", the server said: " + reply);
      }

      private static string ReverseLookupName_(IPAddress address)
      {
         return string.Join(".", address.GetAddressBytes().Reverse()) + ".in-addr.arpa";
      }

      private static int AskedOf_(string name, int type)
      {
         string question = type + "/" + name.ToLowerInvariant();
         return SuiteDns.Zone.Queries.Count(asked => asked == question);
      }

      private static void AssertAsked_(string name, params int[] types)
      {
         foreach (int type in types)
            ClassicAssert.IsTrue(AskedOf_(name, type) > 0,
               "The zone was never asked " + type + "/" + name + ", so the case did not test what it says. Asked: " +
               string.Join(", ", SuiteDns.Zone.Queries));
      }

      private static void AssertScored_(string message, string reason, string what)
      {
         ClassicAssert.IsTrue(message.Contains(reason), "Not scored: " + what + ".\r\n" + message);
      }

      private static void AssertNotScored_(string message, string reason, string what)
      {
         ClassicAssert.IsFalse(message.Contains(reason), "Scored: " + what + " - a lookup that failed was read as a name with no addresses.\r\n" + message);
      }
   }
}
