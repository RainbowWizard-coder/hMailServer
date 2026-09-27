// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Linq;
using System.Text.Json;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// What the Quarantine page shows and what its four actions send: the groups
   /// by the organization's domain the server gave, folding, the sort inside each
   /// group, the three-state checks, the recipients' summary, and the body and the
   /// answer of Quarantine.Bulk in the shape QuarantineReview.cpp writes them.
   /// </summary>
   public class QuarantineReviewTests
   {
      // Newest first, as the server lists them.
      private static readonly HeldMessage[] Held =
      {
         new() { Id = 6, CreatedTime = "2026-09-18 08:14:00", Sender = "noreply@supply.example", Recipients = "ap@example.test, finance@example.test,ops@example.test",
                 Score = 18, Subject = "Order confirmation 90211", Helo = "supply-example.mail.protection.example", SenderDomain = "supply.example", HeloDomain = "protection.example" },
         new() { Id = 5, CreatedTime = "2026-09-18 08:12:00", Sender = "billing@supply.example", Recipients = "ada@example.test",
                 Score = 22, Subject = "Your invoice is ready", Helo = "supply-example.mail.protection.example", SenderDomain = "supply.example", HeloDomain = "protection.example" },
         new() { Id = 4, CreatedTime = "2026-09-18 07:58:00", Sender = "elijah@factory.example", Recipients = "ada@example.test",
                 Score = 25, Subject = "Re: shipment 44821", Helo = "alt1.aspmx.example", SenderDomain = "factory.example", HeloDomain = "aspmx.example" },
         new() { Id = 3, CreatedTime = "2026-09-17 11:19:00", Sender = "010101a0@bounces.notify.example", Recipients = "ada@example.test,ops@example.test",
                 Score = 21, Subject = "Build notification", Helo = "mx1.notify.example", SenderDomain = "notify.example", HeloDomain = "notify.example" },
         new() { Id = 2, CreatedTime = "2026-09-16 19:03:00", Sender = "", Recipients = "postmaster@example.test",
                 Score = 100, Subject = "Undelivered Mail Returned to Sender", Helo = "", SenderDomain = "", HeloDomain = "" }
      };

      private static QuarantineReview Loaded(QuarantineGrouping grouping = QuarantineGrouping.SenderDomain)
      {
         var review = new QuarantineReview();
         review.Load(Held);
         review.SetGrouping(grouping);
         return review;
      }

      [Fact]
      public void GroupsBySenderDomain_TheGroupWithTheNewestMessageFirst()
      {
         QuarantineReview review = Loaded();

         string[] groups = review.Rows.Where(r => r.IsGroup).Select(r => r.GroupKey).ToArray();
         Assert.Equal(new[] { "supply.example", "factory.example", "notify.example", "" }, groups);

         QuarantineRow supply = review.Rows.First(r => r.IsGroup);
         Assert.Equal(2, supply.Members.Count);
         Assert.Equal("2 messages", supply.GroupCount);
         Assert.Equal("(no domain)", review.Rows.Last(r => r.IsGroup).GroupTitle);

         // Every message is under its group's header, a step in.
         Assert.Equal(9, review.Rows.Count);
         Assert.All(review.Rows.Where(r => r.IsMessage), r => Assert.True(r.Indented));
      }

      [Fact]
      public void GroupsByHeloDomain_AndNotAtAll()
      {
         QuarantineReview review = Loaded(QuarantineGrouping.HeloDomain);
         Assert.Equal(new[] { "protection.example", "aspmx.example", "notify.example", "" },
            review.Rows.Where(r => r.IsGroup).Select(r => r.GroupKey).ToArray());

         review.SetGrouping(QuarantineGrouping.None);
         Assert.Equal(5, review.Rows.Count);
         Assert.All(review.Rows, r => Assert.True(r.IsMessage && !r.Indented));
         Assert.Equal(new[] { 6, 5, 4, 3, 2 }, review.Rows.Select(r => r.Id).ToArray());
      }

      [Fact]
      public void AFoldedGroupHidesItsMessagesAndKeepsTheirChecks()
      {
         QuarantineReview review = Loaded();
         QuarantineRow supply = review.Rows.First(r => r.IsGroup);

         review.Toggle(supply);
         review.ToggleFold(review.Rows.First(r => r.IsGroup));

         Assert.Equal(7, review.Rows.Count);
         Assert.False(review.Rows.First(r => r.IsGroup).IsExpanded);
         Assert.True(review.Rows.First(r => r.IsGroup).IsChecked);

         // Hidden, and still what the header's actions act on.
         Assert.Equal(new[] { 6, 5 }, review.CheckedIds.ToArray());

         review.ToggleFold(review.Rows.First(r => r.IsGroup));
         Assert.Equal(9, review.Rows.Count);
      }

      [Fact]
      public void SortingOrdersTheMessagesInsideEachGroup_NotTheGroups()
      {
         QuarantineReview review = Loaded();

         review.SortBy(QuarantineColumn.Score);
         Assert.True(review.SortDescending);
         Assert.Equal(new[] { "supply.example", "factory.example", "notify.example", "" },
            review.Rows.Where(r => r.IsGroup).Select(r => r.GroupKey).ToArray());
         Assert.Equal(new[] { 5, 6 }, review.Rows.Where(r => r.IsMessage).Take(2).Select(r => r.Id).ToArray());

         review.SortBy(QuarantineColumn.Score);
         Assert.False(review.SortDescending);
         Assert.Equal(new[] { 6, 5 }, review.Rows.Where(r => r.IsMessage).Take(2).Select(r => r.Id).ToArray());

         review.SetGrouping(QuarantineGrouping.None);
         review.SortBy(QuarantineColumn.Subject);
         Assert.False(review.SortDescending);
         Assert.Equal("Build notification", review.Rows[0].Subject);
      }

      [Fact]
      public void SortingByHeldReadsTheTimeNotTheText()
      {
         var review = new QuarantineReview();
         review.Load(new[]
         {
            new HeldMessage { Id = 1, CreatedTime = "2026-09-02 10:00:00" },
            new HeldMessage { Id = 2, CreatedTime = "2026-10-01 09:00:00" },
            new HeldMessage { Id = 3, CreatedTime = "2026-09-10 11:00:00" }
         });
         review.SetGrouping(QuarantineGrouping.None);

         review.SortBy(QuarantineColumn.Held);
         Assert.Equal(new[] { 2, 3, 1 }, review.Rows.Select(r => r.Id).ToArray());
      }

      [Fact]
      public void AGroupsBoxIsCheckedWholeByOneClick_AndShowsWhenSomeAre()
      {
         QuarantineReview review = Loaded();
         QuarantineRow supply = review.Rows.First(r => r.IsGroup);
         QuarantineRow first = review.Rows.First(r => r.IsMessage);

         review.Toggle(first);
         Assert.Null(supply.IsChecked);
         Assert.Null(review.AllChecked);

         review.Toggle(supply);
         Assert.True(supply.IsChecked);
         Assert.All(review.Rows.Where(r => r.IsMessage).Take(2), r => Assert.True(r.IsChecked));

         review.Toggle(supply);
         Assert.False(supply.IsChecked);
         Assert.Empty(review.CheckedIds);
         Assert.False(review.AllChecked);
      }

      [Fact]
      public void SelectAllChecksEverything_AndAgainNothing()
      {
         QuarantineReview review = Loaded();

         review.ToggleAll();
         Assert.True(review.AllChecked);
         Assert.Equal(new[] { 6, 5, 4, 3, 2 }, review.CheckedIds.ToArray());
         Assert.All(review.Rows, r => Assert.True(r.IsChecked));

         review.ToggleAll();
         Assert.False(review.AllChecked);
         Assert.Empty(review.CheckedIds);
      }

      [Fact]
      public void AReloadKeepsTheChecksOfWhatIsStillHeld()
      {
         QuarantineReview review = Loaded();
         review.ToggleAll();

         review.Load(Held.Where(m => m.Id != 5));

         Assert.Equal(new[] { 6, 4, 3, 2 }, review.CheckedIds.ToArray());
         Assert.True(review.AllChecked);
      }

      [Fact]
      public void ARowsIconsActOnTheMessage_AGroupsOnEveryMessageInIt()
      {
         QuarantineReview review = Loaded();

         Assert.Equal(new[] { 6, 5 }, QuarantineReview.IdsOf(review.Rows.First(r => r.IsGroup)).ToArray());
         Assert.Equal(new[] { 6 }, QuarantineReview.IdsOf(review.Rows.First(r => r.IsMessage)).ToArray());
         Assert.Empty(QuarantineReview.IdsOf(null));
      }

      [Fact]
      public void SeveralRecipientsAreTheFirstAndHowManyMore_AllInTheTooltip()
      {
         Assert.Equal("ap@example.test + 2 more", QuarantineReview.SummariseRecipients("ap@example.test, finance@example.test,ops@example.test"));
         Assert.Equal("ada@example.test", QuarantineReview.SummariseRecipients("ada@example.test"));
         Assert.Equal("", QuarantineReview.SummariseRecipients(""));

         QuarantineRow row = Loaded().Rows.First(r => r.IsMessage);
         Assert.Equal(new[] { "ap@example.test", "finance@example.test", "ops@example.test" }, row.RecipientsTip.Split(System.Environment.NewLine));
         Assert.Equal("supply-example.mail.protection.example", row.HeloTip);
      }

      [Fact]
      public void TheNullSenderIsShownAsTheConversationWroteIt_AndAMissingHeloSaysSo()
      {
         QuarantineRow bounce = Loaded(QuarantineGrouping.None).Rows.Single(r => r.Id == 2);

         Assert.Equal("<>", bounce.Sender);
         Assert.Equal("", bounce.Helo);
         Assert.Equal("No HELO name was recorded", bounce.HeloTip);
         Assert.Contains("Undelivered", bounce.AccessibleName);
      }

      [Fact]
      public void AGroupsRowHasNoHeldBecause_AMessagesRowHasItsReason()
      {
         var review = new QuarantineReview();
         review.Load(new[] { new HeldMessage { Id = 9, Reason = "Score 12 (SURBL)", SenderDomain = "example.com" } });

         Assert.Null(review.Rows[0].HeldBecause);
         Assert.Equal("Held because: Score 12 (SURBL)", review.Rows[1].HeldBecause);
      }

      [Fact]
      public void TheBulkRequestIsTheBodyTheServerReads()
      {
         using JsonDocument release = JsonDocument.Parse(QuarantineReview.BulkRequest(new[] { 3, 4, 3 }, "release"));
         Assert.Equal(new[] { 3, 4 }, release.RootElement.GetProperty("ids").EnumerateArray().Select(i => i.GetInt32()).ToArray());
         Assert.Equal("release", release.RootElement.GetProperty("action").GetString());
         Assert.False(release.RootElement.TryGetProperty("list", out _));
         Assert.False(release.RootElement.TryGetProperty("dry_run", out _));

         using JsonDocument allow = JsonDocument.Parse(QuarantineReview.BulkRequest(new[] { 5 }, "allow", "domain", "helo", dryRun: true, confirmPublicSuffix: true));
         Assert.Equal("domain", allow.RootElement.GetProperty("list").GetString());
         Assert.Equal("helo", allow.RootElement.GetProperty("domain_from").GetString());
         Assert.True(allow.RootElement.GetProperty("dry_run").GetBoolean());
         Assert.True(allow.RootElement.GetProperty("confirm_public_suffix").GetBoolean());
      }

      [Fact]
      public void TheBulkAnswerIsRead_EntriesAndEveryResult()
      {
         // Byte for byte the shape QuarantineReview::Run writes.
         const string answer =
            "{\"action\":\"block\",\"list\":\"domain\",\"domain_from\":\"sender\",\"dry_run\":false," +
            "\"entries\":[{\"value\":\".supply.example\",\"list\":\"domain\",\"match_on\":\"envelope\",\"public_suffix\":false,\"outcome\":\"added\",\"error\":\"\",\"ids\":[6,5]}]," +
            "\"results\":[{\"id\":6,\"ok\":true,\"outcome\":\"deleted\",\"error\":\"\"},{\"id\":5,\"ok\":true,\"outcome\":\"deleted\",\"error\":\"\"}," +
            "{\"id\":2,\"ok\":false,\"outcome\":\"\",\"error\":\"The message has the null sender, so there is no sender domain to list.\"}]," +
            "\"succeeded\":2,\"failed\":1}";

         BulkAnswer read = QuarantineReview.ReadBulkAnswer(answer);

         Assert.NotNull(read);
         Assert.False(read.DryRun);
         Assert.Equal(2, read.Succeeded);
         Assert.Equal(1, read.Failed);

         BulkEntry entry = Assert.Single(read.Entries);
         Assert.Equal(".supply.example", entry.Value);
         Assert.Equal("envelope", entry.MatchOn);
         Assert.Equal("added", entry.Outcome);
         Assert.Equal(new[] { 6, 5 }, entry.Ids.ToArray());

         Assert.Equal(3, read.Results.Count);
         Assert.Contains("null sender", read.Results.Single(r => !r.Ok).Error);
      }

      [Fact]
      public void AnAnswerThatIsNotOneIsNull()
      {
         Assert.Null(QuarantineReview.ReadBulkAnswer("not json"));
         Assert.Null(QuarantineReview.ReadBulkAnswer("[1,2]"));
         Assert.Null(QuarantineReview.ReadBulkAnswer(null));
      }
   }
}
