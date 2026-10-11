// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Linq;
using System.Xml.Linq;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The Rules page's task flow (review finding F225): a new rule was not selected -
   /// the reload cleared the selection, and the rule was the last row, often out of
   /// view - Add criterion… and Add action… were live with no rule and answered
   /// "Select a rule first." in a message box, the IF and THEN cards were blank with no
   /// rule and with a rule that had nothing in them, a new rule started switched on
   /// before it had criteria or actions, "Enable / disable" never said which it would
   /// do, and a switched-off rule differed from the others only by "No" in its last
   /// column.
   /// </summary>
   [Collection("Loc")]
   public class RuleCardsTests : IDisposable
   {
      public void Dispose() => Loc.Reset();

      /// <summary>A rule with no criteria matches no message (RuleApplier), which the card says, where the review took it to match every one.</summary>
      [Fact]
      public void TheCards_SayWhatTheirEmptinessMeans()
      {
         Assert.Equal("Select a rule above to see which messages it matches.", RuleCards.CriteriaEmpty(false));
         Assert.Equal("No criteria yet, so this rule matches no message and does nothing.", RuleCards.CriteriaEmpty(true));
         Assert.Equal("Select a rule above to see what it does.", RuleCards.ActionsEmpty(false));
         Assert.Equal("No actions yet, so this rule does nothing to the messages it matches.", RuleCards.ActionsEmpty(true));
         Assert.Equal("Spam to junk added, switched off: Enable now switches it on once it has a criterion and an action.", RuleCards.Added("Spam to junk"));
         Assert.Equal("Spam to junk has a criterion and an action: Enable now switches it on.", RuleCards.Ready("Spam to junk"));
         Assert.Equal("Spam to junk switched on.", RuleCards.SwitchedOn("Spam to junk"));
      }

      /// <summary>
      /// The notice's Enable now waits for a criterion and an action: a rule switched on
      /// before them acts on live mail while the rest is still being added.
      /// </summary>
      [Theory]
      [InlineData(0, 0, false)]
      [InlineData(1, 0, false)]
      [InlineData(0, 2, false)]
      [InlineData(1, 1, true)]
      [InlineData(3, 2, true)]
      public void EnableNow_WaitsForACriterionAndAnAction(int criteria, int actions, bool can)
         => Assert.Equal(can, RuleCards.CanSwitchOn(criteria, actions));

      /// <summary>
      /// The new rule's notice carries Enable now (the unit's skeptic review of F225):
      /// after Add rule the page is at the add form and the toolbar's Enable is scrolled
      /// out of view, while the notice named it. The button is in the notice as
      /// PortsChangedNotice carries Reinitialise now, pressed only once the rule has a
      /// criterion and an action - re-checked as they are added - and it switches the
      /// rule on through the toolbar's road, then says so without the button and puts
      /// the keyboard on the rule's row. The notice goes when the page is opened
      /// again, when Show rules for changes and when its rule is gone.
      /// </summary>
      [Fact]
      public void TheNewRulesNotice_CarriesEnableNow()
      {
         string code = Read(Path.Join("Views", "RulesView.xaml.cs"));
         string add = WithheldRowTests.MemberText(code, "private void AddRule_Click(object sender, RoutedEventArgs e)");
         Assert.Contains("addedRuleName_ = name;", add, StringComparison.Ordinal);
         Assert.Contains("ShowAddedOutcome_();", add, StringComparison.Ordinal);

         string show = WithheldRowTests.MemberText(code, "private void ShowAddedOutcome_()");
         Assert.True(show != null, "RulesView.ShowAddedOutcome_ is gone.");
         Assert.Contains("RuleCards.CanSwitchOn(CriteriaGrid.Items.Count, ActionsGrid.Items.Count)", show, StringComparison.Ordinal);
         Assert.Contains("enable.IsEnabled = ready;", show, StringComparison.Ordinal);
         Assert.Contains("ready ? RuleCards.Ready(addedRuleName_) : RuleCards.Added(addedRuleName_)", show, StringComparison.Ordinal);
         Assert.Contains("OutcomeNotice.Content = enable;", show, StringComparison.Ordinal);
         // As the rule comes to have both, the keyboard goes to Enable now with the notice in view.
         Assert.Contains("bool becameReady = ready && !addedReady_;", show, StringComparison.Ordinal);
         Assert.True(show.IndexOf("FocusWithNotice_(enable);", StringComparison.Ordinal) > show.IndexOf("if (becameReady)", StringComparison.Ordinal),
            "The keyboard does not go to Enable now once the rule has a criterion and an action.");

         // The notice is under the add form and above the criteria and actions.
         string xaml = Read(Path.Join("Views", "RulesView.xaml"));
         int form = xaml.IndexOf("x:Name=\"AddRuleForm\"", StringComparison.Ordinal);
         int notice = xaml.IndexOf("x:Name=\"OutcomeNotice\"", StringComparison.Ordinal);
         int parts = xaml.IndexOf("x:Name=\"RuleParts\"", StringComparison.Ordinal);
         Assert.True(form > 0 && notice > form && parts > notice, "The new rule's notice is not between the add form and the criteria and actions.");

         string details = WithheldRowTests.MemberText(code, "private void RefreshDetails()");
         Assert.Contains("ShowAddedOutcome_();", details, StringComparison.Ordinal);

         string button = WithheldRowTests.MemberText(code, "private Wpf.Ui.Controls.Button EnableAddedButton_()");
         Assert.True(button != null, "RulesView.EnableAddedButton_ is gone.");
         Assert.Contains("Content = L(\"Enable _now\")", button, StringComparison.Ordinal);
         Assert.Contains("button.Click += (_, _) => EnableAdded_();", button, StringComparison.Ordinal);

         string enable = WithheldRowTests.MemberText(code, "private void EnableAdded_()");
         Assert.True(enable != null, "RulesView.EnableAdded_ is gone.");
         int on = enable.IndexOf("rule.Active = true;", StringComparison.Ordinal);
         int saves = enable.IndexOf("rule.Save();", StringComparison.Ordinal);
         Assert.True(on > 0 && saves > on, "Enable now does not switch the rule on and save it.");
         Assert.Contains("WithSelectedRule(rule =>", enable, StringComparison.Ordinal);
         int focus = enable.IndexOf("GridKeys.FocusSelectedRow(RuleGrid);", StringComparison.Ordinal);
         int drops = enable.IndexOf("OutcomeNotice.Content = null;", StringComparison.Ordinal);
         Assert.True(focus > 0 && drops > focus, "The keyboard is not moved off Enable now before the button leaves the notice.");
         Assert.Contains("RuleCards.SwitchedOn(name)", enable, StringComparison.Ordinal);

         string hide = WithheldRowTests.MemberText(code, "private void HideOutcome_()");
         Assert.Contains("OutcomeNotice.Content = null;", hide, StringComparison.Ordinal);
         Assert.Contains("HideOutcome_();", WithheldRowTests.MemberText(code, "public void OnEnter()"), StringComparison.Ordinal);
         Assert.Contains("HideOutcome_();", WithheldRowTests.MemberText(code, "private void Scope_Changed(object sender, SelectionChangedEventArgs e)"), StringComparison.Ordinal);
         Assert.Contains("!rows.Exists(r => r.Id == addedRuleId_)", WithheldRowTests.MemberText(code, "private void Reload()"), StringComparison.Ordinal);
      }

      [Fact]
      public void ANewRule_IsSelected_SwitchedOff_AndTheKeyboardGoesToItsNextStep()
      {
         string code = Read(Path.Join("Views", "RulesView.xaml.cs"));
         string add = WithheldRowTests.MemberText(code, "private void AddRule_Click(object sender, RoutedEventArgs e)");
         Assert.True(add != null, "RulesView.AddRule_Click is gone.");
         Assert.Contains("rule.Active = false;", add, StringComparison.Ordinal);
         Assert.DoesNotContain("rule.Active = true;", add, StringComparison.Ordinal);
         Assert.Contains("addedId = (int)rule.ID;", add, StringComparison.Ordinal);
         Assert.Contains("RuleGrid.SelectedItem = added;", add, StringComparison.Ordinal);
         Assert.Contains("ShowRowInList_(added);", add, StringComparison.Ordinal);
         // Not left out of the list by a search typed in before.
         int reveals = add.IndexOf("ListSearchState.Reveal(SearchBar, RuleGrid, added);", StringComparison.Ordinal);
         Assert.True(reveals >= 0 && reveals < add.IndexOf("RuleGrid.SelectedItem = added;", StringComparison.Ordinal),
            "A rule added under a search that hides it is not selected.");

         // In the list's own view, without scrolling the page from the add form.
         string show = WithheldRowTests.MemberText(code, "private void ShowRowInList_(RuleRow row)");
         Assert.True(show != null, "RulesView.ShowRowInList_ is gone.");
         Assert.Contains("list.ScrollToEnd();", show, StringComparison.Ordinal);
         Assert.Contains("FocusWithNotice_(AddCriterionButton)", add, StringComparison.Ordinal);
         string step = WithheldRowTests.MemberText(code, "private void FocusWithNotice_(Control step)");
         Assert.True(step != null, "RulesView.FocusWithNotice_ is gone.");
         Assert.Contains("step.Focus()", step, StringComparison.Ordinal);
         Assert.Contains("DispatcherPriority.Loaded", step, StringComparison.Ordinal);
         Assert.Contains("shown.Bottom - notice.Top <= PageScroll.ViewportHeight", step, StringComparison.Ordinal);
         Assert.DoesNotContain("SayOutcome_(StatusLevel.Information, RuleCards.Added(name));", add, StringComparison.Ordinal);
      }

      [Fact]
      public void TheAdds_WaitForARule_AndTheSwitchSaysWhatItWillDo()
      {
         string code = Read(Path.Join("Views", "RulesView.xaml.cs"));
         int gate = code.IndexOf("SelectionGate.Bind(RuleGrid, MoveUpButton, MoveDownButton, ToggleButton, DeleteRuleButton,", StringComparison.Ordinal);
         Assert.True(gate >= 0, "The rule list's buttons are not gated on a selected rule.");
         Assert.StartsWith("AddCriterionButton, AddActionButton, MatchMode);", code.Substring(gate).Split(',', 6)[5].TrimStart(), StringComparison.Ordinal);
         Assert.DoesNotContain("L(\"Select a rule first.\")", code, StringComparison.Ordinal);

         string toggle = WithheldRowTests.MemberText(code, "private void ShowToggle_(RuleRow row)");
         Assert.True(toggle != null, "RulesView.ShowToggle_ is gone.");
         Assert.Contains("L(\"Di_sable\")", toggle, StringComparison.Ordinal);
         Assert.Contains("L(\"Ena_ble\")", toggle, StringComparison.Ordinal);
         Assert.DoesNotContain("Ena_ble / disable", code + Read(Path.Join("Views", "RulesView.xaml")), StringComparison.Ordinal);

         string parts = WithheldRowTests.MemberText(code, "private void ShowRuleParts_()");
         Assert.True(parts != null, "RulesView.ShowRuleParts_ is gone.");
         Assert.Contains("RuleCards.CriteriaEmpty(selectedRuleId_ != 0)", parts, StringComparison.Ordinal);
         Assert.Contains("RuleCards.ActionsEmpty(selectedRuleId_ != 0)", parts, StringComparison.Ordinal);
      }

      [Fact]
      public void TheCardsHaveTheirEmptyStates_AndASwitchedOffRuleReadsApart()
      {
         XElement page = XDocument.Parse(Read(Path.Join("Views", "RulesView.xaml"))).Root;
         XNamespace x = "http://schemas.microsoft.com/winfx/2006/xaml";
         XElement Named(string name) => page.Descendants().Single(e => (string)e.Attribute(x + "Name") == name);

         // Laid over each list, in the card's filling part, beside the list's own
         // panel - which keeps the list's floor (RulesPageTests).
         foreach ((string grid, string empty) in new[] { ("CriteriaGrid", "CriteriaEmpty"), ("ActionsGrid", "ActionsEmpty") })
            Assert.Same(Named(grid).Parent.Parent, Named(empty).Parent);

         XElement trigger = Named("RuleGrid").Descendants().Single(e => e.Name.LocalName == "DataTrigger");
         Assert.Equal("{Binding Active}", (string)trigger.Attribute("Binding"));
         Assert.Equal("False", (string)trigger.Attribute("Value"));
         XElement setter = trigger.Elements().Single();
         Assert.Equal("Foreground", (string)setter.Attribute("Property"));
         Assert.Equal("{DynamicResource TextFillColorSecondaryBrush}", (string)setter.Attribute("Value"));
         Assert.Null(Named("RuleGrid").Descendants().FirstOrDefault(e => (string)e.Attribute("Property") == "Opacity"));
      }

      private static string Read(string relative)
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
