// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Text.RegularExpressions;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The Journaling and dual delivery page (review finding F255): its columns
   /// share the card's width rather than adding up to 1,100 pixels, a rule that is
   /// off says Off, the switch says which way it goes, adding, switching and
   /// removing a rule say what they did, and the add form is titled and in the
   /// grid's order. JournalRuleText holds the words; the page's lines are read
   /// here, as the other page tests read theirs, since this assembly runs no WPF.
   /// </summary>
   public class JournalingPageTests
   {
      [Fact]
      public void ARulesState_IsOnOrOff_NeverAnEmptyCell()
      {
         Assert.Equal("On", JournalRuleText.State(true));
         Assert.Equal("Off", JournalRuleText.State(false));
      }

      [Fact]
      public void EachAct_NamesTheRuleItActedOn()
      {
         Assert.Equal("Rule for example.com added.", JournalRuleText.Added("example.com"));
         Assert.Equal("Rule for user00@example.com turned off.", JournalRuleText.Switched("user00@example.com", false));
         Assert.Equal("Rule for user00@example.com turned on.", JournalRuleText.Switched("user00@example.com", true));
         Assert.Equal("Rule for example.com removed.", JournalRuleText.Removed("example.com", true));
         Assert.Equal("The rule for example.com was not there any more.", JournalRuleText.Removed("example.com", false));
      }

      [Fact]
      public void WhoseMailsExample_FollowsTheDomainOrAccountChoice()
      {
         Assert.Equal("example.com", JournalRuleText.TargetExample(false));
         Assert.Equal("user@example.com", JournalRuleText.TargetExample(true));

         string page = Page();
         Assert.Contains("scope_.SelectionChanged += (s, e) => target_.PlaceholderText = JournalRuleText.TargetExample(scope_.SelectedIndex == 1);", page, StringComparison.Ordinal);
         Assert.DoesNotContain("A domain, or an account address", page, StringComparison.Ordinal);
         Assert.DoesNotContain("Where the copy goes", page, StringComparison.Ordinal);
      }

      /// <summary>
      /// The two addresses and the note share the width, the journal address twice
      /// as much as each other, trimmed, floored at their headers; the four short
      /// words are as wide as they need, never narrower than the widest word each
      /// can show; no column is a fixed number of pixels, which is what added up to
      /// 1,100 on a card of 900.
      /// </summary>
      [Fact]
      public void TheColumns_ShareTheCardsWidth()
      {
         string page = Page();
         Assert.Contains("rules_.Columns.Add(SharedColumn_(L(\"Whose mail\"), nameof(RuleRow.Target), 160, 1));", page, StringComparison.Ordinal);
         // Each of the four short words' columns is given every word it can show.
         Assert.Contains("rules_.Columns.Add(FittedColumn_(L(\"Domain or account\"), nameof(RuleRow.Scope), L(\"Domain\"), L(\"Account\")));", page, StringComparison.Ordinal);
         Assert.Contains("rules_.Columns.Add(FittedColumn_(L(\"Which way\"), nameof(RuleRow.Direction), L(\"Both ways\"), L(\"Arriving\"), L(\"Sent\")));", page, StringComparison.Ordinal);
         Assert.Contains("rules_.Columns.Add(FittedColumn_(L(\"What is sent\"), nameof(RuleRow.Mode), L(\"Envelope report\"), L(\"The message itself\")));", page, StringComparison.Ordinal);
         Assert.Contains("rules_.Columns.Add(SharedColumn_(L(\"Where it goes\"), nameof(RuleRow.JournalAddress), 160, 2));", page, StringComparison.Ordinal);
         Assert.Contains("rules_.Columns.Add(FittedColumn_(L(\"On\"), nameof(RuleRow.Enabled), JournalRuleText.State(true), JournalRuleText.State(false)));", page, StringComparison.Ordinal);
         Assert.Contains("rules_.Columns.Add(SharedColumn_(L(\"Note\"), nameof(RuleRow.Note), 80, 1));", page, StringComparison.Ordinal);

         string shared = WithheldRowTests.MemberText(page, "private static DataGridTextColumn SharedColumn_(string header, string property, double floor, double weight)");
         Assert.True(shared != null, "JournalingView.SharedColumn_ is gone.");
         Assert.Contains("GridStyles.Trimmed(", shared, StringComparison.Ordinal);
         Assert.Contains("new DataGridLength(weight, DataGridLengthUnitType.Star)", shared, StringComparison.Ordinal);
         Assert.Contains("Math.Max(floor, GridStyles.HeaderWidth(header))", shared, StringComparison.Ordinal);

         // Floored at the widest word as well as the header, and trimmed: at the
         // header alone the smallest window cut "Both ways" to "Both way" and
         // "Beide Richtungen" to "Beide Richtunge", mid-glyph.
         string fitted = WithheldRowTests.MemberText(page, "private static DataGridTextColumn FittedColumn_(string header, string property, params string[] values)");
         Assert.True(fitted != null, "JournalingView.FittedColumn_ is gone.");
         Assert.Contains("Width = DataGridLength.Auto", fitted, StringComparison.Ordinal);
         Assert.Contains("double floor = GridStyles.HeaderWidth(header);", fitted, StringComparison.Ordinal);
         Assert.Contains("foreach (string value in values)", fitted, StringComparison.Ordinal);
         Assert.Contains("floor = Math.Max(floor, GridStyles.CellWidth(value));", fitted, StringComparison.Ordinal);
         Assert.Contains("MinWidth = floor", fitted, StringComparison.Ordinal);
         Assert.Contains("GridStyles.Trimmed(", fitted, StringComparison.Ordinal);

         // Every word the grid's three captions give, and On and Off, is among
         // the words its column is floored at: a word added to a caption and not
         // to its column would be cut again.
         foreach ((string caption, string column) in new[] { ("ScopeCaption_", "Scope"), ("DirectionCaption_", "Direction"), ("ModeCaption_", "Mode") })
         {
            string words = WithheldRowTests.MemberText(page, "private static string " + caption + "(");
            Assert.True(words != null, "JournalingView." + caption + " is gone.");
            string call = Regex.Match(page, @"rules_\.Columns\.Add\(FittedColumn_\([^;]*nameof\(RuleRow\." + column + @"\)[^;]*;").Value;
            foreach (Match word in Regex.Matches(words, @"L\(""[^""]+""\)"))
               Assert.Contains(word.Value, call, StringComparison.Ordinal);
         }

         Assert.DoesNotMatch(new Regex(@"Width = new DataGridLength\(\d+\)"), page);
         Assert.DoesNotContain("MaxWidth = 1100", page, StringComparison.Ordinal);
      }

      [Fact]
      public void TheOnColumn_SaysOff_ForARuleThatIsOff()
      {
         string page = Page();
         Assert.Contains("Enabled = JournalRuleText.State(enabled),", page, StringComparison.Ordinal);
         Assert.DoesNotContain("L(\"Yes\")", page, StringComparison.Ordinal);
      }

      /// <summary>The switch says which way it will go, with a key for each, where "Switch the rule over" said neither.</summary>
      [Fact]
      public void TheSwitch_SaysWhichWayItGoes()
      {
         string page = Page();
         string show = WithheldRowTests.MemberText(page, "private void ShowToggle_(bool enabled)");
         Assert.True(show != null, "JournalingView.ShowToggle_ is gone.");
         Assert.Contains("toggle_.Content = L(\"_Turn off\");", show, StringComparison.Ordinal);
         Assert.Contains("toggle_.Content = L(\"Turn _on\");", show, StringComparison.Ordinal);
         Assert.Contains("rules_.SelectionChanged += (s, e) => { if (rules_.SelectedItem is RuleRow row) ShowToggle_(row.IsEnabled); };", page, StringComparison.Ordinal);
         Assert.DoesNotContain("S_witch the rule over", page, StringComparison.Ordinal);
      }

      /// <summary>
      /// Each act reads the list first and then says what it did, at its level,
      /// with the rule it acted on selected, so that the outcome is seen over the
      /// list it changed; a refusal of the switch or the removal is Critical there.
      /// </summary>
      [Theory]
      [InlineData("private void Add_()", "Select_(added);", "Say_(StatusLevel.Good, JournalRuleText.Added(target));")]
      [InlineData("private void Toggle_()", "Select_(row.Id);", "Say_(StatusLevel.Good, JournalRuleText.Switched(row.Target, !row.IsEnabled));")]
      [InlineData("private void Remove_()", "ListOutcome.IndexAfterDelete(rows.Count, at)", "Say_(removed ? StatusLevel.Good : StatusLevel.Information, JournalRuleText.Removed(row.Target, removed));")]
      public void EachAct_SaysWhatItDid_AfterTheListIsReadAgain(string member, string select, string say)
      {
         string act = WithheldRowTests.MemberText(Page(), member);
         Assert.True(act != null, "JournalingView." + member + " is gone.");
         int reload = act.LastIndexOf("Reload_();", StringComparison.Ordinal);
         int selected = act.IndexOf(select, StringComparison.Ordinal);
         int said = act.IndexOf(say, StringComparison.Ordinal);
         Assert.True(reload >= 0 && selected > reload && said > selected,
            "JournalingView." + member + " does not read the list, select the rule and then say what it did.");
      }

      [Fact]
      public void TheOutcome_IsAboveTheList_AndARefreshClearsIt()
      {
         string page = Page();
         int outcome = page.IndexOf("cards.Children.Add(outcome_);", StringComparison.Ordinal);
         int list = page.IndexOf("cards.Children.Add(listCard_);", StringComparison.Ordinal);
         Assert.True(outcome >= 0 && list > outcome, "The outcome is not above the list's card.");

         string reload = WithheldRowTests.MemberText(page, "private void Reload_()");
         Assert.True(reload != null, "JournalingView.Reload_ is gone.");
         Assert.Contains("outcome_.Visibility = Visibility.Collapsed;", reload, StringComparison.Ordinal);
      }

      /// <summary>The add form is titled and lays its boxes out in the grid's order, three to a line, Add the rule under them.</summary>
      [Fact]
      public void TheAddForm_IsTitledAndInTheGridsOrder()
      {
         string page = Page();
         Assert.Contains("new Card { Title = L(\"Add a journaling rule\"), Content = editing,", page, StringComparison.Ordinal);

         string form = WithheldRowTests.MemberText(page, "private Grid BuildForm_()");
         Assert.True(form != null, "JournalingView.BuildForm_ is gone.");
         string[] order = { "L(\"W_hose mail\"), target_", "L(\"_Domain or account\"), scope_", "L(\"Wh_ich way\"), direction_", "L(\"What is _sent\"), mode_", "L(\"Where it _goes\"), address_", "L(\"_Note\"), note_" };
         int last = -1;
         foreach (string field in order)
         {
            int at = form.IndexOf(field, StringComparison.Ordinal);
            Assert.True(at > last, "The add form does not have " + field + " in the grid's order.");
            last = at;
         }

         Assert.Contains("Grid.SetRow(row, i / 3);", form, StringComparison.Ordinal);
         Assert.Contains("Grid.SetColumn(row, i % 3);", form, StringComparison.Ordinal);
         Assert.Contains("Grid.SetRow(add_, 2);", form, StringComparison.Ordinal);
         Assert.DoesNotContain("new WrapPanel()", page, StringComparison.Ordinal);
      }

      private static string Page() => ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "JournalingView.cs"));
   }
}
