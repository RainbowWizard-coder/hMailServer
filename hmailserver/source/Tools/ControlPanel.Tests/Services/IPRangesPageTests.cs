// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Xml.Linq;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The IP ranges page as work item 79 found it: "Existing IP ranges are not
   /// displayed in the administration panel". In 6.3.4 the list was a star row of
   /// the page's grid between rows sized to their content and nothing scrolled, so
   /// a window shorter than the toolbar, the permissions card and the add form
   /// together gave the list no height at all, and no range could be seen, selected,
   /// edited or deleted. 6.3.5 held the page to the window over a fixed 560 pixels,
   /// which kept the list but cut the add form off in the narrowest window. Every
   /// test here fails against both: the list is the filling part of a
   /// <c>FillPanel</c> (whose arithmetic PageFillTests holds), no grid row and no
   /// fixed height stands between it and the page's scroll viewer, the actions
   /// wrap, the columns keep a width, and the permissions card shows the selected
   /// range's permissions or none.
   /// </summary>
   public class IPRangesPageTests
   {
      private static readonly XNamespace Wpf = "http://schemas.microsoft.com/winfx/2006/xaml/presentation";
      private static readonly XNamespace X = "http://schemas.microsoft.com/winfx/2006/xaml";
      private static readonly XNamespace Scaffold = "clr-namespace:hMailServer.ControlPanel.Views.Scaffold";

      [Fact]
      public void TheList_IsTheFillingPartOfAFillPanel_InThePageScrollViewer()
      {
         XElement page = Page();
         XElement scroll = Named(page, "PageScroll");
         Assert.Equal(Wpf + "ScrollViewer", scroll.Name);
         Assert.Equal("Auto", (string)scroll.Attribute("VerticalScrollBarVisibility"));

         XElement panel = scroll.Elements(Scaffold + "FillPanel").SingleOrDefault();
         Assert.True(panel != null, "The page ScrollViewer's content is not a FillPanel, so nothing sizes the list from the parts around it.");
         Assert.Equal("{Binding ViewportHeight, ElementName=PageScroll}", (string)panel.Attribute("ViewportHeight"));

         List<XElement> filling = panel.Elements().Where(e => (string)e.Attribute(Scaffold + "FillPanel.Fill") == "True").ToList();
         Assert.True(filling.Count == 1, "The FillPanel has " + filling.Count + " filling parts; the list's card is to be the one.");
         Assert.Contains(filling[0].Descendants(Wpf + "DataGrid"), g => (string)g.Attribute(X + "Name") == "RangeGrid");

         // The floor: the card's padding (16) and border (2), the column headers
         // (33) and three rows of AppNavRowHeight (34). Below that the page scrolls
         // rather than the list shrinking to a sliver with no row in it. And the
         // empty state that takes the rows' place when there are none - its glyph,
         // its sentence and their padding, 139 - inside the same padding and
         // border: at 154 its sentence was cut off.
         // Since the empty state carries an action (review finding F207), the
         // card is floored at what it holds: the grid's three rows on a FillPanel
         // of its own, and the empty state laid over them, which raises the floor
         // where its sentence and its action need more (EmptyStateActionTests).
         string unheld = EmptyStateActionTests.ListCardFloorProblem(filling[0], "RangeGrid", 3);
         Assert.True(unheld == null, unheld);
      }

      /// <summary>
      /// The two shapes that lost the list and then the add form: a grid's star row
      /// between the page's scroll viewer and the list, which a short window gives
      /// nothing, and a part held to the viewport's height over a fixed MinHeight,
      /// which knows nothing of how tall the parts are.
      /// </summary>
      [Fact]
      public void NoGridRowAndNoFixedHeight_StandsBetweenTheListAndThePage()
      {
         XElement page = Page();
         XElement grid = Named(page, "RangeGrid");
         XElement scroll = Named(page, "PageScroll");

         for (XElement e = grid.Parent; e != null && e != scroll; e = e.Parent)
         {
            Assert.False(e.Elements(Wpf + "Grid.RowDefinitions").Any(),
               "The list sits in a row of a " + e.Name.LocalName + ", which a window shorter than the other rows gives nothing (6.3.4).");
         }

         foreach (XElement e in page.Descendants())
         {
            string height = (string)e.Attribute("Height") ?? "";
            Assert.False(height.Contains("ViewportHeight", StringComparison.Ordinal),
               e.Name.LocalName + " is held to the viewport's height, which cuts off whatever its parts need beyond it (6.3.5's add form).");
         }
      }

      [Fact]
      public void TheToolbarActions_Wrap_SoPropertiesAndDeleteStayOnThePage()
      {
         XElement page = Page();
         XElement properties = Named(page, "PropertiesButton");
         XElement delete = Named(page, "DeleteRangeButton");

         Assert.Equal(Wpf + "WrapPanel", properties.Parent.Name);
         Assert.Same(properties.Parent, delete.Parent);
         Assert.Equal(Scaffold + "Toolbar.Actions", properties.Parent.Parent.Name);
      }

      [Fact]
      public void EveryColumn_HasAFloor_SoANarrowWindowScrollsTheGridSideways()
      {
         XElement grid = Named(Page(), "RangeGrid");
         List<XElement> columns = grid.Descendants(Wpf + "DataGridTextColumn").ToList();
         // The seven of the range, and when it ends (review finding F231).
         Assert.Equal(8, columns.Count);

         foreach (XElement column in columns)
         {
            string header = (string)column.Attribute("Header");
            Assert.True(column.Attribute("MinWidth") != null, "The column " + header + " has no MinWidth, so a narrow window squeezes it to nothing.");
         }

         XElement name = columns[0];
         Assert.Equal("{loc:L 'Name'}", (string)name.Attribute("Header"));
         Assert.True(double.Parse((string)name.Attribute("MinWidth"), CultureInfo.InvariantCulture) >= 120,
            "The name, the one column that says which range a row is, is narrower than a name.");
      }

      /// <summary>
      /// The reporter's permissions card said "select a range" with seven boxes
      /// ticked: the boxes kept the last range's values when the selection went,
      /// and a save, an edit or an add dropped the selection by reloading the rows.
      /// </summary>
      [Fact]
      public void WithNoRangeSelected_ThePermissionsAreClearAndDisabled()
      {
         string code = CodeBehind();
         string none = WithheldRowTests.MemberText(code, "private void ShowNoRange()");
         Assert.True(none != null, "IPRangesView.ShowNoRange is gone.");
         foreach (string step in new[] { "box.IsChecked = false;", "PermGrid.IsEnabled = false;", "SavePermsButton.IsEnabled = false;", "Permissions (select a range)" })
            Assert.True(none.Contains(step, StringComparison.Ordinal), "ShowNoRange no longer does " + step);

         string boxes = WithheldRowTests.MemberText(code, "private IEnumerable<CheckBox> PermissionBoxes()");
         Assert.True(boxes != null, "IPRangesView.PermissionBoxes is gone.");
         foreach (string box in new[] { "PermSmtp", "PermImap", "PermPop3", "PermTlsAuth", "PermLL", "PermLR", "PermRL", "PermRR" })
            Assert.True(boxes.Contains(box, StringComparison.Ordinal), box + " is not cleared with the others.");

         string constructor = WithheldRowTests.MemberText(code, "public IPRangesView()");
         Assert.True(constructor != null && constructor.Contains("ShowNoRange();", StringComparison.Ordinal),
            "The page opens with the permission boxes live and nothing selected.");

         // The selection's change shows the selected range's permissions, or none,
         // through ShowPermissions_ - once any unsaved changes on the card have been
         // asked about (UnsavedChangesTests).
         string changed = WithheldRowTests.MemberText(code, "private void RangeGrid_SelectionChanged(object sender, SelectionChangedEventArgs e)");
         Assert.True(changed != null, "IPRangesView.RangeGrid_SelectionChanged is gone.");
         Assert.True(changed.Contains("ShowPermissions_();", StringComparison.Ordinal), "Choosing a range no longer shows its permissions.");

         string shows = WithheldRowTests.MemberText(code, "private void ShowPermissions_()");
         Assert.True(shows != null, "IPRangesView.ShowPermissions_ is gone.");
         Assert.True(shows.Contains("ShowNoRange();", StringComparison.Ordinal), "A selection that goes away leaves the last range's permissions on show.");
         Assert.True(shows.Contains("PermGrid.IsEnabled = true;", StringComparison.Ordinal), "A selected range's permissions cannot be changed.");
      }

      [Fact]
      public void AReload_KeepsTheSelectedRange_AndRangesAreFoundByTheirId()
      {
         string code = CodeBehind();
         string reload = WithheldRowTests.MemberText(code, "private void Reload()");
         Assert.True(reload != null, "IPRangesView.Reload is gone.");
         Assert.True(reload.Contains("(RangeGrid.SelectedItem as RangeRow)?.Id", StringComparison.Ordinal) &&
                     reload.Contains("RangeGrid.SelectedItem = again;", StringComparison.Ordinal),
            "Reload no longer selects again the range that was selected, so every save, edit and add drops the selection.");

         // Selected again, not scrolled into view: ScrollIntoView brings the row into
         // the page's view too, so a save at the permissions card or an add at the
         // form scrolled the page away from the button just pressed.
         Assert.DoesNotContain(".ScrollIntoView(", reload, StringComparison.Ordinal);

         // Two ranges may share a name and a lower address; Properties, Save and
         // Delete act on the one selected, which only its ID says.
         string find = WithheldRowTests.MemberText(code, "private static dynamic FindRange(dynamic ranges, RangeRow row)");
         Assert.True(find != null && find.Contains("(int)range.ID == row.Id", StringComparison.Ordinal), "FindRange no longer finds a range by its ID.");
         Assert.DoesNotContain("(string)range.Name == row.Name", code, StringComparison.Ordinal);

         string delete = WithheldRowTests.MemberText(code, "private void Delete_Click(object sender, RoutedEventArgs e)");
         Assert.True(delete != null && delete.Contains("FindRange(ranges, row)", StringComparison.Ordinal), "Delete no longer finds the selected range by its ID.");
      }

      // ---- helpers -------------------------------------------------------------------------

      private static XElement Page() => XDocument.Parse(ReadControlPanelFile(Path.Join("Views", "IPRangesView.xaml"))).Root;

      private static string CodeBehind() => ReadControlPanelFile(Path.Join("Views", "IPRangesView.xaml.cs"));

      private static XElement Named(XElement root, string name)
      {
         XElement found = root.Descendants().FirstOrDefault(e => (string)e.Attribute(X + "Name") == name);
         Assert.True(found != null, "IPRangesView.xaml has no element named " + name + ".");
         return found;
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
