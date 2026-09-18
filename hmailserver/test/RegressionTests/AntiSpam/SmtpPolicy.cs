// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// SMTP policy the score cannot express: discussions #283, #285 and #286, one
// operator's three proposals, every one of them off by default.
//
// What these tests pin, and why each is the one worth having:
//
//   * SPF's SoftFail, TempError and PermError each have a score of their own,
//     and 0 - the default - is "not scored" exactly as it always was: the same
//     message that a score refuses is accepted with the score at 0, which is the
//     negative control that says a stock installation changed nothing;
//   * a TempError with SPFTempErrorDefer on is answered 451 4.4.3 for the
//     transaction, and with it off the same TempError is accepted (and scored,
//     where a score asks) - the resolver blip defers rather than refuses;
//   * MAIL FROM:<> naming a second recipient is refused at that RCPT TO and the
//     DATA after it is refused too, while a single-recipient null sender is
//     delivered and, with the switch off, so is the multi-recipient one; the
//     optional ban is the auto-ban range, expiring and named for the reason;
//   * the domain list: black by envelope domain and black by HELO suffix are a
//     550 at MAIL FROM, and a domain that merely contains the suffix is not;
//     white skips greylisting and the score's refusal while the message is
//     still delivered.
//
// The new COM members are reached through dynamic, as SpamTraps does: the
// suite's interop is generated from the registered type library, and naming
// them early-bound would not compile against a bench whose server predates
// them.
//
// Every test that bans an address lifts the ban before it returns, and the
// tear-down lifts any that a failure left behind: the suite talks to this
// server from 127.0.0.1, so a ban left in place would refuse every test after
// it. Every policy row this fixture adds carries the fixture's own domains, and
// the tear-down removes those rows by name, not the whole list.

using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Shared;

namespace RegressionTests.AntiSpam
{
   [TestFixture]
   public class SmtpPolicy : TestFixtureBase
   {
      private const string ClientAddress = "127.0.0.1";

      // The suffix every domain this fixture publishes or lists ends in, so the
      // tear-down can tell its rows from anybody else's.
      private const string Suffix = ".smtp-policy.test";

      private const string BanPrefix = "Auto-ban: multi-recipient null sender";

      private int softFailBefore_;
      private int tempErrorBefore_;
      private int permErrorBefore_;
      private bool deferBefore_;
      private bool rejectBefore_;
      private bool banBefore_;
      private int banHoursBefore_;

      [SetUp]
      public new void SetUp()
      {
         dynamic antiSpam = _settings.AntiSpam;

         softFailBefore_ = (int) antiSpam.UseSPFSoftFailScore;
         tempErrorBefore_ = (int) antiSpam.UseSPFTempErrorScore;
         permErrorBefore_ = (int) antiSpam.UseSPFPermErrorScore;
         deferBefore_ = (bool) antiSpam.SPFTempErrorDefer;
         rejectBefore_ = (bool) antiSpam.RejectMultiRcptNullSender;
         banBefore_ = (bool) antiSpam.BanMultiRcptNullSender;
         banHoursBefore_ = (int) antiSpam.BanMultiRcptNullSenderHours;

         LiftEveryNullSenderBan();
         RemoveOurDomainPolicyRows();
      }

      [TearDown]
      public new void TearDown()
      {
         LiftEveryNullSenderBan();
         RemoveOurDomainPolicyRows();

         dynamic antiSpam = _settings.AntiSpam;

         antiSpam.UseSPFSoftFailScore = softFailBefore_;
         antiSpam.UseSPFTempErrorScore = tempErrorBefore_;
         antiSpam.UseSPFPermErrorScore = permErrorBefore_;
         antiSpam.SPFTempErrorDefer = deferBefore_;
         antiSpam.RejectMultiRcptNullSender = rejectBefore_;
         antiSpam.BanMultiRcptNullSender = banBefore_;
         antiSpam.BanMultiRcptNullSenderHours = banHoursBefore_;

         _settings.AntiSpam.UseSPF = false;
         _settings.AntiSpam.GreyListingEnabled = false;
         _settings.AntiSpam.SpamDeleteThreshold = 10000;
         _settings.AntiSpam.SpamMarkThreshold = 10000;

         // Nothing this fixture published may be visible to the fixture after it:
         // the zone is the whole suite's.
         SuiteDns.Reset();
      }

