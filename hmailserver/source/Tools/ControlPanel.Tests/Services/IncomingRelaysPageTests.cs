// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Text.RegularExpressions;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The Incoming relays page (review finding F221): a grid of the relays' name
   /// and two addresses, each row carrying the relay's id, a toolbar with a search
   /// and the row verbs, Edit… opening the relay's dialog, and Delete selected
   /// deleting by the id, where the page listed concatenated strings and deleted
   /// the first relay whose name matched the text before "   (".
   /// </summary>
   public class IncomingRelaysPageTests
   {
      [Fact]
      public void TheList_IsAGridOfTheRelaysColumns()
      {
         string page = Page();
         Assert.Contains("private readonly DataGrid relays_ = new()", page, StringComparison.Ordinal);
         Assert.DoesNotContain("private readonly ListView list_", page, StringComparison.Ordinal);
         Assert.Contains("Binding = new System.Windows.Data.Binding(nameof(IncomingRelayRow.Name)),", page, StringComparison.Ordinal);
         Assert.Contains("MinWidth = Math.Max(160, GridStyles.HeaderWidth(L(\"Name\")))", page, StringComparison.Ordinal);
         Assert.Contains("relays_.Columns.Add(AddressColumn_(L(\"Lower IP\"), nameof(IncomingRelayRow.LowerIP)));", page, StringComparison.Ordinal);
         Assert.Contains("relays_.Columns.Add(AddressColumn_(L(\"Upper IP\"), nameof(IncomingRelayRow.UpperIP)));", page, StringComparison.Ordinal);

         string address = WithheldRowTests.MemberText(page, "private static DataGridTextColumn AddressColumn_(string header, string path)");
         Assert.True(address != null, "IncomingRelaysView.AddressColumn_ is gone.");
         Assert.Contains("Math.Max(170, GridStyles.HeaderWidth(header))", address, StringComparison.Ordinal);
         Assert.Contains("GridStyles.Trimmed(", address, StringComparison.Ordinal);

         Assert.Contains("rows.Children.Add(FillPanel.Filling(relays_, FillPanel.ListRowsFloor));", page, StringComparison.Ordinal);
         Assert.Contains("parts.Children.Add(FillPanel.Filling(listCard, 0));", page, StringComparison.Ordinal);
      }

      /// <summary>A row is the relay's own, by its id, and says its addresses with their columns' names.</summary>
      [Fact]
      public void ARow_CarriesTheRelaysId()
      {
         string dialog = Dialog();
         Assert.Matches(new Regex(@"class IncomingRelayRow\s*\{[\s\S]*?public int Id \{ get; init; \}"), dialog);
         Assert.Contains("=> RowSpeech.Of(Name, RowSpeech.Labelled(L(\"Lower IP\"), LowerIP), RowSpeech.Labelled(L(\"Upper IP\"), UpperIP));", dialog, StringComparison.Ordinal);

         string reload = WithheldRowTests.MemberText(Page(), "private void Reload()");
         Assert.True(reload != null, "IncomingRelaysView.Reload is gone.");
         Assert.Contains("Id = (int)relay.ID,", reload, StringComparison.Ordinal);
         Assert.DoesNotContain("\"   (\"", reload, StringComparison.Ordinal);
      }

      /// <summary>Delete selected deletes the relay by its id, where it deleted the first relay whose name matched the row's text.</summary>
      [Fact]
      public void DeleteSelected_DeletesByTheId()
      {
         string delete = WithheldRowTests.MemberText(Page(), "private void DeleteSelected()");
         Assert.True(delete != null, "IncomingRelaysView.DeleteSelected is gone.");
         Assert.Contains("relay = relays.ItemByDBID[row.Id];", delete, StringComparison.Ordinal);
         Assert.Contains("relay.Delete();", delete, StringComparison.Ordinal);
         Assert.DoesNotContain("Split(", delete, StringComparison.Ordinal);
         Assert.DoesNotContain("relay.Name ==", delete, StringComparison.Ordinal);
         Assert.Contains("Say_(StatusLevel.Good, ListOutcome.Deleted(row.Name));", delete, StringComparison.Ordinal);
         Assert.Contains("Say_(StatusLevel.Critical, F(\"Could not delete the relay: {0}\", ServerSession.DescribeComError(ex)));", delete, StringComparison.Ordinal);
      }

      /// <summary>The toolbar holds the search and the row verbs; the header the count and Refresh.</summary>
      [Fact]
      public void TheToolbar_HoldsTheSearchAndTheRowVerbs()
      {
         string page = Page();
         Assert.Contains("private readonly Toolbar toolbar_ = new() { SearchPlaceholder = L(\"Name or address contains\") };", page, StringComparison.Ordinal);
         Assert.Contains("toolbar_.SearchTextChanged += (s, e) => ListSearch.Apply(relays_, toolbar_.SearchText);", page, StringComparison.Ordinal);
         Assert.Contains("var edit = new Wpf.Ui.Controls.Button { Content = L(\"_Edit…\"), Margin = new Thickness(0, 0, 8, 0) };", page, StringComparison.Ordinal);
         int edit = page.IndexOf("actions.Children.Add(edit);", StringComparison.Ordinal);
         int del = page.IndexOf("actions.Children.Add(del);", StringComparison.Ordinal);
         Assert.True(edit >= 0 && del > edit, "Edit… and Delete selected are not the toolbar's actions, in that order.");
         Assert.Contains("SelectionGate.Bind(relays_, edit, del);", page, StringComparison.Ordinal);
         Assert.Contains("Status = count_,", page, StringComparison.Ordinal);
         Assert.Contains("Actions = refresh", page, StringComparison.Ordinal);
         Assert.Contains("count_.Text = Plural(rows.Count, \"{0} incoming relay\", \"{0} incoming relays\");", page, StringComparison.Ordinal);
      }

      /// <summary>Edit… opens the relay's dialog, which reads and saves it by its id, and the page says it was saved.</summary>
      [Fact]
      public void Edit_SavesTheRelayInPlace()
      {
         string edit = WithheldRowTests.MemberText(Page(), "private void EditSelected()");
         Assert.True(edit != null, "IncomingRelaysView.EditSelected is gone.");
         Assert.Contains("IncomingRelayDialog.Edit(Window.GetWindow(this), row.Id)", edit, StringComparison.Ordinal);
         Assert.Contains("Say_(StatusLevel.Good, ListOutcome.Saved(saved, DateTime.Now));", edit, StringComparison.Ordinal);

         string dialog = Dialog();
         string write = WithheldRowTests.MemberText(dialog, "private static bool Write_(int id, string name, string lower, string upper, out string refusal)");
         Assert.True(write != null, "IncomingRelayDialog.Write_ is gone.");
         Assert.Contains("relay = relays.ItemByDBID[id];", write, StringComparison.Ordinal);
         Assert.Contains("relay.Save();", write, StringComparison.Ordinal);
         Assert.Contains("DialogFields.Field(L(\"_Name\"), name)", dialog, StringComparison.Ordinal);
         Assert.Contains("DialogFields.Field(L(\"_Lower IP\"), lower,", dialog, StringComparison.Ordinal);
         Assert.Contains("DialogFields.Field(L(\"_Upper IP\"), upper,", dialog, StringComparison.Ordinal);
      }

      private static string Page()
      {
         string all = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "UtilityViews.cs"));
         int start = all.IndexOf("public class IncomingRelaysView", StringComparison.Ordinal);
         int end = all.IndexOf("public class MxQueryView", StringComparison.Ordinal);
         Assert.True(start >= 0 && end > start, "IncomingRelaysView could not be found in UtilityViews.cs.");
         return all.Substring(start, end - start);
      }

      private static string Dialog() => ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "IncomingRelayDialog.cs"));
   }
}
