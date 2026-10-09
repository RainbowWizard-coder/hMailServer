// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Linq;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// Review finding F261: why a message was held was the row's tool tip alone, so
   /// keyboard and screen-reader users never got it and the row's accessible name
   /// left it out; nothing searched the held messages; Sender, Recipients and HELO
   /// shared the width alike and the addresses were cut while Subject had room; and
   /// Held was the database's raw string in every language. QuarantineReview filters
   /// on sender, recipients, subject and reason, says the reason in the accessible
   /// name, and writes Held in the reader's format with its age; the page's wiring
   /// is read from its source.
   /// </summary>
   public class QuarantineSearchTests
   {
      private static readonly HeldMessage[] Held =
      {
         new() { Id = 3, CreatedTime = "2026-10-08 03:42:00", Sender = "sender0@mass-mail.example", Recipients = "user00@example.com",
                 Score = 21, Subject = "Your invoice is attached", Reason = "Score 21: DNSBL zen.spamhaus.org, no PTR", SenderDomain = "mass-mail.example" },
         new() { Id = 2, CreatedTime = "2026-10-07 22:10:00", Sender = "billing@supply.example", Recipients = "ada@example.test, ops@example.test",
                 Score = 25, Subject = "Monthly statement", Reason = "Score 25: SPF fail, DKIM fail", SenderDomain = "supply.example" },
         new() { Id = 1, CreatedTime = "2026-10-07 09:00:00", Sender = "", Recipients = "postmaster@example.test",
                 Score = 100, Subject = "Undelivered Mail Returned to Sender", Reason = "", SenderDomain = "" }
      };

      private static QuarantineReview Loaded()
      {
         var review = new QuarantineReview();
         review.Load(Held);
         review.SetGrouping(QuarantineGrouping.None);
         return review;
      }

      [Theory]
      [InlineData("mass-mail", 3)]
      [InlineData("OPS@example", 2)]
      [InlineData("statement", 2)]
      [InlineData("spf fail", 2)]
      [InlineData("Undelivered", 1)]
      public void TheSearch_LooksAtSenderRecipientsSubjectAndReason(string filter, int id)
      {
         QuarantineReview review = Loaded();
         review.SetFilter(filter);
         Assert.Equal(new[] { id }, review.Rows.Select(r => r.Id).ToArray());
      }

      [Fact]
      public void ASearchThatMatchesNothing_ShowsNothing_AndClearingItShowsEverything()
      {
         QuarantineReview review = Loaded();
         review.SetFilter("nowhere.example");
         Assert.Empty(review.Rows);
         Assert.Empty(review.Shown);
         Assert.Equal(3, review.Messages.Count);

         review.SetFilter("  ");
         Assert.Equal(3, review.Rows.Count);
      }

      [Fact]
      public void CheckingEverything_ChecksWhatTheSearchShows()
      {
         QuarantineReview review = Loaded();
         review.SetFilter("example.test");
         review.ToggleAll();
         Assert.Equal(new[] { 2, 1 }, review.CheckedIds.ToArray());
         Assert.True(review.AllChecked);
      }

      [Fact]
      public void TheRowsAccessibleName_SaysWhyItWasHeld()
      {
         QuarantineReview review = Loaded();
         Assert.Equal("Your invoice is attached, from sender0@mass-mail.example, score 21, held because: Score 21: DNSBL zen.spamhaus.org, no PTR", review.Rows[0].AccessibleName);

         // A message the server gave no reason for keeps the shorter name.
         Assert.DoesNotContain("held because", review.Rows[2].AccessibleName, StringComparison.Ordinal);
      }

      [Fact]
      public void Held_IsItsAgeWhereTheClocksAreOne_AndTheReadersTimeWhereNot()
      {
         string when = new DateTime(2026, 10, 8, 3, 42, 0).ToString("g", System.Globalization.CultureInfo.CurrentCulture);

         Assert.Equal(when, QuarantineReview.HeldText("2026-10-08 03:42:00", null));
         Assert.Equal("15 hours ago", QuarantineReview.HeldText("2026-10-08 03:42:00", new DateTime(2026, 10, 8, 18, 50, 0)));
         Assert.EndsWith(" (server time)", QuarantineReview.HeldTip("2026-10-08 03:42:00"), StringComparison.Ordinal);
         Assert.Equal("not a time", QuarantineReview.HeldText("not a time", DateTime.Now));

         var review = new QuarantineReview { AgesFrom = new DateTime(2026, 10, 8, 4, 0, 0) };
         review.Load(Held);
         review.SetGrouping(QuarantineGrouping.None);
         Assert.Equal("18 minutes ago", review.Rows[0].Held);
      }

      [Theory]
      [InlineData(0.5, "less than a minute ago")]
      [InlineData(60, "1 minute ago")]
      [InlineData(600, "10 minutes ago")]
      [InlineData(3600 * 26, "26 hours ago")]
      [InlineData(3600 * 24 * 13, "13 days ago")]
      public void AnAge_IsInTheBackupPagesWords(double seconds, string expected)
         => Assert.Equal(expected, AgeWords.Ago(TimeSpan.FromSeconds(seconds)));

      [Fact]
      public void ThePage_SaysWhyUnderTheList_SearchesAndWidensTheAddresses()
      {
         string view = ReadControlPanelFile(Path.Join("Views", "QuarantineView.cs"));

         // Why, for the selected message, under the list - and said once to a screen
         // reader, in the row's accessible name: the strip is no live region, which
         // announced the reason a second time on every move of the selection.
         Assert.Contains("parts.Children.Add(whyCard_);", view, StringComparison.Ordinal);
         string why = WithheldRowTests.MemberText(view, "private void ShowWhy_()");
         Assert.True(why != null, "QuarantineView.ShowWhy_ is gone.");
         Assert.Contains("why_.Text = row.HeldBecause;", why, StringComparison.Ordinal);
         Assert.DoesNotContain("Announce.Request(why", view, StringComparison.Ordinal);
         Assert.DoesNotContain("SetLiveSetting(why", view, StringComparison.Ordinal);

         // The search, and what it says when it finds nothing among a partial list.
         Assert.Contains("review_.SetFilter(search_.SearchText);", view, StringComparison.Ordinal);
         Assert.Contains("None of the {0} most recent held messages has", view, StringComparison.Ordinal);
         string searchState = WithheldRowTests.MemberText(view, "private void ShowSearchState_()");
         Assert.True(searchState != null, "QuarantineView.ShowSearchState_ is gone.");
         Assert.True(searchState.IndexOf("if (ReadFailure.IsShown(status_))", StringComparison.Ordinal) < searchState.IndexOf("StatusText.Show(", StringComparison.Ordinal),
            "A search after a failed read brings back the empty state's sentence, which says nothing is held.");

         // Held's ages only where the clocks are one; Sender wider, HELO left out below the sidebar's threshold.
         Assert.Contains("review_.AgesFrom = ServerSession.IsLocalSession ? DateTime.Now : null;", view, StringComparison.Ordinal);
         Assert.Contains("new DataGridLength(2, DataGridLengthUnitType.Star), nameof(QuarantineColumn.Sender)", view, StringComparison.Ordinal);
         Assert.Contains("sender.MinWidth = 140;", view, StringComparison.Ordinal);
         Assert.Contains("new DataGridLength(1.5, DataGridLengthUnitType.Star), nameof(QuarantineColumn.Recipients)", view, StringComparison.Ordinal);
         Assert.Contains("DesignTokens.Shell.CollapseSidebarBelow ? Visibility.Collapsed : Visibility.Visible", view, StringComparison.Ordinal);
      }

      private static string ReadControlPanelFile(string relative)
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", relative);
            if (File.Exists(candidate))
               return File.ReadAllText(candidate);
            directory = directory.Parent;
         }

         Assert.Fail("ControlPanel/" + relative.Replace('\\', '/') + " could not be found from the test binaries.");
         return null;
      }
   }
}
