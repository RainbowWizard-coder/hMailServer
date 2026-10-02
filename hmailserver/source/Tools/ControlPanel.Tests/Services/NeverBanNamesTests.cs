// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Linq;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The Auto-ban page's never-ban list by reverse name draws what
   /// Settings.AutoBanNeverBanNames and Settings.CheckAutoBanNeverBanName return,
   /// which NeverBanNameList::Document and NeverBanNameList::Check write. These pin
   /// that shape - a key renamed on one side fails here rather than drawing an
   /// empty column or a wrong verdict - and the sentence the page says for each
   /// kind of verdict, each beside the case that must not say it.
   /// </summary>
   public class NeverBanNamesTests
   {
      // The shape NeverBanNameList::Document writes.
      private const string List =
         "{\"items\":[" +
         "{\"id\":3,\"pattern\":\".google.com\",\"note\":\"Gmail's outbound hosts\",\"created\":\"2026-10-01 09:00:00\"}," +
         "{\"id\":7,\"pattern\":\"mail.example.net\",\"note\":\"\",\"created\":\"2026-10-01 09:05:00\"}" +
         "]}";

      // The shape NeverBanNameList::Check writes, for a spared address.
      private const string Spared =
         "{\"address\":\"209.85.220.41\",\"reverse_names\":[{\"name\":\"mail-sor-f41.google.com\",\"usable\":true,\"pattern\":\".google.com\",\"resolves_back\":true}]," +
         "\"answered\":true,\"spared\":true,\"name\":\"mail-sor-f41.google.com\",\"pattern\":\".google.com\"," +
         "\"reason\":\"its reverse name mail-sor-f41.google.com resolves back to it and is covered by .google.com on the never-ban list\"}";

      private static string Verdict(string names, bool answered = true)
         => "{\"address\":\"203.0.113.9\",\"reverse_names\":[" + names + "],\"answered\":" + (answered ? "true" : "false") +
            ",\"spared\":false,\"name\":null,\"pattern\":null,\"reason\":\"x\"}";

      [Fact]
      public void ParseList_ReadsEveryRowAndField()
      {
         var rows = NeverBanNames.ParseList(List, out string error);

         Assert.Null(error);
         Assert.Equal(2, rows.Count);
         Assert.Equal(3, rows[0].Id);
         Assert.Equal(".google.com", rows[0].Pattern);
         Assert.Equal("Gmail's outbound hosts", rows[0].Note);
         Assert.Equal("2026-10-01 09:00:00", rows[0].Created);
         Assert.Equal("mail.example.net", rows[1].Pattern);
      }

      [Fact]
      public void ParseList_AnEmptyListIsNoRowsAndNoError()
      {
         var rows = NeverBanNames.ParseList("{\"items\":[]}", out string error);

         Assert.Null(error);
         Assert.Empty(rows);
      }

      [Theory]
      [InlineData("")]
      [InlineData("not json")]
      [InlineData("[]")]
      [InlineData("{\"entries\":[]}")]
      public void ParseList_ADocumentItCannotReadIsAnErrorNotAnException(string json)
      {
         var rows = NeverBanNames.ParseList(json, out string error);

         Assert.NotNull(error);
         Assert.Empty(rows);
      }

      [Fact]
      public void ParseCheck_ReadsTheVerdictAndEveryName()
      {
         NeverBanCheck check = NeverBanNames.ParseCheck(Spared, out string error);

         Assert.Null(error);
         Assert.True(check.Spared);
         Assert.True(check.Answered);
         Assert.Equal("209.85.220.41", check.Address);
         Assert.Equal("mail-sor-f41.google.com", check.Name);
         Assert.Equal(".google.com", check.Pattern);
         NeverBanCheckedName name = check.Names.Single();
         Assert.True(name.Usable);
         Assert.Equal(".google.com", name.Pattern);
         Assert.True(name.ResolvesBack);
      }

      [Fact]
      public void ParseCheck_NullsAreEmptyAndAnUnaskedLookupIsNull()
      {
         NeverBanCheck check = NeverBanNames.ParseCheck(Verdict("{\"name\":\"h.isp.example\",\"usable\":true,\"pattern\":null,\"resolves_back\":null}"), out string error);

         Assert.Null(error);
         Assert.False(check.Spared);
         Assert.Equal("", check.Name);
         Assert.Equal("", check.Pattern);
         Assert.Null(check.Names.Single().ResolvesBack);
      }

      [Theory]
      [InlineData("")]
      [InlineData("{")]
      [InlineData("{\"items\":[]}")]
      public void ParseCheck_ADocumentWithNoVerdictIsAnError(string json)
      {
         Assert.Null(NeverBanNames.ParseCheck(json, out string error));
         Assert.NotNull(error);
      }

      [Fact]
      public void Describe_ASparedAddressNamesItsNameAndPattern()
      {
         string text = NeverBanNames.Describe(NeverBanNames.ParseCheck(Spared, out _));

         Assert.Contains("never auto-banned", text);
         Assert.Contains("mail-sor-f41.google.com", text);
         Assert.Contains(".google.com", text);
      }

      [Fact]
      public void Describe_ACoveredNameThatDoesNotResolveBackIsNotSparedAndSaysWhy()
      {
         string text = NeverBanNames.Describe(NeverBanNames.ParseCheck(
            Verdict("{\"name\":\"mail.google.com\",\"usable\":true,\"pattern\":\".google.com\",\"resolves_back\":false}"), out _));

         Assert.Contains("does not resolve back", text);
         Assert.DoesNotContain("never auto-banned", text);
      }

      [Fact]
      public void Describe_ACoveredNameWhoseLookupWasNotAnsweredSaysSo()
      {
         string text = NeverBanNames.Describe(NeverBanNames.ParseCheck(
            Verdict("{\"name\":\"mail.google.com\",\"usable\":true,\"pattern\":\".google.com\",\"resolves_back\":null}"), out _));

         Assert.Contains("not answered", text);
         Assert.DoesNotContain("does not resolve back", text);
      }

      [Fact]
      public void Describe_NoReverseNamePointsAtTheAddressList()
      {
         string text = NeverBanNames.Describe(NeverBanNames.ParseCheck(Verdict(""), out _));

         Assert.Contains("has no reverse name", text);
         Assert.Contains("AutoBanNeverBan", text);
      }

      [Fact]
      public void Describe_AnUnansweredReverseLookupIsNotCalledNoName()
      {
         string text = NeverBanNames.Describe(NeverBanNames.ParseCheck(Verdict("", answered: false), out _));

         Assert.Contains("was not answered", text);
         Assert.DoesNotContain("has no reverse name", text);
      }

      [Fact]
      public void Describe_AnUncoveredNameSaysWhetherAPatternWouldWork()
      {
         string would = NeverBanNames.Describe(NeverBanNames.ParseCheck(
            Verdict("{\"name\":\"out1.provider.example\",\"usable\":true,\"pattern\":null,\"resolves_back\":true}"), out _));
         string wouldNot = NeverBanNames.Describe(NeverBanNames.ParseCheck(
            Verdict("{\"name\":\"out1.provider.example\",\"usable\":true,\"pattern\":null,\"resolves_back\":false}"), out _));

         Assert.Contains("would spare the address", would);
         Assert.Contains("no pattern would spare", wouldNot);
         Assert.DoesNotContain("no pattern would spare", would);
      }

      [Fact]
      public void Describe_AnUnusableNameSaysTheListNeverMatchesIt()
      {
         string text = NeverBanNames.Describe(NeverBanNames.ParseCheck(
            Verdict("{\"name\":\"203.0.113.9\",\"usable\":false,\"pattern\":null,\"resolves_back\":null}"), out _));

         Assert.Contains("never matches", text);
      }

      [Fact]
      public void Describe_NothingForNothing()
      {
         Assert.Equal("", NeverBanNames.Describe(null));
      }
   }
}
