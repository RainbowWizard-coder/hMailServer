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
         Assert.Contains("- RoundingSlack;", layOut, StringComparison.Ordinal);
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

      // ---- the server's search, while the page holds only the most recent ------------

      private static HeldMessage Held_(int id, string subject, string sender = "news@letters.example", string recipients = "ada@example.test")
         => new() { Id = id, CreatedTime = "2026-10-08 03:42:00", Sender = sender, Recipients = recipients, Score = 21, Subject = subject, Reason = "Score 21", SenderDomain = "letters.example" };

      /// <summary>
      /// The server's answer is shown as it came while it is the answer for the text in
      /// the box: its database matched it, by rules of its own - MySQL's collations
      /// fold accents, so "café" finds "Cafe" there - and a second filter here hid what
      /// it had found. A text typed since is the page's again until the server answers it.
      /// </summary>
      [Fact]
      public void TheServersAnswer_IsShownWhole_WhileItIsTheAnswerForTheText()
      {
         var review = new QuarantineReview();
         review.SetGrouping(QuarantineGrouping.None);
         review.Load(new[] { Held_(900, "Newest"), Held_(899, "Second") });

         review.SetFilter("café");
         Assert.Empty(review.Shown);
         Assert.False(review.ShowsServerSearch);

         review.LoadServerSearch("café", new[] { Held_(12, "Cafe opening"), Held_(7, "Old cafe receipt") }, 2);
         Assert.True(review.ShowsServerSearch);
         Assert.Equal(new[] { 12, 7 }, review.Rows.Select(r => r.Id).ToArray());
         Assert.Equal(2, review.ServerMatches);
         Assert.Equal(2, review.Messages.Count);

         // Another text: the page's own matches, until the server answers it.
         review.SetFilter("second");
         Assert.False(review.ShowsServerSearch);
         Assert.Equal(new[] { 899 }, review.Rows.Select(r => r.Id).ToArray());

         // The same text again: the answer for it.
         review.SetFilter("  café ");
         Assert.True(review.ShowsServerSearch);

         // No search: the page.
         review.SetFilter("");
         Assert.False(review.ShowsServerSearch);
         Assert.Equal(new[] { 900, 899 }, review.Rows.Select(r => r.Id).ToArray());
      }

      /// <summary>
      /// A check on a message only the server's answer holds survives a reload of the
      /// page, which every release and delete makes, and goes once the message has gone
      /// from the answer as well; the four actions act on it as on any checked message.
      /// </summary>
      [Fact]
      public void ChecksOnTheAnswersMessages_SurviveAReload_AndGoWithTheMessage()
      {
         var review = new QuarantineReview();
         review.SetGrouping(QuarantineGrouping.None);
         review.Load(new[] { Held_(900, "Newest invoice") });
         review.SetFilter("invoice");
         review.LoadServerSearch("invoice", new[] { Held_(900, "Newest invoice"), Held_(12, "Old invoice") }, 2);

         review.Toggle(review.Rows.Single(r => r.Id == 12));
         Assert.Equal(new[] { 12 }, review.CheckedIds.ToArray());
         Assert.Equal("Old invoice", review.MessagesWith(new[] { 12 }).Single().Subject);

         // The reload: the page again, the answer still standing until it is asked again.
         review.Load(new[] { Held_(901, "Newer still"), Held_(900, "Newest invoice") });
         Assert.Equal(new[] { 12 }, review.CheckedIds.ToArray());

         // The answer asked again after the release of 12: the check goes with it.
         review.LoadServerSearch("invoice", new[] { Held_(900, "Newest invoice") }, 1);
         Assert.Empty(review.CheckedIds);

         // A message in the page and the answer both is one message.
         Assert.Single(review.MessagesWith(new[] { 900 }));
      }

      [Fact]
      public void ClearingTheAnswer_GivesTheSearchBackToThePage()
      {
         var review = new QuarantineReview();
         review.SetGrouping(QuarantineGrouping.None);
         review.Load(new[] { Held_(900, "Invoice 9"), Held_(899, "Statement") });
         review.SetFilter("invoice");
         review.LoadServerSearch("invoice", new[] { Held_(900, "Invoice 9"), Held_(3, "Invoice 1") }, 2);
         review.Toggle(review.Rows.Single(r => r.Id == 3));

         review.ClearServerSearch();
         Assert.False(review.ShowsServerSearch);
         Assert.Null(review.ServerSearchText);
         Assert.Empty(review.Found);
         Assert.Equal(new[] { 900 }, review.Rows.Select(r => r.Id).ToArray());
         Assert.Empty(review.CheckedIds);
      }

      [Fact]
      public void HowManyMatch_IsNeverFewerThanTheServerSent()
      {
         var review = new QuarantineReview();
         review.SetFilter("x");
         review.LoadServerSearch("x", new[] { Held_(2, "x"), Held_(1, "x") }, 0);
         Assert.Equal(2, review.ServerMatches);

         review.LoadServerSearch("x", new[] { Held_(2, "x") }, 723);
         Assert.Equal(723, review.ServerMatches);
         Assert.Single(review.Found);
      }

      /// <summary>
      /// The page asks Quarantine.Search while its list holds only the most recent and a
      /// search is typed in - a moment after the typing stops, and at once after a
      /// reload - says what came of it in the notice over the list, and falls back to the
      /// page's own matches, saying so, on a failure or a server without the search. It
      /// asks for a page no larger than the server gives and a text no longer than the
      /// server takes.
      /// </summary>
      [Fact]
      public void ThePage_AsksTheServerWhileItsListIsPartial_AndSaysSo()
      {
         string view = ReadControlPanelFile(Path.Join("Views", "QuarantineView.cs"));

         string asks = WithheldRowTests.MemberText(view, "private bool SearchesTheServer_");
         Assert.True(asks != null, "QuarantineView.SearchesTheServer_ is gone.");
         Assert.Contains("review_.Messages.Count < total_", asks, StringComparison.Ordinal);
         Assert.Contains("review_.Filter.Length <= ServerSearchLongest", asks, StringComparison.Ordinal);
         Assert.Contains("!serverCannotSearch_", asks, StringComparison.Ordinal);

         string search = WithheldRowTests.MemberText(view, "private void SearchServer_(bool again)");
         Assert.True(search != null, "QuarantineView.SearchServer_ is gone.");
         Assert.Contains("int matches = (int)quarantine.Search(text, 0, ServerSearchPage);", search, StringComparison.Ordinal);
         Assert.Contains("review_.LoadServerSearch(text, found, matches);", search, StringComparison.Ordinal);
         Assert.Contains("catch (Exception ex) when (ComErrorText.IsMissingMember(ex))", search, StringComparison.Ordinal);
         Assert.DoesNotContain("catch (COMException", search, StringComparison.Ordinal);
         Assert.Contains("serverCannotSearch_ = true;", search, StringComparison.Ordinal);
         Assert.Contains("serverSearchError_ = ServerSession.DescribeComError(ex);", search, StringComparison.Ordinal);

         // A moment after the typing stops, not at every key; at once after a reload.
         Assert.Contains("if (SearchesTheServer_ && !review_.ShowsServerSearch)\n               serverSearchTimer_.Start();", view.Replace("\r\n", "\n"), StringComparison.Ordinal);
         Assert.Contains("SearchServer_(again: false);", view, StringComparison.Ordinal);
         string reload = WithheldRowTests.MemberText(view, "private void Reload()");
         Assert.Contains("SearchServer_(again: true);", reload, StringComparison.Ordinal);
         Assert.Contains("review_.ClearServerSearch();", reload, StringComparison.Ordinal);
         string leave = WithheldRowTests.MemberText(view, "public void OnLeave()");
         Assert.Contains("serverSearchTimer_.Stop();", leave, StringComparison.Ordinal);

         // What it says.
         string notice = WithheldRowTests.MemberText(view, "private void ShowListNotice_()");
         Assert.True(notice != null, "QuarantineView.ShowListNotice_ is gone.");
         foreach (string said in new[]
         {
            "Searching every held message on the server…",
            "Searched every held message on the server: {0} matches the search.",
            "{0} of them, the most recent, are shown here.",
            "Could not search every held message on the server, so the search looks at the most recent only: {0}",
            "This server is older than the Control Panel and cannot search every held message, so the search looks at the most recent only. Upgrade the server.",
            "{0} held, showing the {1} most recent."
         })
            Assert.Contains(said, notice, StringComparison.Ordinal);
         Assert.True(notice.IndexOf("if (ReadFailure.IsShown(status_))", StringComparison.Ordinal) < notice.IndexOf("Status_(", StringComparison.Ordinal),
            "The search's notice takes the place of a failed read's.");

         // The pill counts what was looked at: every held message where the server did.
         string state = WithheldRowTests.MemberText(view, "private void ShowSearchState_()");
         Assert.Contains("int lookedAt = review_.ShowsServerSearch ? total_ : review_.Messages.Count;", state, StringComparison.Ordinal);
         Assert.Contains("if (serverSearchTimer_.IsEnabled)", state, StringComparison.Ordinal);

         // The page and the text are the server's own bounds.
         Assert.Contains("private const int ServerSearchPage = 500;", view, StringComparison.Ordinal);
         Assert.Contains("private const int ServerSearchLongest = 255;", view, StringComparison.Ordinal);
         Assert.Contains("static const int max_listed_ = 500;", ReadServerFile(Path.Join("COM", "InterfaceQuarantine.h")), StringComparison.Ordinal);
         Assert.Contains("static constexpr int MaxSearchText = 255;", ReadServerFile(Path.Join("Common", "AntiSpam", "QuarantineStore.h")), StringComparison.Ordinal);
      }

      /// <summary>
      /// A server older than the Control Panel is told by what the page's own call
      /// throws. Through dynamic, as the page calls it, a method the server's
      /// IDispatch lacks is the binder's RuntimeBinderException - "does not contain a
      /// definition for" - and not a COMException: the binder asks GetIDsOfNames
      /// itself. A filter on DISP_E_UNKNOWNNAME alone never saw it, so a real older
      /// server's missing search was a failure, a Warning in the runtime's English,
      /// asked again at every pause in the typing. Scripting.Dictionary, on every
      /// Windows, stands in for a server without Quarantine.Search.
      /// </summary>
      [Fact]
      public void AServerWithoutTheSearch_IsToldByWhatTheCallThrows()
      {
         Type dictionaryType = Type.GetTypeFromProgID("Scripting.Dictionary");
         Assert.True(dictionaryType != null, "Scripting.Dictionary is not registered on this machine.");

         dynamic withoutSearch = Activator.CreateInstance(dictionaryType);
         try
         {
            Exception thrown = Record.Exception(() => { _ = (int)withoutSearch.Search("invoice", 0, 500); });

            Assert.NotNull(thrown);
            Assert.False(thrown is System.Runtime.InteropServices.COMException, "The binder's failure was a COMException after all: " + thrown);
            Assert.True(ComErrorText.IsMissingMember(thrown), thrown.GetType().FullName + ": " + thrown.Message);
         }
         finally
         {
            System.Runtime.InteropServices.Marshal.FinalReleaseComObject((object)withoutSearch);
         }
      }

      /// <summary>
      /// No page tells an older server by DISP_E_UNKNOWNNAME (0x80020006) alone:
      /// ComErrorText.IsMissingMember is the one test, and knows DISP_E_MEMBERNOTFOUND
      /// and the binder's words as well. The Quarantine page's search, its Bulk and
      /// its headers and preview each caught the COMException only.
      /// </summary>
      [Fact]
      public void NoPageTellsAnOlderServerByUnknownNameAlone()
      {
         var wrong = new List<string>();
         string root = ControlPanelDirectory();
         foreach (string file in Directory.EnumerateFiles(root, "*.cs", SearchOption.AllDirectories))
         {
            string name = Path.GetFileName(file);
            string top = Path.GetRelativePath(root, file).Split(Path.DirectorySeparatorChar)[0];
            if (top is "obj" or "bin")
               continue;

            // The two tests themselves.
            if (name is "ComErrorText.cs" or "SettingApplies.cs")
               continue;

            string[] lines = File.ReadAllLines(file);
            for (int i = 0; i < lines.Length; i++)
            {
               if (lines[i].Contains("0x80020006", StringComparison.OrdinalIgnoreCase))
                  wrong.Add(name + ":" + (i + 1) + ": " + lines[i].Trim());
            }
         }

         Assert.True(wrong.Count == 0, "Use ComErrorText.IsMissingMember, which knows the binder's RuntimeBinderException too:\n" + string.Join("\n", wrong));
      }

      private static string ControlPanelDirectory()
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel");
            if (File.Exists(Path.Join(candidate, "ControlPanel.csproj")))
               return candidate;
            directory = directory.Parent;
         }

         Assert.Fail("ControlPanel/ could not be found from the test binaries.");
         return null;
      }

      private static string ReadServerFile(string relative)
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "Server", relative);
            if (File.Exists(candidate))
               return File.ReadAllText(candidate);
            directory = directory.Parent;
         }

         Assert.Fail("Server/" + relative.Replace('\\', '/') + " could not be found from the test binaries.");
         return null;
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
