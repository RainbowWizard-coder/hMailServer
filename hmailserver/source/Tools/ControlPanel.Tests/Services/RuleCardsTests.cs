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
         Assert.Equal("Spam to junk added, switched off: give it its criteria and actions, then Enable switches it on.", RuleCards.Added("Spam to junk"));
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
         Assert.Contains("AddCriterionButton.Focus()", add, StringComparison.Ordinal);
         Assert.Contains("RuleCards.Added(name)", add, StringComparison.Ordinal);
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
         Assert.Equal("{Binding IsActive}", (string)trigger.Attribute("Binding"));
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
