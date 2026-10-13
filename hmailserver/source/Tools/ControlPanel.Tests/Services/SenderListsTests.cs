// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// "Which list do I need?" and "Check a sender" on the spam filtering overview
   /// (review finding F269): eight lists let a sender through or stop one, each page
   /// explained at most one neighbour, and nothing said whether a sender was on any
   /// list. The guide names every list once, in the order a message meets them; the
   /// check matches each list as the server's own code does, which these tests hold
   /// it to case by case.
   /// </summary>
   [Collection("Loc")]
   public class SenderListsTests : IDisposable
   {
      public void Dispose() => Loc.Reset();

      [Fact]
      public void TheGuideNamesEveryListOnceInTheOrderAMessageMeetsThem()
      {
         string[] order = { "spamwhitelist", "domainpolicylist", "blockedsenders", "dnsbl", "greylistwhitelist", "greylistbypass", "surbl", "senderlist" };
         Assert.Equal(order, SenderListGuide.Rows.Select(r => r.Page).ToArray());

         foreach (SenderListGuideRow row in SenderListGuide.Rows)
         {
            NavNode page = NavigationMap.Find(row.Page);
            Assert.NotNull(page);
            Assert.Equal("Spam filtering", page.Parent.Title);
            Assert.False(string.IsNullOrWhiteSpace(row.When));
            Assert.False(string.IsNullOrWhiteSpace(row.Matches));
            Assert.False(string.IsNullOrWhiteSpace(row.Does));
         }

         // Two of the eight can do either, as the card's lead says: the allowed and
         // blocked domains and the allow and block list each say what an allowed entry
         // does and what a blocked one does.
         Assert.Contains("Skips every spam check", SenderListGuide.Rows[0].Does, StringComparison.Ordinal);
         Assert.Contains("refused at MAIL FROM", SenderListGuide.Rows[1].Does, StringComparison.Ordinal);
         string[] either = SenderListGuide.Rows.Where(r => r.Does.StartsWith("Allowed:", StringComparison.Ordinal) && r.Does.Contains(" Blocked: ", StringComparison.Ordinal))
            .Select(r => r.Page).ToArray();
         Assert.Equal(new[] { "domainpolicylist", "senderlist" }, either);
         string view = ReadView_("SpamOverviewView.cs");
         Assert.Contains("L(\"Eight lists decide whether a sender is let through or stopped - two of them can do either - ", view, StringComparison.Ordinal);
         Assert.DoesNotContain("Five lists let a sender through and three stop one", view, StringComparison.Ordinal);
      }

      // ---- the spam exemptions: WhiteListCache::IsWhitelisted -----------------------

      [Theory]
      [InlineData("10.0.0.5", "anyone@example.com", SenderCheckOutcome.Matches)]
      [InlineData("10.0.1.5", "anyone@example.com", SenderCheckOutcome.NoMatch)]
      [InlineData("", "anyone@example.com", SenderCheckOutcome.MatchesIf)]
      public void ASpamExemptionNeedsItsRange(string ip, string address, SenderCheckOutcome expected)
      {
         var entries = new[] { new SenderCheck.SpamExemption("10.0.0.0", "10.0.0.255", "*", "Office") };
         Assert.Equal(expected, SenderCheck.SpamExemptions(entries, address, ip).Outcome);
      }

      [Theory]
      [InlineData("boss@partner.test", SenderCheckOutcome.Matches)]
      [InlineData("BOSS@Partner.TEST", SenderCheckOutcome.Matches)]
      [InlineData("boss@other.test", SenderCheckOutcome.NoMatch)]
      [InlineData("", SenderCheckOutcome.MatchesIf)]
      public void ASpamExemptionsAddressIsAWildcard(string address, SenderCheckOutcome expected)
      {
         // The whole IPv4 space: an address alone decides.
         var entries = new[] { new SenderCheck.SpamExemption("0.0.0.0", "255.255.255.255", "*@partner.test", "") };
         SenderCheckResult result = SenderCheck.SpamExemptions(entries, address, "");
         Assert.Equal(expected, result.Outcome);
      }

      [Fact]
      public void AnEmptyExemptionAddressCoversEveryAddress()
      {
         var entries = new[] { new SenderCheck.SpamExemption("192.0.2.1", "192.0.2.1", "", "") };
         Assert.Equal(SenderCheckOutcome.Matches, SenderCheck.SpamExemptions(entries, "", "192.0.2.1").Outcome);
         Assert.Equal(SenderCheckOutcome.NoMatch, SenderCheck.SpamExemptions(entries, "", "192.0.2.2").Outcome);
         Assert.Equal(SenderCheckOutcome.NoMatch, SenderCheck.SpamExemptions(entries, "", "2001:db8::1").Outcome);
      }

      // ---- the blocked senders: SenderPattern::Matches, the highest score ------------

      [Theory]
      [InlineData("spammer@bad.test", "spammer@bad.test", true)]
      [InlineData("Spammer@Bad.Test", "spammer@bad.test", true)]
      [InlineData("other@bad.test", "spammer@bad.test", false)]
      [InlineData("x@bad.test", "bad.test", true)]
      [InlineData("x@mail.bad.test", "bad.test", true)]
      [InlineData("x@notbad.test", "bad.test", false)]
      [InlineData("x@bad.test", "@bad.test", true)]
      [InlineData("x@bad.test", "https://bad.test/", false)]
      [InlineData("", "bad.test", false)]
      public void APatternIsOneAddressOrADomainWithItsSubdomains(string address, string pattern, bool matches)
      {
         Assert.Equal(matches, SenderCheck.PatternMatches(pattern, address));
      }

      [Fact]
      public void TheBlockedSenderWithTheHighestScoreDecides()
      {
         var entries = new[] { new SenderCheck.BlockedSender("bad.test", 5), new SenderCheck.BlockedSender("x@bad.test", 100) };
         SenderCheckResult result = SenderCheck.BlockedSenders(entries, "x@bad.test");
         Assert.Equal(SenderCheckOutcome.Matches, result.Outcome);
         Assert.True(result.Blocks);
         Assert.Contains("x@bad.test", result.Text, StringComparison.Ordinal);
         Assert.Contains("100", result.Text, StringComparison.Ordinal);
         Assert.Equal(SenderCheckOutcome.NotChecked, SenderCheck.BlockedSenders(entries, "").Outcome);
      }

      // ---- the allow and block list: specificity, then a block, and expiry ------------

      [Fact]
      public void TheMostSpecificEntryDecidesAndABlockWinsATie()
      {
         var now = new DateTime(2026, 10, 10, 12, 0, 0);
         var entries = new[]
         {
            new SenderCheck.SenderListEntry("example.test", true, false, DateTime.MinValue, ""),
            new SenderCheck.SenderListEntry("boss@example.test", false, false, DateTime.MinValue, "")
         };
         Assert.False(SenderCheck.SenderList(entries, "boss@example.test", now).Blocks);
         Assert.True(SenderCheck.SenderList(entries, "other@example.test", now).Blocks);

         var tie = new[]
         {
            new SenderCheck.SenderListEntry("example.test", false, false, DateTime.MinValue, ""),
            new SenderCheck.SenderListEntry("@example.test", true, false, DateTime.MinValue, "")
         };
         Assert.True(SenderCheck.SenderList(tie, "x@example.test", now).Blocks);
      }

      [Fact]
      public void AnExpiredEntryIsSkipped()
      {
         var now = new DateTime(2026, 10, 10, 12, 0, 0);
         var entries = new[] { new SenderCheck.SenderListEntry("example.test", true, true, now.AddMinutes(-1), "") };
         Assert.Equal(SenderCheckOutcome.NoMatch, SenderCheck.SenderList(entries, "x@example.test", now).Outcome);
         var live = new[] { new SenderCheck.SenderListEntry("example.test", true, true, now.AddDays(1), "") };
         Assert.Equal(SenderCheckOutcome.Matches, SenderCheck.SenderList(live, "x@example.test", now).Outcome);
      }

      [Theory]
      [InlineData("sub.example.test", 4)]
      [InlineData("example.test", 3)]
      [InlineData("a@example.test", 1000)]
      [InlineData("", 0)]
      public void SpecificityIsTheServers(string pattern, int expected)
      {
         Assert.Equal(expected, SenderCheck.Specificity(pattern));
      }

      // ---- the greylisting exemptions: the address against * and ? ------------------

      [Theory]
      [InlineData("10.78.0.*", "10.78.0.200", true)]
      [InlineData("10.78.0.?", "10.78.0.5", true)]
      [InlineData("10.78.0.?", "10.78.0.55", false)]
      [InlineData("10.78.0.1", "10.78.0.10", false)]
      public void AGreylistingExemptionIsAWildcard(string pattern, string ip, bool matches)
      {
         SenderCheckResult result = SenderCheck.GreylistingExemptions(new[] { pattern }, ip);
         Assert.Equal(matches ? SenderCheckOutcome.Matches : SenderCheckOutcome.NoMatch, result.Outcome);
      }

      [Fact]
      public void AGreylistingExemptionNeedsAnAddress()
      {
         Assert.Equal(SenderCheckOutcome.NotChecked, SenderCheck.GreylistingExemptions(new[] { "*" }, "").Outcome);
      }

      // ---- the greylisting bypass list: GreyListingBypassEntry::Covers ------------------

      [Theory]
      [InlineData(".example.test", "example.test", true)]
      [InlineData(".example.test", "mail.example.test", true)]
      [InlineData(".example.test", "notexample.test", false)]
      [InlineData("example.test", "mail.example.test", false)]
      [InlineData("Example.Test", "example.test", true)]
      public void ABypassEntryCoversItsDomainOrSuffix(string pattern, string name, bool covers)
      {
         Assert.Equal(covers, SenderCheck.BypassCovers(pattern, name));
      }

      [Fact]
      public void ABypassEntryThatNeedsAuthenticationLetsThroughOnlyIfItPasses()
      {
         var spf = new[] { new SenderCheck.BypassEntry(".partner.test", SenderCheck.MatchEnvelope, true, false, false) };
         SenderCheckResult result = SenderCheck.GreylistingBypass(spf, "a@mail.partner.test");
         Assert.Equal(SenderCheckOutcome.MatchesIf, result.Outcome);
         Assert.Contains("SPF", result.Text, StringComparison.Ordinal);

         var helo = new[] { new SenderCheck.BypassEntry(".partner.test", SenderCheck.MatchHelo, false, false, false) };
         Assert.Equal(SenderCheckOutcome.NoMatch, SenderCheck.GreylistingBypass(helo, "a@partner.test").Outcome);

         var none = new[] { new SenderCheck.BypassEntry("partner.test", SenderCheck.MatchBoth, false, false, false) };
         Assert.Equal(SenderCheckOutcome.Matches, SenderCheck.GreylistingBypass(none, "a@partner.test").Outcome);
      }

      // ---- the allowed and blocked domains: DomainPolicyList::Decide -------------------

      private const string Policy =
         "{\"items\":[" +
         "{\"pattern\":\".example.test\",\"match_on\":\"both\",\"action\":\"white\"}," +
         "{\"pattern\":\"spam.example.test\",\"match_on\":\"envelope\",\"action\":\"black\"}," +
         "{\"pattern\":\".helo.test\",\"match_on\":\"helo\",\"action\":\"black\"}]}";

      [Theory]
      [InlineData("a@example.test", SenderCheckOutcome.Matches, false)]
      [InlineData("a@mail.example.test", SenderCheckOutcome.Matches, false)]
      [InlineData("a@spam.example.test", SenderCheckOutcome.Matches, true)]
      [InlineData("a@helo.test", SenderCheckOutcome.NoMatch, false)]
      [InlineData("a@other.test", SenderCheckOutcome.NoMatch, false)]
      public void AnExactDomainBeatsASuffix(string address, SenderCheckOutcome outcome, bool blocks)
      {
         SenderCheckResult result = SenderCheck.DomainPolicy(Policy, address);
         Assert.Equal(outcome, result.Outcome);
         Assert.Equal(blocks, result.Blocks);
      }

      [Fact]
      public void ADocumentThatCannotBeReadIsAListOfNothing()
      {
         Assert.Empty(SenderCheck.DomainPolicyRows("not json"));
         Assert.Equal(SenderCheckOutcome.NoMatch, SenderCheck.DomainPolicy("", "a@example.test").Outcome);
         Assert.Equal(SenderCheckOutcome.NotChecked, SenderCheck.DomainPolicy(Policy, "").Outcome);
      }

      // ---- the network's lists, and a list that cannot be read --------------------------

      [Fact]
      public void TheNetworksListsAreNotCheckedHereAndAFailedReadSaysWhy()
      {
         Assert.Equal(SenderCheckOutcome.NotChecked, SenderCheck.AskedOfTheNetwork("dnsbl").Outcome);
         Assert.Contains("SURBL", SenderCheck.AskedOfTheNetwork("surbl").Text, StringComparison.Ordinal);
         SenderCheckResult failed = SenderCheck.NotRead("senderlist", "the link went");
         Assert.Equal(SenderCheckOutcome.NotRead, failed.Outcome);
         Assert.EndsWith("the link went", failed.Text, StringComparison.Ordinal);
      }

      // ---- the order the server reads the lists in ---------------------------------

      /// <summary>
      /// The lists of the overview's own render: the office network 10.0.0.0 -
      /// 10.255.255.255 is a spam exemption, mass-mail.example a blocked domain (or an
      /// allowed one) and a blocked sender of score 100, one address at mass-mail.example
      /// is on the allow and block list, and one provider's address is a greylisting
      /// exemption. Every answer is the list's own, asked on its own, in the guide's order.
      /// </summary>
      private static List<SenderCheckResult> Answers_(string address, string ip, string domainAction = "black")
      {
         var now = new DateTime(2026, 10, 10, 12, 0, 0);
         return new List<SenderCheckResult>
         {
            SenderCheck.SpamExemptions(new[] { new SenderCheck.SpamExemption("10.0.0.0", "10.255.255.255", "*", "The office network") }, address, ip),
            SenderCheck.DomainPolicy("{\"items\":[{\"pattern\":\"mass-mail.example\",\"match_on\":\"both\",\"action\":\"" + domainAction + "\"}]}", address),
            SenderCheck.BlockedSenders(new[] { new SenderCheck.BlockedSender("mass-mail.example", 100) }, address),
            SenderCheck.AskedOfTheNetwork("dnsbl"),
            SenderCheck.GreylistingExemptions(new[] { "203.0.113.25" }, ip),
            SenderCheck.GreylistingBypass(new[] { new SenderCheck.BypassEntry(".mass-mail.example", SenderCheck.MatchEnvelope, false, false, false) }, address),
            SenderCheck.AskedOfTheNetwork("surbl"),
            SenderCheck.SenderList(new[] { new SenderCheck.SenderListEntry("spammer@mass-mail.example", true, false, DateTime.MinValue, "") }, address, now)
         };
      }

      private static SenderCheckResult At_(IReadOnlyList<SenderCheckResult> answers, string page) => answers.Single(answer => answer.Page == page);

      /// <summary>
      /// news@mass-mail.example from 10.1.2.3, as the render had it: the exemption read
      /// "Lets it through" above "Stops it" on the blocked domain, refused at MAIL FROM,
      /// and on the blocked senders, adding 100 to the score, where the server accepts
      /// the message with no check at all. A spam exemption sets the session's spam
      /// protection to none (SMTPConnection::InitializeSpamProtectionType_), so the domain
      /// policy is not consulted (CheckDomainPolicy_) and no spam test or greylisting runs
      /// (GetDoSpamProtection_); the allow and block list still applies at delivery.
      /// </summary>
      [Fact]
      public void ASpamExemptionKeepsTheServerFromEveryListButTheAllowAndBlockList()
      {
         List<SenderCheckResult> asked = Answers_("news@mass-mail.example", "10.1.2.3");
         Assert.Equal(SenderCheckOutcome.Matches, At_(asked, "domainpolicylist").Outcome);
         Assert.True(At_(asked, "domainpolicylist").Blocks);
         Assert.True(At_(asked, "blockedsenders").Blocks);

         IReadOnlyList<SenderCheckResult> answers = SenderCheck.ApplyPrecedence(asked);
         Assert.Equal(SenderListGuide.Rows.Select(row => row.Page), answers.Select(answer => answer.Page));
         Assert.Equal(SenderCheckOutcome.Matches, At_(answers, "spamwhitelist").Outcome);
         Assert.Equal("Lets it through", SenderCheck.Pill(At_(answers, "spamwhitelist")).Word);
         foreach (string page in new[] { "domainpolicylist", "blockedsenders", "dnsbl", "greylistwhitelist", "greylistbypass", "surbl" })
         {
            SenderCheckResult answer = At_(answers, page);
            Assert.Equal(SenderCheckOutcome.NotReached, answer.Outcome);
            Assert.False(answer.Blocks);
            Assert.Equal((StatusLevel.Normal, "Not reached"), SenderCheck.Pill(answer));
            Assert.StartsWith("Not consulted: the spam exemption covers this sender", answer.Text, StringComparison.Ordinal);
         }

         // The allow and block list is read at delivery whatever the exemption says.
         Assert.Same(asked[7], At_(answers, "senderlist"));
         Assert.DoesNotContain(answers, answer => SenderCheck.Pill(answer).Word == "Stops it");

         // And when the list blocks the address, it is still the list that says so.
         IReadOnlyList<SenderCheckResult> spammer = SenderCheck.ApplyPrecedence(Answers_("spammer@mass-mail.example", "10.1.2.3"));
         Assert.Equal("Stops it", SenderCheck.Pill(At_(spammer, "senderlist")).Word);
      }

      /// <summary>
      /// The same sender from outside the office network: the blocked domain refuses it at
      /// MAIL FROM (CheckDomainPolicy_), so no list after it is read - the blocked senders,
      /// the greylisting lists and the allow and block list among them.
      /// </summary>
      [Fact]
      public void ABlockedDomainIsRefusedBeforeAnyLaterList()
      {
         IReadOnlyList<SenderCheckResult> answers = SenderCheck.ApplyPrecedence(Answers_("news@mass-mail.example", "192.0.2.7"));
         Assert.Equal(SenderCheckOutcome.NoMatch, At_(answers, "spamwhitelist").Outcome);
         Assert.Equal((StatusLevel.Warning, "Stops it"), SenderCheck.Pill(At_(answers, "domainpolicylist")));
         Assert.Contains("refused at MAIL FROM", At_(answers, "domainpolicylist").Text, StringComparison.Ordinal);
         foreach (string page in new[] { "blockedsenders", "dnsbl", "greylistwhitelist", "greylistbypass", "surbl", "senderlist" })
         {
            Assert.Equal(SenderCheckOutcome.NotReached, At_(answers, page).Outcome);
            Assert.Equal("Not reached: the message is refused at MAIL FROM.", At_(answers, page).Text);
         }
      }

      /// <summary>
      /// The same sender with its domain allowed: an allowed domain is never greylisted
      /// (the RCPT TO gate reads DomainPolicyWhite_), and no score refuses or quarantines
      /// it (DoSpamProtection_), though the tests run - so the blocked sender adds 100 to
      /// the score, which can mark the message as spam and no longer stops it.
      /// </summary>
      [Fact]
      public void AnAllowedDomainIsNeverGreylistedAndNoScoreRefusesIt()
      {
         List<SenderCheckResult> asked = Answers_("news@mass-mail.example", "203.0.113.25", "white");
         Assert.Equal(SenderCheckOutcome.Matches, At_(asked, "greylistwhitelist").Outcome);
         Assert.Equal(SenderCheckOutcome.Matches, At_(asked, "greylistbypass").Outcome);

         IReadOnlyList<SenderCheckResult> answers = SenderCheck.ApplyPrecedence(asked);
         Assert.Equal((StatusLevel.Good, "Lets it through"), SenderCheck.Pill(At_(answers, "domainpolicylist")));
         foreach (string page in new[] { "greylistwhitelist", "greylistbypass" })
         {
            Assert.Equal(SenderCheckOutcome.NotReached, At_(answers, page).Outcome);
            Assert.Equal("Not consulted: the allowed domain is never greylisted.", At_(answers, page).Text);
         }

         SenderCheckResult blocked = At_(answers, "blockedsenders");
         Assert.Equal(SenderCheckOutcome.AddsToScore, blocked.Outcome);
         Assert.False(blocked.Blocks);
         Assert.Equal((StatusLevel.Warning, "Adds to the score"), SenderCheck.Pill(blocked));
         Assert.Equal("The entry mass-mail.example matches, adding 100 to the score. The allowed domain means the score cannot refuse or quarantine the message, though it can still mark it as spam.", blocked.Text);

         // The lists the allowed domain does not touch answer as they did.
         Assert.Same(asked[0], At_(answers, "spamwhitelist"));
         Assert.Same(asked[3], At_(answers, "dnsbl"));
         Assert.Same(asked[6], At_(answers, "surbl"));
         Assert.Same(asked[7], At_(answers, "senderlist"));
      }

      /// <summary>
      /// Nothing that decides - no exemption, no listed domain - and each list says what
      /// it says; an exemption that covers the sender only on a condition not asked about
      /// decides nothing yet either, and the domain's block then holds for the rest.
      /// </summary>
      [Fact]
      public void WhereNoListDecidesEachAnswersForItself()
      {
         List<SenderCheckResult> asked = Answers_("someone@example.org", "192.0.2.7");
         Assert.True(asked.SequenceEqual(SenderCheck.ApplyPrecedence(asked)), "An answer changed where no list decides.");

         IReadOnlyList<SenderCheckResult> addressOnly = SenderCheck.ApplyPrecedence(Answers_("news@mass-mail.example", ""));
         Assert.Equal(SenderCheckOutcome.MatchesIf, At_(addressOnly, "spamwhitelist").Outcome);
         Assert.Equal(SenderCheckOutcome.Matches, At_(addressOnly, "domainpolicylist").Outcome);
         Assert.Equal(SenderCheckOutcome.NotReached, At_(addressOnly, "blockedsenders").Outcome);
      }

      // ---- the line that says which list decides --------------------------------------

      /// <summary>
      /// Above the rows, the list that decides, at its level, with its sentence - the line
      /// a screen reader hears once a check is done, where the eight rows it added were
      /// not a live region and it heard nothing.
      /// </summary>
      [Fact]
      public void TheSummarySaysWhichListDecides()
      {
         // The exemption covers the office network's sender, and nothing blocks it at delivery.
         (StatusLevel level, string text) = SenderCheck.Summary(SenderCheck.ApplyPrecedence(Answers_("news@mass-mail.example", "10.1.2.3")));
         Assert.Equal(StatusLevel.Good, level);
         Assert.StartsWith("The list that decides is Spam exemptions. The exemption \"The office network\"", text, StringComparison.Ordinal);

         // The exemption covers it, but the allow and block list blocks the address at delivery.
         (level, text) = SenderCheck.Summary(SenderCheck.ApplyPrecedence(Answers_("spammer@mass-mail.example", "10.1.2.3")));
         Assert.Equal(StatusLevel.Warning, level);
         Assert.StartsWith("The list that decides is Allow and block list. The entry spammer@mass-mail.example blocks this sender", text, StringComparison.Ordinal);

         // From outside the office network the blocked domain refuses it at MAIL FROM.
         (level, text) = SenderCheck.Summary(SenderCheck.ApplyPrecedence(Answers_("news@mass-mail.example", "192.0.2.7")));
         Assert.Equal(StatusLevel.Warning, level);
         Assert.Equal("The list that decides is Allowed and blocked domains. The entry mass-mail.example blocks this domain: its mail is refused at MAIL FROM.", text);

         // With the domain allowed, the blocked sender only adds to the score, and the allowed domain decides.
         (level, text) = SenderCheck.Summary(SenderCheck.ApplyPrecedence(Answers_("news@mass-mail.example", "192.0.2.7", "white")));
         Assert.Equal(StatusLevel.Good, level);
         Assert.StartsWith("The list that decides is Allowed and blocked domains. The entry mass-mail.example allows this domain", text, StringComparison.Ordinal);

         // The address alone: the exemption decides if the sender connects from its range.
         (level, text) = SenderCheck.Summary(SenderCheck.ApplyPrecedence(Answers_("news@mass-mail.example", "")));
         Assert.Equal(StatusLevel.Information, level);
         Assert.StartsWith("The list that decides is Spam exemptions, if its condition holds. The exemption \"The office network\"", text, StringComparison.Ordinal);

         // No list says anything: the spam checks decide.
         (level, text) = SenderCheck.Summary(SenderCheck.ApplyPrecedence(Answers_("someone@example.org", "192.0.2.7")));
         Assert.Equal(StatusLevel.Information, level);
         Assert.Equal("No list decides for this sender: the spam checks and their score do.", text);
      }

      /// <summary>
      /// A blocked sender whose score refuses the message decides before the allow and
      /// block list, which a refused message never reaches; a list that could not be read
      /// leaves the answer open, at Warning at least.
      /// </summary>
      [Fact]
      public void ABlockedSenderDecidesAndAListNotReadLeavesTheAnswerOpen()
      {
         List<SenderCheckResult> asked = Answers_("news@mass-mail.example", "192.0.2.7");
         asked[1] = SenderCheck.DomainPolicy("", "news@mass-mail.example");
         (StatusLevel level, string text) = SenderCheck.Summary(SenderCheck.ApplyPrecedence(asked));
         Assert.Equal(StatusLevel.Warning, level);
         Assert.Equal("The list that decides is Blocked senders. The entry mass-mail.example matches, adding 100 to the score.", text);

         asked = Answers_("someone@example.org", "192.0.2.7");
         asked[7] = SenderCheck.NotRead("senderlist", "the link went");
         (level, text) = SenderCheck.Summary(SenderCheck.ApplyPrecedence(asked));
         Assert.Equal(StatusLevel.Warning, level);
         Assert.Equal("No list decides for this sender: the spam checks and their score do. A list could not be read, so the answer may change once it can be.", text);
      }

      [Fact]
      public void EveryAnswerHasItsPill()
      {
         Assert.Equal((StatusLevel.Normal, "No match"), SenderCheck.Pill(SenderCheck.BlockedSenders(Array.Empty<SenderCheck.BlockedSender>(), "a@example.test")));
         Assert.Equal((StatusLevel.Normal, "Not checked"), SenderCheck.Pill(SenderCheck.AskedOfTheNetwork("dnsbl")));
         Assert.Equal((StatusLevel.Critical, "Not read"), SenderCheck.Pill(SenderCheck.NotRead("senderlist", "the link went")));
         Assert.Equal((StatusLevel.Information, "Depends"), SenderCheck.Pill(SenderCheck.SpamExemptions(new[] { new SenderCheck.SpamExemption("10.0.0.0", "10.0.0.255", "*", "") }, "a@example.test", "")));
      }

      /// <summary>
      /// The overview draws the guide in place of "The lists behind the checks", and
      /// the check card asks each list in the guide's order, a list that cannot be
      /// read saying so while the others answer.
      /// </summary>
      [Fact]
      public void TheOverviewHasTheGuideAndTheCheck()
      {
         string view = ReadView_("SpamOverviewView.cs");
         Assert.DoesNotContain("L(\"The lists behind the checks\")", view, StringComparison.Ordinal);
         Assert.Contains("Section(L(\"Which list do I need?\")", view, StringComparison.Ordinal);
         Assert.Contains("foreach (SenderListGuideRow list in SenderListGuide.Rows)", view, StringComparison.Ordinal);
         Assert.Contains("Section(L(\"Check a sender\")", view, StringComparison.Ordinal);
         Assert.Contains("answers.Add(Check_((object)antiSpam, list.Page, address, ip));", view, StringComparison.Ordinal);
         // The rows are the answers as the server acts on them, each with the pill its outcome has.
         Assert.Contains("IReadOnlyList<SenderCheckResult> applied = SenderCheck.ApplyPrecedence(answers);", view, StringComparison.Ordinal);
         Assert.Contains("foreach (SenderCheckResult answer in applied)", view, StringComparison.Ordinal);
         Assert.Contains("(StatusLevel level, string word) = SenderCheck.Pill(result);", view, StringComparison.Ordinal);

         // A check that answered says which list decides in the card's notice, a live
         // region, after the rows are drawn: the keyboard stays in the address box.
         string run = view.Substring(view.IndexOf("private void RunCheck_()", StringComparison.Ordinal));
         run = run.Substring(0, run.IndexOf("private void SayCheck_(", StringComparison.Ordinal));
         int rows = run.IndexOf("checkResults_.Children.Add(CheckRow_(answer));", StringComparison.Ordinal);
         int said = run.IndexOf("SayCheck_(level, summary);", StringComparison.Ordinal);
         Assert.True(rows > 0 && said > rows, "RunCheck_ does not say which list decides once the rows are drawn.");
         Assert.Contains("(StatusLevel level, string summary) = SenderCheck.Summary(applied);", run, StringComparison.Ordinal);
         Assert.Contains("private readonly InlineNotice checkNotice_", view, StringComparison.Ordinal);
         Assert.Contains("return SenderCheck.NotRead(page, ServerSession.DescribeComError(ex));", view, StringComparison.Ordinal);
         Assert.Contains("FormSubmit.Bind(form, checkButton_, checkAddress_, checkIp_);", view, StringComparison.Ordinal);
         Assert.Contains("body_.Children.Add(CheckCard_());", view, StringComparison.Ordinal);
      }

      private static string ReadView_(string file)
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", "Views", file);
            if (File.Exists(candidate))
               return File.ReadAllText(candidate);
            directory = directory.Parent;
         }

         Assert.Fail("ControlPanel/Views/" + file + " could not be found from the test binaries.");
         return null;
      }
   }
}
