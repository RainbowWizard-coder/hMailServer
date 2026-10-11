// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Linq;
using System.Xml.Linq;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// States shown as marks, not only as a word at the row's end (review finding
   /// F323): a blocked domain on the Allowed and blocked domains page differed from an
   /// allowed one only in its word, a rule switched off only by "No" in the last
   /// column, and the empty quarantine's glyph is a normal state's. Each test fails
   /// against the pages as they were.
   /// </summary>
   public class StateMarksTests
   {
      private static readonly XNamespace Wpf = "http://schemas.microsoft.com/winfx/2006/xaml/presentation";
      private static readonly XNamespace Scaffold = "clr-namespace:hMailServer.ControlPanel.Views.Scaffold";

      [Fact]
      public void TheActionCell_IsLedByItsStatusMark_TheCrossForBlocked()
      {
         string page = Read("DomainPolicyListView.cs");
         string column = WithheldRowTests.MemberText(page, "private static DataGridTemplateColumn ActionColumn_()");
         Assert.True(column != null, "DomainPolicyListView.ActionColumn_ is no template column with a mark.");
         Assert.Contains("mark.SetBinding(ContentPresenter.ContentProperty, new System.Windows.Data.Binding(nameof(EntryView.ActionMark)));", column, StringComparison.Ordinal);
         Assert.True(column.IndexOf("panel.AppendChild(mark);", StringComparison.Ordinal) < column.IndexOf("panel.AppendChild(word);", StringComparison.Ordinal),
            "The mark does not lead the word.");
         Assert.Contains("SortMemberPath = nameof(EntryView.Action)", column, StringComparison.Ordinal);
         // Copied as its word: a template column copies nothing of its own.
         Assert.Contains("ClipboardContentBinding = new System.Windows.Data.Binding(nameof(EntryView.Action)),", column, StringComparison.Ordinal);
         Assert.Contains("GridStyles.CellWidth(DomainPolicyWords.Action(action), ActionMarkWidth + ActionMarkGap)", column, StringComparison.Ordinal);

         string mark = WithheldRowTests.MemberText(page, "private static FrameworkElement ActionMark_(string action)");
         Assert.Contains("StatusSemantics.For(action == \"black\" ? StatusLevel.Critical : StatusLevel.Normal)", mark, StringComparison.Ordinal);
         // By resource key, so that a change of theme recolours it.
         Assert.Contains("ShapeMarkVisuals.ApplyMark(path, presentation.Shape, presentation.BrushKey);", mark, StringComparison.Ordinal);
         Assert.Contains("ActionMark = ActionMark_(action),", page, StringComparison.Ordinal);
      }

      [Fact]
      public void ARuleSwitchedOff_IsGreyed_AndItsStateIsAPill()
      {
         XElement grid = XDocument.Parse(Read("RulesView.xaml")).Root.Descendants(Wpf + "DataGrid")
            .Single(g => (string)g.Attribute(XName.Get("Name", "http://schemas.microsoft.com/winfx/2006/xaml")) == "RuleGrid");

         // The whole row of a rule switched off, the name with it, in the secondary text: the
         // grid's CellStyle (t213b, review findings F225 and F051), where this unit greyed the
         // Name column alone; the fold keeps one of the two.
         XElement cells = grid.Elements().Single(e => e.Name.LocalName == "DataGrid.CellStyle");
         XElement trigger = cells.Descendants(Wpf + "DataTrigger").Single();
         Assert.Equal("{Binding Active}", (string)trigger.Attribute("Binding"));
         Assert.Equal("False", (string)trigger.Attribute("Value"));
         Assert.Equal("{DynamicResource TextFillColorSecondaryBrush}", (string)trigger.Descendants(Wpf + "Setter").Single().Attribute("Value"));

         XElement enabled = grid.Descendants(Wpf + "DataGridTemplateColumn").Single();
         Assert.Equal("{loc:L 'Enabled'}", (string)enabled.Attribute("Header"));
         Assert.Equal("Enabled", (string)enabled.Attribute("SortMemberPath"));
         // Copied as its word, On or Off: a template column copies nothing of its own.
         Assert.Equal("{Binding Enabled}", (string)enabled.Attribute("ClipboardContentBinding"));
         XElement pill = enabled.Descendants(Scaffold + "StatusPill").Single();
         Assert.Equal("{Binding EnabledLevel}", (string)pill.Attribute("Level"));
         Assert.Equal("{Binding Enabled}", (string)pill.Attribute("Text"));
         Assert.DoesNotContain(grid.Descendants(Wpf + "DataGridTextColumn"), c => (string)c.Attribute("Binding") == "{Binding Enabled}");

         string code = Read("RulesView.xaml.cs");
         Assert.Contains("public string Enabled => Active ? L(\"On\") : L(\"Off\");", code, StringComparison.Ordinal);
         Assert.Contains("public StatusLevel EnabledLevel => Active ? StatusLevel.Good : StatusLevel.Normal;", code, StringComparison.Ordinal);
         Assert.Contains("Active = (bool)rule.Active", code, StringComparison.Ordinal);
      }

      [Fact]
      public void TheEmptyQuarantine_HasANeutralGlyph()
      {
         string page = Read("QuarantineView.cs");
         Assert.Contains("private readonly EmptyState empty_ = new() { Icon = SymbolRegular.MailInbox24,", page, StringComparison.Ordinal);
         foreach (string warning in new[] { "Warning", "Error", "Alert" })
            Assert.DoesNotContain("empty_ = new() { Icon = SymbolRegular.Mail" + warning, page, StringComparison.Ordinal);
      }

      private static string Read(string file) => HostedAddressTests.ReadControlPanelFile(Path.Join("Views", file));
   }
}
