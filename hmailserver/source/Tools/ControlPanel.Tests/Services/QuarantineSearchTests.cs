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
         Assert.Equal("15 h ago", QuarantineReview.HeldText("2026-10-08 03:42:00", new DateTime(2026, 10, 8, 18, 50, 0)));
         Assert.EndsWith(" (server time)", QuarantineReview.HeldTip("2026-10-08 03:42:00"), StringComparison.Ordinal);
         Assert.Equal("not a time", QuarantineReview.HeldText("not a time", DateTime.Now));

         // The age in words and the whole time in the tool tip, where the cell is the age.
         string tip = QuarantineReview.HeldTip("2026-10-08 03:42:00", new DateTime(2026, 10, 8, 18, 50, 0));
         Assert.StartsWith("15 hours ago" + Environment.NewLine, tip, StringComparison.Ordinal);
         Assert.EndsWith(" (server time)", tip, StringComparison.Ordinal);

         var review = new QuarantineReview { AgesFrom = new DateTime(2026, 10, 8, 4, 0, 0) };
         review.Load(Held);
         review.SetGrouping(QuarantineGrouping.None);
         Assert.Equal("18 min ago", review.Rows[0].Held);
         Assert.StartsWith("18 minutes ago", review.Rows[0].HeldTip, StringComparison.Ordinal);
      }

      /// <summary>
      /// Where the cells are the server's own date and time - the server on another
      /// computer - the column says so in its caption, which every reader reaches (the
      /// owner's decision 13), where the marker was in each cell's tool tip alone; an
      /// age needs no clock named.
      /// </summary>
      [Fact]
      public void TheHeldColumn_SaysServerTimeWhereItsCellsAreTheServersTime()
      {
         var remote = new QuarantineReview { AgesFrom = null };
         Assert.Equal("Held (server time)", remote.HeldCaption);
         var local = new QuarantineReview { AgesFrom = new DateTime(2026, 10, 8, 4, 0, 0) };
         Assert.Equal("Held", local.HeldCaption);

         string view = ReadControlPanelFile(Path.Join("Views", "QuarantineView.cs"));
         int ages = view.IndexOf("review_.AgesFrom = ServerSession.IsLocalSession ? DateTime.Now : null;", StringComparison.Ordinal);
         int caption = view.IndexOf("heldColumn_.Header = review_.HeldCaption;", StringComparison.Ordinal);
         Assert.True(ages >= 0 && caption > ages, "The Held column is not captioned for the clock its cells are read by.");
         Assert.Contains("GridStyles.HeaderWidth(review_.HeldCaption)", view, StringComparison.Ordinal);
      }

      [Theory]
      [InlineData(0.5, "just now")]
      [InlineData(60, "1 min ago")]
      [InlineData(600, "10 min ago")]
      [InlineData(3600 * 26, "26 h ago")]
      [InlineData(3600 * 24 * 13, "13 d ago")]
      [InlineData(-5, "just now")]
      public void AnAgeInACell_IsInShortUnits(double seconds, string expected)
         => Assert.Equal(expected, AgeWords.Short(TimeSpan.FromSeconds(seconds)));

      // ---- the text columns' widths -----------------------------------------------------

      // What the seed's widest sender, recipients and subject (its two icons with it)
      // need at 100 %, near enough: sender0@mass-mail.example, user00@example.com,
      // Account verification required.
      private const double SenderNeed = 200, RecipientsNeed = 152, SubjectNeed = 244;

      [Fact]
      public void At1280_TheSenderAndTheSubjectAreWhole_AndTheHeloNameIsLeftOut()
      {
         // A 1280 window with the sidebar open gives the grid 921 pixels, of which the
         // box, the actions, Held and Score take some 320 in English and 390 in German.
         QuarantineColumnLayout english = QuarantineReview.Columns(921, 600, SenderNeed, RecipientsNeed, SubjectNeed);
         Assert.False(english.ShowsHelo);
         Assert.True(english.ShowsRecipients);
         Assert.Equal(SenderNeed, english.SenderMin);
         Assert.Equal(SubjectNeed, english.SubjectMin);
         Assert.Equal(RecipientsNeed, english.RecipientsMin);

         QuarantineColumnLayout german = QuarantineReview.Columns(921, 530, SenderNeed, RecipientsNeed, SubjectNeed);
         Assert.True(german.ShowsRecipients);
         Assert.Equal(SenderNeed, german.SenderMin);
         Assert.Equal(SubjectNeed, german.SubjectMin);
         Assert.Equal(86, german.RecipientsMin);   // what is left, over its floor
         Assert.True(german.SenderMin + german.RecipientsMin + german.SubjectMin <= 530);
      }

      [Fact]
      public void TheHeloName_IsShownByTheGridsOwnWidth_NotTheWindows()
      {
         Assert.False(QuarantineReview.Columns(999, 680, SenderNeed, RecipientsNeed, SubjectNeed).ShowsHelo);
         QuarantineColumnLayout wide = QuarantineReview.Columns(1000, 680, SenderNeed, RecipientsNeed, SubjectNeed);
         Assert.True(wide.ShowsHelo);
         Assert.True(wide.SenderMin + wide.RecipientsMin + wide.SubjectMin + QuarantineReview.HeloFloor <= 680);
      }

      [Fact]
      public void WhereTheyCannotBeWhole_TheSenderAndTheSubjectShare_AndNothingIsWiderThanTheRoom()
      {
         // The narrowest window, 760 by 520, gives the text columns some 316 pixels:
         // the recipients' column goes, and the sender and the subject share the rest
         // in proportion to their need.
         QuarantineColumnLayout narrow = QuarantineReview.Columns(633, 316, SenderNeed, RecipientsNeed, SubjectNeed);
         Assert.False(narrow.ShowsHelo);
         Assert.False(narrow.ShowsRecipients);
         Assert.True(narrow.SenderMin + narrow.SubjectMin <= 316);
         Assert.True(narrow.SenderMin >= 140 && narrow.SubjectMin >= 170);

         foreach (double room in new[] { 0.0, 40, 150, 300, 419, 420, 500, 800 })
         {
            QuarantineColumnLayout layout = QuarantineReview.Columns(700, room, SenderNeed, RecipientsNeed, SubjectNeed);
            Assert.True(layout.SenderMin + layout.RecipientsMin + layout.SubjectMin <= Math.Max(room, 0) + 0.001, "wider than the room at " + room);
            Assert.Equal(room >= QuarantineReview.SenderFloor + QuarantineReview.RecipientsFloor + QuarantineReview.SubjectFloor, layout.ShowsRecipients);
         }
      }

      [Fact]
      public void ShortTextIsFlooredAtTheFloors_AndLongTextAtTheCaps()
      {
         QuarantineColumnLayout shortText = QuarantineReview.Columns(921, 900, 40, 40, 60);
         Assert.Equal(QuarantineReview.SenderFloor, shortText.SenderMin);
         Assert.Equal(QuarantineReview.SubjectFloor, shortText.SubjectMin);
         Assert.Equal(QuarantineReview.RecipientsFloor, shortText.RecipientsMin);

         QuarantineColumnLayout longText = QuarantineReview.Columns(921, 2000, 2000, 2000, 2000);
         Assert.True(longText.SenderMin < 400 && longText.RecipientsMin < 400 && longText.SubjectMin < 500);
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

         // Held's ages only where the clocks are one.
         Assert.Contains("review_.AgesFrom = ServerSession.IsLocalSession ? DateTime.Now : null;", view, StringComparison.Ordinal);

         // The text columns by the grid's own width, not the window's - which at 1280
         // with the sidebar open kept the HELO column beside cut addresses - laid out
         // as QuarantineReview.Columns says, again as the grid's size or its scroll
         // bar changes and as the rows are bound.
         Assert.DoesNotContain("Window.GetWindow(this)?.ActualWidth", view, StringComparison.Ordinal);
         Assert.DoesNotContain("CollapseSidebarBelow", view, StringComparison.Ordinal);
         Assert.Contains("list_.SizeChanged += (_, _) => LayOutTextColumns_();", view, StringComparison.Ordinal);
         string layOut = WithheldRowTests.MemberText(view, "private void LayOutTextColumns_()");
         Assert.True(layOut != null, "QuarantineView.LayOutTextColumns_ is gone.");
         Assert.Contains("QuarantineReview.Columns(list_.ActualWidth, room, senderNeed_, recipientsNeed_, subjectNeed_)", layOut, StringComparison.Ordinal);
         Assert.Contains("heloColumn_.Visibility = layout.ShowsHelo", layOut, StringComparison.Ordinal);
         Assert.Contains("subjectColumn_.MinWidth", layOut, StringComparison.Ordinal);
         string rebind = WithheldRowTests.MemberText(view, "private void Rebind_()");
         Assert.Contains("MeasureTextColumns_();", rebind, StringComparison.Ordinal);

         // The headers' icon is the subject's, beside the preview, so that it does not go
         // with the HELO column; the HELO cell is its name alone.
         string subject = WithheldRowTests.MemberText(view, "private static FrameworkElementFactory SubjectCell_()");
         Assert.True(subject != null, "QuarantineView.SubjectCell_ is gone.");
         Assert.Contains("(\"headers\", SymbolRegular.DocumentHeader24", subject, StringComparison.Ordinal);
         Assert.Contains("(\"preview\", SymbolRegular.Glance24", subject, StringComparison.Ordinal);
         Assert.Contains("Trimmed_(nameof(QuarantineRow.Helo), nameof(QuarantineRow.HeloTip))", view, StringComparison.Ordinal);
         Assert.DoesNotContain("WithIcon_(", view, StringComparison.Ordinal);
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
