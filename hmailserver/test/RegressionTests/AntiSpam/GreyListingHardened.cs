// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Text;
using System.Threading;
using NUnit.Framework;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.AntiSpam
{
   /// <summary>
   ///    Greylisting, hardened (discussions #281, #282 and #284 of 17 September 2026):
   ///    the fixed-width triplet key, the sliding wait and the abuse refusal, and the
   ///    per-domain bypass list. Every one of them is off by default, and the first
   ///    thing several of these tests prove is that with the setting at its default
   ///    greylisting answers as it always has.
   ///
   ///    The triplet key cannot be seen from outside - there is no route that lists
   ///    triplets - so it is proved through the abuse threshold: a triplet hammered
   ///    past the threshold is refused with 550, and a second sender that shares the
   ///    first 240 of its 246 characters is answered 451 as a newcomer, where the old
   ///    200-character columns made it the same triplet and would have answered 550.
   ///
   ///    The bypass list is a REST resource, made and torn down over the API on a
   ///    port of this fixture's own, so that nothing here needs the new COM
   ///    collection registered on the bench.
   ///
   ///    Three of these are negative controls of one rule - a result an entry
   ///    requires must belong to a name the entry covers - and each is paired with
   ///    the positive case, because a negative control that passes because the
   ///    result never happened proves nothing: the signed message satisfies the
   ///    entry for the domain that signed it and not one for the envelope's
   ///    domain, the DMARC pass satisfies the entry for the From domain and not
   ///    one for the envelope's, and the HELO entry is satisfied by a record at
   ///    the HELO name and not by the envelope's SPF pass. Every one of them
   ///    answered 250 before the rule.
   /// </summary>
   [TestFixture]
   public class GreyListingHardened : TestFixtureBase
   {
      private const int RestPort = 9676;
      private const string AdminPassword = "testar";

      // selector1._domainkey.outlook.com, served locally as AntiSpam.DKIM.Verification
      // serves it: the key the captured message in TestResources.MessageWithValidDkim
      // verifies against.
      private const string OutlookSelectorName = "selector1._domainkey.outlook.com";

      private const string OutlookSelectorKey =
         "v=DKIM1;k=rsa;p=MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAvWyktrIL8DO/+UGvMbv7cPd/Xogpbs7pgVw8" +
         "y9ldO6AAMmg8+ijENl/c7Fb1MfKM7uG3LMwAr0dVVKyM+mbkoX2k5L7lsROQr0Z9gGSpu7xrnZOa58+/pIhd2Xk/DFPpa5+T" +
         "KbWodbsSZPRN8z0RY5x59jdzSclXlEyN9mEZdmOiKTsOP6A7vQxfSya9jg5N81dfNNvP7HnWejMMsKyIMrXptxOhIBuEYH67" +
         "JDe98QgX14oHvGM2Uz53if/SW8MF09rYh9sp4ZsaWLIg6T343JzlbtrsGRGCDJ9JPpxRWZimtz+Up/BlKzT6sCCrBihb/Bi3" +
         "pZiEBB4Ui/vruL5RCQIDAQAB;n=2048,1452627113,1468351913";

      private hMailServer.AntiSpam _antiSpam;

      // The three new members are reached through dynamic, as SpamTraps and
      // SenderLists reach theirs: the suite's interop is generated from the
      // registered type library, and naming them early-bound would not compile
      // against a bench whose server predates them.
      private dynamic _hardened;
      private int initialDelayBefore_;
      private bool bypassOnSpfBefore_;
      private readonly List<long> bypassEntriesMade_ = new List<long>();
      private bool restStarted_;

      [SetUp]
      public new void SetUp()
      {
         _antiSpam = _settings.AntiSpam;
         _hardened = _antiSpam;

         initialDelayBefore_ = _antiSpam.GreyListingInitialDelay;
         bypassOnSpfBefore_ = _antiSpam.BypassGreylistingOnSPFSuccess;

         // The server-wide SPF bypass is what the list is an alternative to, and a
         // fresh database ships it ON: with it on, an SPF pass would skip greylisting
         // whatever the list said, and the tests below could not tell the two apart.
         _antiSpam.BypassGreylistingOnSPFSuccess = false;
         _hardened.GreyListingSlideOnEarlyRetry = false;
         _hardened.GreyListingAbuseThreshold = 0;
         _hardened.GreyListingAbuseLockMinutes = 60;
         _antiSpam.GreyListingEnabled = true;
      }

      [TearDown]
      public new void TearDown()
      {
         // Every delete is checked. An entry left behind switches greylisting
         // off for its domain in every fixture that runs after this one, and a
         // DELETE whose status nobody looked at is how that happens quietly -
         // the failure would land on some later fixture's missing 451.
         var notRemoved = new List<string>();

         try
         {
            foreach (long id in bypassEntriesMade_)
            {
               (int status, string body) removed = Http("DELETE", "/api/v1/greylisting-bypass/" + id);
               if (removed.status != 200)
                  notRemoved.Add(id + " -> " + removed.status + " " + removed.body);
            }

            bypassEntriesMade_.Clear();
         }
         finally
         {
            SuiteDns.Reset();

            // Restored here rather than in TestSetup.DisableSpamProtection, which
            // is early-bound and runs before every test on every bench.
            _antiSpam.GreyListingEnabled = false;
            _hardened.GreyListingSlideOnEarlyRetry = false;
            _hardened.GreyListingAbuseThreshold = 0;
            _hardened.GreyListingAbuseLockMinutes = 60;
            _antiSpam.GreyListingInitialDelay = initialDelayBefore_;
            _antiSpam.BypassGreylistingOnSPFSuccess = bypassOnSpfBefore_;
            _antiSpam.UseSPF = false;
            _antiSpam.DKIMVerificationEnabled = false;
            _antiSpam.DMARCEnabled = false;

            // The listener this fixture started is stopped the way it was started:
            // the setting, then the reinitialise that reads it.
            if (restStarted_)
            {
               restStarted_ = false;
               RestListener.Stop();
               _application.Reinitialize();
            }
         }

         Assert.AreEqual(0, notRemoved.Count,
            "A greylisting bypass entry this fixture made was not removed, so greylisting is off for its domain in every " +
            "fixture after this one: " + string.Join("; ", notRemoved));
      }

      // ------------------------------------------------------------- the triplet key

      [Test]
      [Description("Two 246-character senders that share their first 240 characters are two triplets. Proved through the abuse " +
                   "threshold: the first is hammered into its 550, and the second is answered 451 as a newcomer, where the old " +
                   "200-character columns made the two the same triplet and would have answered 550.")]
      public void TwoLongAddressesSharingTheirFirstTwoHundredCharactersAreTwoTriplets()
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "grey@example.test", "test");

         _hardened.GreyListingAbuseThreshold = 2;

         // 60 + 1 + 63 + 1 + 63 + 1 + 51 + 6 = 246 characters, under RFC 5321's 254
         // and over the 200 the columns used to hold; the two differ in their last
         // label only.
         string localPart = new string('a', 60);
         string domainStem = new string('b', 63) + "." + new string('c', 63) + "." + new string('d', 51);
         string senderOne = localPart + "@" + domainStem + "1.test";
         string senderTwo = localPart + "@" + domainStem + "2.test";

         Assert.AreEqual(246, senderOne.Length);
         Assert.AreEqual(senderOne.Substring(0, 240), senderTwo.Substring(0, 240));

         Assert.AreEqual("451", Attempt(senderOne, account.Address), "the first attempt is deferred");
         Assert.AreEqual("451", Attempt(senderOne, account.Address), "the first early retry is deferred");
         Assert.AreEqual("550", Attempt(senderOne, account.Address), "the second early retry crosses the threshold of 2");

         // A different triplet, or the same one: this is the whole of #281.
         Assert.AreEqual("451", Attempt(senderTwo, account.Address),
            "a sender sharing the first 240 characters is a triplet of its own and is deferred as a newcomer, not refused as the abuser");
      }

      // --------------------------------------------------------- the abuse refusal

      [Test]
      [Description("With GreyListingAbuseThreshold 3, the first attempt and the two retries before the window opens are 451, the " +
                   "third retry is 550 and so is every attempt while the lock lasts; another recipient is another triplet and " +
                   "is deferred as usual; and with the threshold back at 0 the locked triplet is deferred again rather than refused.")]
      public void TheAbuseThresholdTurnsTheThirdEarlyRetryIntoA550ThatLocksTheTriplet()
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "grey@example.test", "test");
         var other = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "other@example.test", "test");

         _hardened.GreyListingAbuseThreshold = 3;

         const string sender = "hammer@retries-on-a-timer.test";

         Assert.AreEqual("451", Attempt(sender, account.Address), "first attempt");
         Assert.AreEqual("451", Attempt(sender, account.Address), "early retry 1");
         Assert.AreEqual("451", Attempt(sender, account.Address), "early retry 2");
         Assert.AreEqual("550", Attempt(sender, account.Address), "early retry 3 reaches the threshold");
         Assert.AreEqual("550", Attempt(sender, account.Address), "and the triplet stays refused while the lock lasts");

         Assert.AreEqual("451", Attempt(sender, other.Address), "another recipient is another triplet, deferred as a newcomer");

         _hardened.GreyListingAbuseThreshold = 0;
         Assert.AreEqual("451", Attempt(sender, account.Address), "with the threshold off the same triplet is deferred, not refused");
      }

      [Test]
      [Description("The control: with the threshold at its default of 0, a triplet hammered six times is deferred six times, " +
                   "which is classic greylisting and what a stock installation does.")]
      public void WithoutTheThresholdAHammeredTripletIsDeferredEveryTime()
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "grey@example.test", "test");

         const string sender = "hammer@retries-on-a-timer.test";

         for (int attempt = 1; attempt <= 6; attempt++)
            Assert.AreEqual("451", Attempt(sender, account.Address), "attempt " + attempt);
      }

      // ----------------------------------------------------------- the sliding wait

      [Test]
      [Description("With a one-minute initial delay, two triplets are made at the same moment; forty seconds on, one is retried " +
                   "with the slide ON and the other with it OFF; twenty-six seconds after that the slid triplet is still " +
                   "deferred, since its window moved to a hundred seconds, and the other walks through at the original sixty. " +
                   "Sixty-six seconds long because the delay's unit is the minute and the slide is only visible in time.")]
      public void TheSlideMovesTheWindowWhereAClassicTripletOpensAtTheOriginalTime()
      {
         var slid = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "slid@example.test", "test");
         var classic = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "classic@example.test", "test");

         _antiSpam.GreyListingInitialDelay = 1;

         const string sender = "patient@retries-from-a-queue.test";

         DateTime start = DateTime.UtcNow;
         Assert.AreEqual("451", Attempt(sender, slid.Address), "first attempt for the triplet that will slide");
         Assert.AreEqual("451", Attempt(sender, classic.Address), "first attempt for the classic triplet");

         SleepUntil(start.AddSeconds(40));

         _hardened.GreyListingSlideOnEarlyRetry = true;
         Assert.AreEqual("451", Attempt(sender, slid.Address), "an early retry with the slide on: the window moves to 100 seconds");

         _hardened.GreyListingSlideOnEarlyRetry = false;
         Assert.AreEqual("451", Attempt(sender, classic.Address), "an early retry with the slide off: the window stays at 60 seconds");

         SleepUntil(start.AddSeconds(66));

         Assert.AreEqual("451", Attempt(sender, slid.Address), "at 66 seconds the slid triplet is still short of its moved window");
         Assert.AreEqual("250", Attempt(sender, classic.Address), "at 66 seconds the classic triplet's original window has passed");

         Pop3ClientSimulator.AssertGetFirstMessageText(classic.Address, "test");
      }

      // ------------------------------------------------------------ the bypass list

      [Test]
      [Description("An entry requiring SPF alone decides at RCPT TO: a sender whose domain publishes a policy naming the client " +
                   "is not greylisted, a sender whose domain publishes none is, and the same passing sender is greylisted again " +
                   "once the entry is gone - so it was the entry and not the server-wide SPF bypass, which is off throughout.")]
      public void AnEntryRequiringSpfAloneSkipsGreylistingAtRcptTo()
      {
         StartRest();

         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "grey@example.test", "test");

         _antiSpam.UseSPF = true;
         SuiteDns.Zone.WithTxt("spf-pass.test", "v=spf1 ip4:127.0.0.1 -all");

         long passing = AddBypass("{\"pattern\":\"spf-pass.test\",\"match\":\"envelope\",\"require_spf\":true,\"note\":\"tidy enough\"}");
         AddBypass("{\"pattern\":\"spf-none.test\",\"match\":\"envelope\",\"require_spf\":true}");

         Assert.AreEqual("250", Attempt("sender@spf-pass.test", account.Address), "SPF passes, the entry is satisfied, no 451");
         Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test");

         Assert.AreEqual("451", Attempt("sender@spf-none.test", account.Address), "no SPF policy, no pass, greylisted as usual");

         (int status, string body) removed = Http("DELETE", "/api/v1/greylisting-bypass/" + passing);
         Assert.AreEqual(200, removed.status, removed.body);
         bypassEntriesMade_.Remove(passing);

         Assert.AreEqual("451", Attempt("sender@spf-pass.test", account.Address), "without the entry an SPF pass earns nothing");
      }

      [Test]
      [Description("An entry requiring DKIM cannot be judged at RCPT TO. A signed message from the domain is accepted whole; an " +
                   "unsigned one is answered 250 at RCPT TO and 451 after DATA, where the greylisting it was spared at RCPT TO " +
                   "runs; and with DKIM verification off the entry can never be satisfied, so the message is greylisted at " +
                   "RCPT TO as if the entry were not there rather than received first.")]
      public void AnEntryRequiringDkimDefersTheVerdictToAfterData()
      {
         StartRest();

         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "grey@example.test", "test");

         _antiSpam.DKIMVerificationEnabled = true;
         SuiteDns.Zone.WithTxt(OutlookSelectorName, OutlookSelectorKey);

         AddBypass("{\"pattern\":\"outlook.com\",\"match\":\"envelope\",\"require_spf\":false,\"require_dkim\":true,\"note\":\"signed or not at all\"}");

         const string sender = "someone@outlook.com";

         // The signed message: the entry is satisfied after DATA and nothing is deferred.
         SmtpClientSimulator.StaticSendRaw(sender, account.Address, TestResources.MessageWithValidDkim);
         Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test");

         // An unsigned message from the same sender to the same recipient: through
         // RCPT TO on the strength of the entry, greylisted after DATA.
         string rcptReply, dataReply;
         Converse(sender, account.Address, "Subject: unsigned\r\n\r\nNo signature here.\r\n", out rcptReply, out dataReply);
         Assert.IsTrue(rcptReply.StartsWith("250"), "RCPT TO is accepted while the DKIM result is unknown: " + rcptReply);
         Assert.IsTrue(dataReply.StartsWith("451"), "the message is greylisted after DATA once DKIM has not passed: " + dataReply);

         // With DKIM verification off the entry can never be satisfied, so the
         // message is not received first and greylisted after: it is greylisted at
         // RCPT TO as if the entry were not there.
         _antiSpam.DKIMVerificationEnabled = false;
         Converse(sender, account.Address, "Subject: unsigned\r\n\r\nNo signature here.\r\n", out rcptReply, out dataReply);
         Assert.IsTrue(rcptReply.StartsWith("451"), "with DKIM verification off the entry is not waited for: " + rcptReply);
      }

      [Test]
      [Description("A HELO entry is matched against the HELO host, at a label boundary: .mail.partner.test covers " +
                   "gw1.mail.partner.test and not notmail.partner.test. Its SPF requirement is answered by a check of the HELO " +
                   "identity itself (RFC 7208 section 2.3) and never by the envelope's pass: a client that says HELO " +
                   "gw2.mail.partner.test - covered by the suffix, and free text - while its MAIL FROM is at a domain that does " +
                   "publish a passing record earns nothing, because that pass authorises a different name.")]
      public void AHeloSuffixEntryCoversNamesUnderItAtALabelBoundary()
      {
         StartRest();

         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "grey@example.test", "test");
         var other = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "grey-helo@example.test", "test");

         _antiSpam.UseSPF = true;

         // Three records, and which one satisfies the entry is the whole test:
         // the HELO name's own, the envelope domain of the first two attempts,
         // and the envelope domain of the last one - which passes SPF and is not
         // covered by the entry.
         SuiteDns.Zone
            .WithTxt("gw1.mail.partner.test", "v=spf1 ip4:127.0.0.1 -all")
            .WithTxt("partner.test", "v=spf1 ip4:127.0.0.1 -all")
            .WithTxt("spf-pass.test", "v=spf1 ip4:127.0.0.1 -all");

         AddBypass("{\"pattern\":\".mail.partner.test\",\"match\":\"helo\",\"require_spf\":true}");

         Assert.AreEqual("250", Attempt("sender@partner.test", account.Address, "gw1.mail.partner.test"),
            "a host under the suffix whose own name authorises this client");
         Assert.AreEqual("451", Attempt("sender@partner.test", account.Address, "notmail.partner.test"),
            "a host that merely ends in the letters");

         // The negative control of the identity rule. Before the rule this was a
         // 250: the entry covered the HELO, an SPF pass existed, and nothing
         // asked whose it was.
         Assert.AreEqual("451", Attempt("sender@spf-pass.test", other.Address, "gw2.mail.partner.test"),
            "the HELO is covered but publishes nothing of its own, and the pass that exists is spf-pass.test's");
      }

      [Test]
      [Description("A DKIM signature is the signer's, not the sender's. The captured message is signed d=outlook.com: an entry on " +
                   "outlook.com requiring DKIM is satisfied by it - which is what proves the signature really did verify - and an " +
                   "entry on partner.example is not, though the message arrives with MAIL FROM at partner.example and carries a " +
                   "signature that passes. Anybody can sign a message with a key they publish themselves.")]
      public void AnEntryRequiringDkimIsNotSatisfiedByAnotherDomainsSignature()
      {
         StartRest();

         var signed = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "dkim-signed@example.test", "test");
         var borrowed = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "dkim-borrowed@example.test", "test");

         _antiSpam.DKIMVerificationEnabled = true;
         SuiteDns.Zone.WithTxt(OutlookSelectorName, OutlookSelectorKey);

         AddBypass("{\"pattern\":\"outlook.com\",\"match\":\"envelope\",\"require_dkim\":true}");
         AddBypass("{\"pattern\":\"partner.example\",\"match\":\"envelope\",\"require_dkim\":true}");

         // The positive control first: without it a broken signature would make
         // the negative one pass for the wrong reason.
         Assert.AreEqual("250", AttemptWith("someone@outlook.com", signed.Address, TestResources.MessageWithValidDkim),
            "the entry for the domain that signed it is satisfied after DATA");

         Assert.AreEqual("451", AttemptWith("someone@partner.example", borrowed.Address, TestResources.MessageWithValidDkim),
            "the entry for partner.example is not satisfied by outlook.com's signature, so greylisting runs after DATA");
      }

      [Test]
      [Description("DMARC judges the RFC5322.From domain, which the envelope need not agree with: the captured message is from " +
                   "@outlook.com and passes DMARC for outlook.com, an entry on outlook.com requiring DMARC is satisfied by it, and " +
                   "an entry on the envelope domain partner.example is not - the pass is not that domain's.")]
      public void AnEntryRequiringDmarcIsNotSatisfiedByAPassForAnotherFromDomain()
      {
         StartRest();

         var aligned = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "dmarc-aligned@example.test", "test");
         var borrowed = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "dmarc-borrowed@example.test", "test");

         _antiSpam.DKIMVerificationEnabled = true;
         _antiSpam.DMARCEnabled = true;

         // The signature aligns with the From, so the policy is met however the
         // envelope is addressed.
         SuiteDns.Zone
            .WithTxt(OutlookSelectorName, OutlookSelectorKey)
            .WithTxt("_dmarc.outlook.com", "v=DMARC1; p=none");

         AddBypass("{\"pattern\":\"outlook.com\",\"match\":\"envelope\",\"require_dmarc\":true}");
         AddBypass("{\"pattern\":\"partner.example\",\"match\":\"envelope\",\"require_dmarc\":true}");

         Assert.AreEqual("250", AttemptWith("someone@outlook.com", aligned.Address, TestResources.MessageWithValidDkim),
            "the entry for the From domain is satisfied after DATA, which is what proves DMARC passed at all");

         Assert.AreEqual("451", AttemptWith("someone@partner.example", borrowed.Address, TestResources.MessageWithValidDkim),
            "a DMARC pass for outlook.com is not a DMARC pass for partner.example");
      }

      // ------------------------------------------------- the row from before the key

      [Test]
      [Description("The migration half of #281. A row is put back the way schema 6071 left it - no key, the addresses cut to 200 " +
                   "characters - and the next attempt finds it by those addresses, is let through because its window has passed, " +
                   "and gives the row its key and its whole address there and then; every row still without a key is then deleted " +
                   "and the sender is still known, so it is the key that found it the second time. The service is restarted in the " +
                   "middle because the lookup asks once per process whether any keyless row remains, and on a bench that has never " +
                   "been upgraded the answer is no.")]
      public void ARowFromBeforeTheKeyIsFoundByItsAddressesAndRewrittenWithItsKey()
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "legacy@example.test", "test");

         // 60 + 1 + 63 + 1 + 63 + 1 + 55 = 244 characters, longer than the 200 the
         // old columns held: the legacy row keeps a prefix of it, the key is over
         // the whole of it, and the two have to agree for this to be found.
         string sender = new string('a', 60) + "@" + new string('b', 63) + "." + new string('c', 63) + "." +
                         new string('e', 49) + "legacy.test";

         Assert.AreEqual(244, sender.Length);

         Assert.AreEqual("451", Attempt(sender, account.Address), "the first attempt makes the triplet and defers it");

         // Written back as the upgrade leaves a row from before schema 6072, with
         // its window in the past so that finding it shows as an acceptance.
         string past = DateTime.Now.AddHours(-1).ToString("yyyy-MM-dd HH:mm:ss");
         string future = DateTime.Now.AddHours(24).ToString("yyyy-MM-dd HH:mm:ss");

         _application.Database.ExecuteSQL(
            "update hm_greylisting_triplets set glsenderhash = null, glrecipienthash = null, " +
            "glsenderaddress = '" + sender.Substring(0, 200) + "', " +
            "glblockendtime = '" + past + "', gldeletetime = '" + future + "' " +
            "where glsenderaddress = '" + sender + "'");

         RestartServerAndReacquireCom();
         _antiSpam = _settings.AntiSpam;
         _hardened = _antiSpam;

         Assert.AreEqual("250", Attempt(sender, account.Address),
            "the row from before the step is found by its first 200 characters, and its window has passed");

         // If the rewrite did not happen, this deletes the triplet and the next
         // attempt is a newcomer's 451.
         _application.Database.ExecuteSQL("delete from hm_greylisting_triplets where glsenderhash is null");

         Assert.AreEqual("250", Attempt(sender, account.Address),
            "the row carries its key now, and nothing else is left to find it by");

         Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test");
      }

      // ------------------------------------------------------------------- helpers

      /// <summary>
      ///    One RCPT TO over a raw socket after HELO and MAIL FROM, and if it is
      ///    accepted the whole of a small message after it: the three-digit code of
      ///    the reply that ended the attempt - 451 or 550 at RCPT TO, or the reply to
      ///    the end of DATA.
      /// </summary>
      private static string Attempt(string sender, string recipient, string helo = "client.example.test")
      {
         string rcptReply, dataReply;
         Converse(sender, recipient, "Subject: greylisting\r\n\r\nA body.\r\n", out rcptReply, out dataReply, helo);

         if (!rcptReply.StartsWith("250"))
            return rcptReply.Substring(0, 3);

         return dataReply.Substring(0, 3);
      }

      /// <summary>
      ///    One attempt carrying a message of the caller's own - a captured,
      ///    signed one - rather than the three lines Attempt sends. The code of
      ///    the reply that ended it, as Attempt answers.
      /// </summary>
      private static string AttemptWith(string sender, string recipient, string message, string helo = "client.example.test")
      {
         string rcptReply, dataReply;
         Converse(sender, recipient, message, out rcptReply, out dataReply, helo);

         if (!rcptReply.StartsWith("250"))
            return rcptReply.Substring(0, 3);

         return dataReply.Substring(0, 3);
      }

      private static void Converse(string sender, string recipient, string message, out string rcptReply, out string dataReply, string helo = "client.example.test")
      {
         var socket = new TcpConnection();
         Assert.IsTrue(socket.Connect(25), "Could not connect to SMTP.");

         try
         {
            socket.ReadUntil("220");

            socket.Send("HELO " + helo + "\r\n");
            socket.ReadUntil("250");

            socket.Send("MAIL FROM:<" + sender + ">\r\n");
            socket.ReadUntil("250");

            socket.Send("RCPT TO:<" + recipient + ">\r\n");
            rcptReply = socket.Receive();
            dataReply = "";

            if (rcptReply.StartsWith("250"))
            {
               socket.Send("DATA\r\n");
               socket.ReadUntil("354");

               socket.Send(message);
               if (!message.EndsWith("\r\n"))
                  socket.Send("\r\n");
               socket.Send(".\r\n");

               dataReply = socket.Receive();
            }

            socket.Send("QUIT\r\n");
         }
         finally
         {
            socket.Disconnect();
         }
      }

      private static void SleepUntil(DateTime when)
      {
         TimeSpan left = when - DateTime.UtcNow;
         if (left > TimeSpan.Zero)
            Thread.Sleep(left);
      }

      private void StartRest()
      {
         _settings.SetAdministratorPassword(AdminPassword);
         RestListener.Start(RestPort);
         restStarted_ = true;
         _application.Reinitialize();

         (int status, string body) probe = Http("GET", "/api/v1/status");
         Assert.AreEqual(200, probe.status, "REST API did not answer /api/v1/status. Body: " + probe.body);
      }

      private long AddBypass(string json)
      {
         (int status, string body) created = Http("POST", "/api/v1/greylisting-bypass", json);
         Assert.AreEqual(201, created.status, created.body);

         long id = MiniJson.Number(MiniJson.Object(created.body), "id");
         bypassEntriesMade_.Add(id);
         return id;
      }

      private static (int status, string body) Http(string method, string path, string requestBody = null)
      {
         string credentials = Convert.ToBase64String(Encoding.ASCII.GetBytes("Administrator:" + AdminPassword));
         return RestRequest.Send("127.0.0.1", RestListener.Provided > 0 ? RestListener.Provided : RestPort, method, path, "Basic " + credentials, requestBody);
      }
   }
}