      // ------------------------------------------------------------------ #286: SPF's other results

      [Test]
      [Description("A softfail (~all) adds UseSPFSoftFailScore, and a score at the delete threshold refuses the message at RCPT TO as a fail does. With the score at 0, its default, the same message is accepted - the negative control that a stock installation changed nothing.")]
      public void ASoftFailIsScoredOnItsOwnAndZeroIsNotScored()
      {
         string domain = "softfail" + Suffix;
         SuiteDns.Zone.WithTxt(domain, "v=spf1 ~all");

         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "softfail@example.test", "test");

         _settings.AntiSpam.UseSPF = true;
         _settings.AntiSpam.UseSPFScore = 3;
         _settings.AntiSpam.SpamDeleteThreshold = 5;

         dynamic antiSpam = _settings.AntiSpam;

         antiSpam.UseSPFSoftFailScore = 0;
         string acceptedReply = RcptReply("sender@" + domain, account.Address);
         StringAssert.StartsWith("250", acceptedReply, "With the softfail score at 0 the message must be accepted, as it always was: " + acceptedReply);

         antiSpam.UseSPFSoftFailScore = 5;
         string refusedReply = RcptReply("sender@" + domain, account.Address);
         StringAssert.StartsWith("550", refusedReply, "A softfail score at the delete threshold must refuse at RCPT TO: " + refusedReply);
         StringAssert.Contains("softfail", refusedReply.ToLowerInvariant(), refusedReply);
      }

      [Test]
      [Description("A permerror - here two SPF records, which RFC 7208 section 4.5 makes one - adds UseSPFPermErrorScore, and is never deferred: with SPFTempErrorDefer on it is still a 550 and not a 451, because a retry will not fix a broken record.")]
      public void APermErrorIsScoredOnItsOwnAndNeverDeferred()
      {
         string domain = "permerror" + Suffix;
         SuiteDns.Zone.WithTxt(domain, "v=spf1 -all");
         SuiteDns.Zone.WithTxt(domain, "v=spf1 +all");

         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "permerror@example.test", "test");

         _settings.AntiSpam.UseSPF = true;
         _settings.AntiSpam.SpamDeleteThreshold = 5;

         dynamic antiSpam = _settings.AntiSpam;

         antiSpam.UseSPFPermErrorScore = 0;
         antiSpam.SPFTempErrorDefer = true;
         string acceptedReply = RcptReply("sender@" + domain, account.Address);
         StringAssert.StartsWith("250", acceptedReply, "With the permerror score at 0 the message must be accepted, and a permerror is never deferred: " + acceptedReply);

