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
            Assert.Equal("Spam & virus filtering", page.Parent.Title);
            Assert.False(string.IsNullOrWhiteSpace(row.When));
            Assert.False(string.IsNullOrWhiteSpace(row.Matches));
            Assert.False(string.IsNullOrWhiteSpace(row.Does));
         }

         // Five let a sender through and three stop one, as the card's lead says.
         Assert.Contains("Skips every spam check", SenderListGuide.Rows[0].Does, StringComparison.Ordinal);
         Assert.Contains("refused at MAIL FROM", SenderListGuide.Rows[1].Does, StringComparison.Ordinal);
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
         Assert.Contains("checkResults_.Children.Add(CheckRow_(Check_((object)antiSpam, list.Page, address, ip)));", view, StringComparison.Ordinal);
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