         antiSpam.UseSPFPermErrorScore = 5;
         string refusedReply = RcptReply("sender@" + domain, account.Address);
         StringAssert.StartsWith("550", refusedReply, "A permerror score at the delete threshold must refuse at RCPT TO: " + refusedReply);
         StringAssert.Contains("permerror", refusedReply.ToLowerInvariant(), refusedReply);
      }

      [Test]
      [Description("A temperror - the zone answers SERVFAIL for the domain - adds UseSPFTempErrorScore when the defer switch is off, and with SPFTempErrorDefer on it is answered 451 4.4.3 for the transaction whatever the score: the resolver blip defers rather than refuses.")]
      public void ATempErrorIsScoredOrDeferred()
      {
         string domain = "temperror" + Suffix;
         SuiteDns.Zone.WithServFail(domain);

         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "temperror@example.test", "test");

         _settings.AntiSpam.UseSPF = true;
         _settings.AntiSpam.SpamDeleteThreshold = 5;

         dynamic antiSpam = _settings.AntiSpam;

         antiSpam.SPFTempErrorDefer = false;
         antiSpam.UseSPFTempErrorScore = 0;
         string acceptedReply = RcptReply("sender@" + domain, account.Address);
         StringAssert.StartsWith("250", acceptedReply, "With the temperror score at 0 and the defer off, the message must be accepted as it always was: " + acceptedReply);

         antiSpam.UseSPFTempErrorScore = 5;
         string refusedReply = RcptReply("sender@" + domain, account.Address);
         StringAssert.StartsWith("550", refusedReply, "A temperror score at the delete threshold must refuse at RCPT TO: " + refusedReply);
         StringAssert.Contains("temperror", refusedReply.ToLowerInvariant(), refusedReply);

         antiSpam.SPFTempErrorDefer = true;
         string deferredReply = RcptReply("sender@" + domain, account.Address);
         StringAssert.StartsWith("451", deferredReply, "With the defer on, a temperror must be answered 451: " + deferredReply);
         StringAssert.Contains("4.4.3", deferredReply, "RFC 7208 section 8.6 names 4.4.3 for it: " + deferredReply);
         StringAssert.Contains("SPF", deferredReply, deferredReply);
      }

      [Test]
      [Description("A pass is still a pass: a policy naming the client with the three new scores raised and the defer on is accepted, delivered, and nothing about the new scoring touches it.")]
      public void APassIsUntouchedByTheNewScores()
      {
         string domain = "pass" + Suffix;
         SuiteDns.Zone.WithTxt(domain, "v=spf1 ip4:" + ClientAddress + " -all");

         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "pass@example.test", "test");

         _settings.AntiSpam.UseSPF = true;
         _settings.AntiSpam.SpamDeleteThreshold = 5;

         dynamic antiSpam = _settings.AntiSpam;
         antiSpam.UseSPFSoftFailScore = 5;
         antiSpam.UseSPFTempErrorScore = 5;
         antiSpam.UseSPFPermErrorScore = 5;
         antiSpam.SPFTempErrorDefer = true;

         SmtpClientSimulator.StaticSend("sender@" + domain, account.Address, "A pass", "Body");

         Pop3ClientSimulator.AssertMessageCount(account.Address, "test", 1);
      }

      // ------------------------------------------------------------------ #283: the null sender

      [Test]
      [Description("With RejectMultiRcptNullSender off, MAIL FROM:<> may name two recipients and the message is accepted for both - the server as it always was.")]
      public void WithTheSwitchOffANullSenderMayNameTwoRecipients()
      {
         var first = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "null-off-1@example.test", "test");
         var second = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "null-off-2@example.test", "test");

         dynamic antiSpam = _settings.AntiSpam;
         antiSpam.RejectMultiRcptNullSender = false;

         List<string> replies = NullSenderSession(new[] { first.Address, second.Address }, sendData: true);

         StringAssert.StartsWith("250", replies[0], replies[0]);
         StringAssert.StartsWith("250", replies[1], replies[1]);
         StringAssert.StartsWith("354", replies[2], "DATA must be invited: " + replies[2]);
         StringAssert.StartsWith("250", replies[3], "The message must be accepted: " + replies[3]);

         Pop3ClientSimulator.AssertMessageCount(first.Address, "test", 1);
         Pop3ClientSimulator.AssertMessageCount(second.Address, "test", 1);
      }

      [Test]
      [Description("With the switch on, the second recipient of a null sender is refused with 550 5.7.1 and the transaction's DATA is refused as well, so the first recipient is not handed the 'bounce' either; and nothing is banned while BanMultiRcptNullSender is off.")]
      public void ASecondRecipientOfANullSenderIsRefusedAndSoIsTheData()
      {
         var first = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "null-on-1@example.test", "test");
         var second = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "null-on-2@example.test", "test");

         dynamic antiSpam = _settings.AntiSpam;
         antiSpam.RejectMultiRcptNullSender = true;
         antiSpam.BanMultiRcptNullSender = false;

         List<string> replies = NullSenderSession(new[] { first.Address, second.Address }, sendData: true);

         StringAssert.StartsWith("250", replies[0], "The first recipient of a null sender is what a delivery report has: " + replies[0]);
         StringAssert.StartsWith("550", replies[1], "The second recipient must be refused: " + replies[1]);
         StringAssert.Contains("5.7.1", replies[1], replies[1]);
         StringAssert.StartsWith("550", replies[2], "DATA must be refused after the lie was told: " + replies[2]);

         ClassicAssert.IsNull(FindNullSenderBan(), "Nothing may be banned while BanMultiRcptNullSender is off.");

         // Nothing reached anybody.
         Pop3ClientSimulator.AssertMessageCount(first.Address, "test", 0);
         Pop3ClientSimulator.AssertMessageCount(second.Address, "test", 0);
      }

      [Test]
      [Description("A single-recipient null sender is never touched: with the switch on, a genuine delivery report shape is accepted and delivered.")]
      public void ASingleRecipientNullSenderIsUntouched()
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "null-single@example.test", "test");

         dynamic antiSpam = _settings.AntiSpam;
         antiSpam.RejectMultiRcptNullSender = true;

         List<string> replies = NullSenderSession(new[] { account.Address }, sendData: true);

         StringAssert.StartsWith("250", replies[0], replies[0]);
         StringAssert.StartsWith("354", replies[1], replies[1]);
         StringAssert.StartsWith("250", replies[2], "A one-recipient null sender must be accepted: " + replies[2]);

         Pop3ClientSimulator.AssertMessageCount(account.Address, "test", 1);
      }

      [Test]
      [Description("One address named twice is one recipient. RFC 5321 allows a client to repeat a RCPT TO, the recipient list deduplicates it, and the count of recipients on the message would have made the repeat look like a second one - which refused a genuine one-recipient report and lost it. The distinct addresses are what count.")]
      public void ANullSenderRepeatingOneRecipientIsUntouched()
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "null-repeat@example.test", "test");

         dynamic antiSpam = _settings.AntiSpam;
         antiSpam.RejectMultiRcptNullSender = true;
         antiSpam.BanMultiRcptNullSender = false;

         // The same address three times, then the message.
         List<string> replies = NullSenderSession(new[] { account.Address, account.Address, account.Address }, sendData: true);

         StringAssert.StartsWith("250", replies[0], replies[0]);
         StringAssert.StartsWith("250", replies[1], "A repeated RCPT TO is one recipient, not two: " + replies[1]);
         StringAssert.StartsWith("250", replies[2], "A repeated RCPT TO is one recipient, not two: " + replies[2]);
         StringAssert.StartsWith("354", replies[3], replies[3]);
         StringAssert.StartsWith("250", replies[4], "The report must be accepted: " + replies[4]);

         ClassicAssert.IsNull(FindNullSenderBan(), "A repeated recipient must not be treated as abuse.");
         Pop3ClientSimulator.AssertMessageCount(account.Address, "test", 1);
      }

      [Test]
      [Description("A second DIFFERENT address after a repeated one is still refused: the repeat does not spend the allowance.")]
      public void ARepeatThenASecondAddressIsStillRefused()
      {
         var first = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "null-repeat-a@example.test", "test");
         var second = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "null-repeat-b@example.test", "test");

         dynamic antiSpam = _settings.AntiSpam;
         antiSpam.RejectMultiRcptNullSender = true;
         antiSpam.BanMultiRcptNullSender = false;

         List<string> replies = NullSenderSession(new[] { first.Address, first.Address, second.Address }, sendData: true);

         StringAssert.StartsWith("250", replies[0], replies[0]);
         StringAssert.StartsWith("250", replies[1], replies[1]);
         StringAssert.StartsWith("550", replies[2], "The second address is what this refuses: " + replies[2]);
      }

      [Test]
      [Description("With BanMultiRcptNullSender on, the connecting address is banned through the auto-ban: an expiring range with the 'Auto-ban: ' prefix the firewall reconciliation and the expiry sweep both look for, named for the reason and the address.")]
      public void TheConnectingAddressIsBannedWhenAsked()
      {
         var first = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "null-ban-1@example.test", "test");
         var second = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "null-ban-2@example.test", "test");

         dynamic antiSpam = _settings.AntiSpam;
         antiSpam.RejectMultiRcptNullSender = true;
         antiSpam.BanMultiRcptNullSender = true;
         antiSpam.BanMultiRcptNullSenderHours = 1;

         try
         {
            List<string> replies = NullSenderSession(new[] { first.Address, second.Address }, sendData: false);

            StringAssert.StartsWith("550", replies[1], replies[1]);

            SecurityRange ban = FindNullSenderBan();

            ClassicAssert.IsNotNull(ban, "No auto-ban range was created for the address that named a second recipient with a null sender.");
            ClassicAssert.IsTrue(ban.Expires, "The ban must expire, as an auto-ban from failed logons does.");
            StringAssert.Contains(ClientAddress, ban.Name, ban.Name);
            ClassicAssert.AreEqual(ClientAddress, ban.LowerIP);
            ClassicAssert.AreEqual(ClientAddress, ban.UpperIP);
         }
         finally
         {
            LiftEveryNullSenderBan();
         }

         // Lifted, so the next session is accepted again.
         string afterwards = RcptReply("sender@" + "after-ban" + Suffix, first.Address);
         StringAssert.StartsWith("250", afterwards, afterwards);
      }

      // ------------------------------------------------------------------ #285: the domain list

      [Test]
      [Description("A black domain is refused at MAIL FROM by its envelope domain, with a 550 5.7.1, before any recipient is named; a domain that merely contains the pattern's text is not.")]
      public void ABlackEnvelopeDomainIsRefusedAtMailFrom()
      {
         string domain = "black-envelope" + Suffix;

         AddRow(domain, "envelope", "black", "Never accept");

         string refused = MailFromReply("sender@" + domain, "mail.somewhere.example");
         StringAssert.StartsWith("550", refused, "A black envelope domain must be refused at MAIL FROM: " + refused);
         StringAssert.Contains("5.7.1", refused, refused);
         StringAssert.Contains(domain, refused, refused);

         // Not a suffix, so the neighbour beneath it is not covered; and not a
         // substring match either.
         string beneath = MailFromReply("sender@sub." + domain, "mail.somewhere.example");
         StringAssert.StartsWith("250", beneath, "example.com covers that domain alone, not what is beneath it: " + beneath);

         string lookalike = MailFromReply("sender@not" + domain, "mail.somewhere.example");
         StringAssert.StartsWith("250", lookalike, "A domain that merely ends in the same text is not covered: " + lookalike);
      }

      [Test]
      [Description("A black suffix matched on the HELO name refuses a session greeting from beneath it and from the apex, and not one from a name that merely ends in the same letters; the envelope sender plays no part.")]
      public void ABlackHeloSuffixIsRefusedAtMailFrom()
      {
         string apex = "black-helo" + Suffix;

         AddRow("." + apex, "helo", "black", "Their outbound hosts change weekly; their HELO does not");

         string beneath = MailFromReply("sender@innocent" + Suffix, "mx3." + apex);
         StringAssert.StartsWith("550", beneath, "A HELO beneath the suffix must be refused: " + beneath);
         StringAssert.Contains("mx3." + apex, beneath, beneath);

         string atApex = MailFromReply("sender@innocent" + Suffix, apex);
         StringAssert.StartsWith("550", atApex, ".example.com covers example.com itself: " + atApex);

         string lookalike = MailFromReply("sender@innocent" + Suffix, "not" + apex);
         StringAssert.StartsWith("250", lookalike, "A HELO that merely ends in the same letters is not beneath the suffix: " + lookalike);

         // The row is keyed on HELO, so the same text as an envelope domain is
         // not refused by it.
         string envelope = MailFromReply("sender@" + apex, "mail.somewhere.example");
         StringAssert.StartsWith("250", envelope, "A row matched on helo says nothing about the envelope: " + envelope);
      }

      [Test]
      [Description("A white domain skips greylisting: the first attempt from an unknown triplet is 451 for anybody else and 250 for a sender whose domain is white, and the message is delivered.")]
      public void AWhiteDomainSkipsGreylisting()
      {
         string white = "white-grey" + Suffix;
         string other = "grey-other" + Suffix;

         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "white-grey@example.test", "test");

         _settings.AntiSpam.ClearGreyListingTriplets();
         _settings.AntiSpam.GreyListingEnabled = true;

         string greylisted = RcptReply("sender@" + other, account.Address);
         StringAssert.StartsWith("451", greylisted, "An unknown triplet must be greylisted for a domain the list says nothing about: " + greylisted);

         AddRow(white, "envelope", "white", "Their mail is wanted");

         SmtpClientSimulator.StaticSend("sender@" + white, account.Address, "White", "Body");

         Pop3ClientSimulator.AssertMessageCount(account.Address, "test", 1);
      }

      [Test]
      [Description("A white domain is not refused on the spam score: a softfail scored at the delete threshold refuses a stranger at RCPT TO and lets the white domain's message through to the mailbox - the tests still ran, the score was simply not the accept gate.")]
      public void AWhiteDomainIsNotRefusedOnTheScore()
      {
         string white = "white-score" + Suffix;
         string other = "score-other" + Suffix;

         SuiteDns.Zone.WithTxt(white, "v=spf1 ~all");
         SuiteDns.Zone.WithTxt(other, "v=spf1 ~all");

         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "white-score@example.test", "test");

         _settings.AntiSpam.UseSPF = true;
         _settings.AntiSpam.SpamDeleteThreshold = 5;

         dynamic antiSpam = _settings.AntiSpam;
         antiSpam.UseSPFSoftFailScore = 5;

         string refused = RcptReply("sender@" + other, account.Address);
         StringAssert.StartsWith("550", refused, "The stranger's softfail must be refused: " + refused);

         AddRow(white, "both", "white", "Their SPF has been broken since 2019 and will stay broken");

         SmtpClientSimulator.StaticSend("sender@" + white, account.Address, "White", "Body");

         Pop3ClientSimulator.AssertMessageCount(account.Address, "test", 1);
      }

      [Test]
      [Description("A white domain is never deferred for an SPF temperror: with SPFTempErrorDefer on, the stranger's SERVFAIL is a 451 and the white domain's message is delivered.")]
      public void AWhiteDomainIsNotDeferred()
      {
         string white = "white-defer" + Suffix;
         string other = "defer-other" + Suffix;

         SuiteDns.Zone.WithServFail(white);
         SuiteDns.Zone.WithServFail(other);

         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "white-defer@example.test", "test");

         _settings.AntiSpam.UseSPF = true;

         dynamic antiSpam = _settings.AntiSpam;
         antiSpam.SPFTempErrorDefer = true;

         string deferred = RcptReply("sender@" + other, account.Address);
         StringAssert.StartsWith("451", deferred, deferred);

         AddRow(white, "envelope", "white", "Wanted whatever their DNS does");

         SmtpClientSimulator.StaticSend("sender@" + white, account.Address, "White", "Body");

         Pop3ClientSimulator.AssertMessageCount(account.Address, "test", 1);
      }

      [Test]
      [Description("The list judges what it is given: a pattern that is not a domain, a word that is not a choice, and a second row for the same pattern and key are refused in words, and the listing carries what was added.")]
      public void TheListJudgesWhatItIsGiven()
      {
         dynamic antiSpam = _settings.AntiSpam;

         COMException notADomain = Assert.Throws<COMException>(() => antiSpam.AddDomainPolicyListEntry("someone@" + "judged" + Suffix, "both", "white", ""));
         StringAssert.Contains("domain", notADomain.Message, notADomain.Message);

         COMException notAWord = Assert.Throws<COMException>(() => antiSpam.AddDomainPolicyListEntry("judged" + Suffix, "sideways", "white", ""));
         StringAssert.Contains("match_on", notAWord.Message, notAWord.Message);

         COMException notAnAction = Assert.Throws<COMException>(() => antiSpam.AddDomainPolicyListEntry("judged" + Suffix, "both", "grey", ""));
         StringAssert.Contains("action", notAnAction.Message, notAnAction.Message);

         int id = (int) antiSpam.AddDomainPolicyListEntry("Judged" + Suffix + ".", "helo", "black", "A note");
         ClassicAssert.Greater(id, 0);

         COMException twice = Assert.Throws<COMException>(() => antiSpam.AddDomainPolicyListEntry("judged" + Suffix, "helo", "white", ""));
         StringAssert.Contains("already", twice.Message, twice.Message);

         Dictionary<string, object> listed = FindRow("judged" + Suffix, "helo");
         ClassicAssert.IsNotNull(listed, "The added row is not in the listing.");
         ClassicAssert.AreEqual("black", listed["action"].ToString());
         ClassicAssert.AreEqual("A note", listed["note"].ToString());
         ClassicAssert.AreEqual("judged" + Suffix, listed["pattern"].ToString(), "The pattern is stored lower-cased and without its trailing dot.");

         ClassicAssert.IsTrue((bool) antiSpam.RemoveDomainPolicyListEntry(id));
         ClassicAssert.IsFalse((bool) antiSpam.RemoveDomainPolicyListEntry(id), "Removing a row that has gone answers false rather than failing.");
      }

      [Test]
      [Description("The settings judge what they are given: the ban length is between 1 and 8760 hours, and what is set is what is read back.")]
      public void TheSettingsJudgeWhatTheyAreGiven()
      {
         dynamic antiSpam = _settings.AntiSpam;

         COMException tooShort = Assert.Throws<COMException>(() => { antiSpam.BanMultiRcptNullSenderHours = 0; });
         StringAssert.Contains("BanMultiRcptNullSenderHours", tooShort.Message, tooShort.Message);

         COMException tooLong = Assert.Throws<COMException>(() => { antiSpam.BanMultiRcptNullSenderHours = 9000; });
         StringAssert.Contains("BanMultiRcptNullSenderHours", tooLong.Message, tooLong.Message);

         antiSpam.BanMultiRcptNullSenderHours = 48;
         antiSpam.UseSPFSoftFailScore = 7;
         antiSpam.UseSPFTempErrorScore = 2;
         antiSpam.UseSPFPermErrorScore = 9;
         antiSpam.SPFTempErrorDefer = true;
         antiSpam.RejectMultiRcptNullSender = true;
         antiSpam.BanMultiRcptNullSender = true;

         ClassicAssert.AreEqual(48, (int) antiSpam.BanMultiRcptNullSenderHours);
         ClassicAssert.AreEqual(7, (int) antiSpam.UseSPFSoftFailScore);
         ClassicAssert.AreEqual(2, (int) antiSpam.UseSPFTempErrorScore);
         ClassicAssert.AreEqual(9, (int) antiSpam.UseSPFPermErrorScore);
         ClassicAssert.IsTrue((bool) antiSpam.SPFTempErrorDefer);
         ClassicAssert.IsTrue((bool) antiSpam.RejectMultiRcptNullSender);
         ClassicAssert.IsTrue((bool) antiSpam.BanMultiRcptNullSender);
      }

      // ---------------------------------------------------------------------- helpers

      private void AddRow(string pattern, string matchOn, string action, string note)
      {
         dynamic antiSpam = _settings.AntiSpam;
         antiSpam.AddDomainPolicyListEntry(pattern, matchOn, action, note);
      }

      private List<Dictionary<string, object>> Rows()
      {
         dynamic antiSpam = _settings.AntiSpam;
         string json = (string) antiSpam.DomainPolicyList;

         Dictionary<string, object> document = MiniJson.Object(json);
         var rows = new List<Dictionary<string, object>>();

         foreach (object item in MiniJson.List(document, "items"))
            rows.Add((Dictionary<string, object>) item);

         return rows;
      }

      private Dictionary<string, object> FindRow(string pattern, string matchOn)
      {
         foreach (Dictionary<string, object> row in Rows())
         {
            if (string.Equals(row["pattern"].ToString(), pattern, StringComparison.OrdinalIgnoreCase) &&
                string.Equals(row["match_on"].ToString(), matchOn, StringComparison.OrdinalIgnoreCase))
               return row;
         }

         return null;
      }

      /// <summary>
      ///    The rows this fixture made, and only those: every one of them names a
      ///    domain under the fixture's own suffix. The list is the operator's, and a
      ///    test that empties it has no way to put back what was on it.
      /// </summary>
      private void RemoveOurDomainPolicyRows()
      {
         dynamic antiSpam = _settings.AntiSpam;

         foreach (Dictionary<string, object> row in Rows())
         {
            string pattern = row["pattern"].ToString();

            if (pattern.EndsWith(Suffix, StringComparison.OrdinalIgnoreCase))
               antiSpam.RemoveDomainPolicyListEntry(Convert.ToInt32(row["id"]));
         }
      }

      private SecurityRange FindNullSenderBan()
      {
         _settings.SecurityRanges.Refresh();

         for (int i = 0; i < _settings.SecurityRanges.Count; i++)
         {
            SecurityRange range = _settings.SecurityRanges[i];

            if (range.Name.StartsWith(BanPrefix, StringComparison.OrdinalIgnoreCase))
               return range;
         }

         return null;
      }

      private void LiftEveryNullSenderBan()
      {
         while (true)
         {
            SecurityRange ban = FindNullSenderBan();

            if (ban == null)
               return;

            ban.Delete();
         }
      }

      /// <summary>
      ///    One session, one MAIL FROM, one RCPT TO, and the reply to the RCPT TO.
      ///    Raw, because the point of these tests is the text of that reply.
      /// </summary>
      private static string RcptReply(string sender, string recipient)
      {
         var socket = new TcpConnection();
         ClassicAssert.IsTrue(socket.Connect(25), "Could not connect to SMTP. An auto-ban left behind by an earlier test refuses this.");

         try
         {
            socket.ReadUntil("220");

            socket.Send("EHLO test\r\n");
            socket.ReadUntil("250 ");

            socket.Send("MAIL FROM:<" + sender + ">\r\n");
            string mailFrom = socket.ReadUntil(new List<string> { "250", "550", "451" });

            if (!mailFrom.StartsWith("250"))
               return mailFrom;

            socket.Send("RCPT TO:<" + recipient + ">\r\n");
            string reply = socket.Receive();

            socket.Send("QUIT\r\n");

            return reply;
         }
         finally
         {
            socket.Disconnect();
         }
      }

      /// <summary>One session greeting with the HELO name given, and the reply to MAIL FROM.</summary>
      private static string MailFromReply(string sender, string heloName)
      {
         var socket = new TcpConnection();
         ClassicAssert.IsTrue(socket.Connect(25), "Could not connect to SMTP. An auto-ban left behind by an earlier test refuses this.");

         try
         {
            socket.ReadUntil("220");

            socket.Send("EHLO " + heloName + "\r\n");
            socket.ReadUntil("250 ");

            socket.Send("MAIL FROM:<" + sender + ">\r\n");
            string reply = socket.Receive();

            socket.Send("QUIT\r\n");

            return reply;
         }
         finally
         {
            socket.Disconnect();
         }
      }

      /// <summary>
      ///    MAIL FROM:&lt;&gt;, one RCPT TO per recipient, and - when asked - DATA with a
      ///    small message. Answers every reply after the MAIL FROM, in order: one per
      ///    recipient, then the reply to DATA, then the reply to the message when DATA
      ///    was invited.
      /// </summary>
      private static List<string> NullSenderSession(string[] recipients, bool sendData)
      {
         var replies = new List<string>();

         var socket = new TcpConnection();
         ClassicAssert.IsTrue(socket.Connect(25), "Could not connect to SMTP. An auto-ban left behind by an earlier test refuses this.");

         try
         {
            socket.ReadUntil("220");

            socket.Send("EHLO test\r\n");
            socket.ReadUntil("250 ");

            socket.Send("MAIL FROM:<>\r\n");
            string mailFrom = socket.ReadUntil("250");
            StringAssert.StartsWith("250", mailFrom, "A null sender is always accepted at MAIL FROM: " + mailFrom);

            foreach (string recipient in recipients)
            {
               socket.Send("RCPT TO:<" + recipient + ">\r\n");
               replies.Add(socket.Receive());
            }

            if (sendData)
            {
               socket.Send("DATA\r\n");
               string data = socket.Receive();
               replies.Add(data);

               if (data.StartsWith("354"))
               {
                  socket.Send("From: <>\r\nTo: <" + recipients[0] + ">\r\nSubject: A report\r\n\r\nThe body.\r\n.\r\n");
                  replies.Add(socket.Receive());
               }
            }

            socket.Send("QUIT\r\n");
         }
         finally
         {
            socket.Disconnect();
         }

         return replies;
      }
   }
}
