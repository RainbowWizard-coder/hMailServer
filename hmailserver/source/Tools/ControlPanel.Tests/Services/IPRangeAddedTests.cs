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
   /// A range added on the IP ranges page is selected and explained, and the list is in
   /// the order the server decides by (review finding F230). The add form made a range
   /// that may connect but deliver in no direction, the new row was not selected, so the
   /// permissions card went on showing another range - the palette's "Let a device send
   /// mail" led here and then said nothing more - and the rows were in the server's
   /// order, though where ranges overlap the higher priority wins.
   /// </summary>
   public class IPRangeAddedTests
   {
      [Fact]
      public void ARangeAdded_IsSelected_ItsCardInView_WithWhatToDoNext()
      {
         string code = Read(Path.Join("Views", "IPRangesView.xaml.cs"));
         string add = WithheldRowTests.MemberText(code, "private void Add_Click(object sender, RoutedEventArgs e)");
         Assert.True(add != null, "IPRangesView.Add_Click is gone.");
         Assert.Contains("addedId = (int)range.ID;", add, StringComparison.Ordinal);
         Assert.Contains("F(\"{0} added. Addresses in the range may connect, but send no mail until you choose what they may deliver and Save permissions.\", name)", add, StringComparison.Ordinal);
         // Not left out of the list by a search typed in before, then selected.
         int reveals = add.IndexOf("ListSearchState.Reveal(SearchBar, RangeGrid, added);", StringComparison.Ordinal);
         int selects = add.IndexOf("RangeGrid.SelectedItem = added;", StringComparison.Ordinal);
         Assert.True(reveals >= 0 && selects > reveals, "A range added under a search that hides it is not selected.");
         // The card brought into view after the row, with the keyboard in it.
         int row = add.IndexOf("RangeGrid.ScrollIntoView(added);", StringComparison.Ordinal);
         int card = add.IndexOf("PageScroll.ScrollToVerticalOffset(PageScroll.VerticalOffset + top.Y);", StringComparison.Ordinal);
         Assert.True(row > selects && card > row, "The new range's card is not what the page shows.");
         Assert.Contains("PermSmtp.Focus();", add, StringComparison.Ordinal);
         Assert.Contains("Announce.Request(AddedNotice);", add, StringComparison.Ordinal);

         // The notice is shown with the range's permissions and goes with them: once
         // they are saved, here or in the range's dialog, or the card shows another
         // range, or none, or the page is opened again.
         string shows = WithheldRowTests.MemberText(code, "private void ShowPermissions_()");
         Assert.Contains("ShowAdded_();", shows, StringComparison.Ordinal);
         Assert.Contains("ShowAdded_();", WithheldRowTests.MemberText(code, "private void ShowNoRange()"), StringComparison.Ordinal);
         string notice = WithheldRowTests.MemberText(code, "private void ShowAdded_()");
         Assert.Contains("if (permsRow_ == null || permsRow_.Id != addedRangeId_)", notice, StringComparison.Ordinal);
         string save = WithheldRowTests.MemberText(code, "private bool SavePermissions_(RangeRow row)");
         Assert.Contains("if (row.Id == addedRangeId_)", save, StringComparison.Ordinal);
         string edit = WithheldRowTests.MemberText(code, "private void Edit_Click(object sender, RoutedEventArgs e)");
         Assert.Contains("if (dialog.Saved && row.Id == addedRangeId_)", edit, StringComparison.Ordinal);
         Assert.Contains("Saved = true;", Read(Path.Join("Views", "IPRangeDialog.cs")), StringComparison.Ordinal);
         Assert.Contains("addedText_ = null;", WithheldRowTests.MemberText(code, "public void OnEnter()"), StringComparison.Ordinal);

         XElement page = XDocument.Parse(Read(Path.Join("Views", "IPRangesView.xaml"))).Root;
         XNamespace x = "http://schemas.microsoft.com/winfx/2006/xaml";
         XElement added = page.Descendants().Single(e => (string)e.Attribute(x + "Name") == "AddedNotice");
         Assert.Equal("PermGrid", (string)added.Parent.Attribute(x + "Name"));
         Assert.Same(added, added.Parent.Elements().First());
      }

      [Fact]
      public void TheList_IsInPriorityOrder_TheHighestFirst_UntilAHeaderIsClicked()
      {
         string code = Read(Path.Join("Views", "IPRangesView.xaml.cs"));
         string reload = WithheldRowTests.MemberText(code, "private void Reload()");
         // The order is read before the rows are replaced, which clears it, and set after.
         int keeps = reload.IndexOf("RangeGrid.Items.SortDescriptions.ToArray()", StringComparison.Ordinal);
         int replaces = reload.IndexOf("RangeGrid.ItemsSource = rows;", StringComparison.Ordinal);
         int sorts = reload.IndexOf("SortRows_(order, directions);", StringComparison.Ordinal);
         Assert.True(keeps >= 0 && replaces > keeps && sorts > replaces, "The list's order is not kept across a read.");

         string sort = WithheldRowTests.MemberText(code, "private void SortRows_(SortDescription[] order, ListSortDirection?[] directions)");
         Assert.True(sort != null, "IPRangesView.SortRows_ is gone.");
         Assert.Contains("PriorityColumn.SortDirection = ListSortDirection.Descending;", sort, StringComparison.Ordinal);
         Assert.Contains("new(nameof(RangeRow.Priority), ListSortDirection.Descending)", code, StringComparison.Ordinal);
         Assert.Contains("new(nameof(RangeRow.Name), ListSortDirection.Ascending)", code, StringComparison.Ordinal);

         XElement page = XDocument.Parse(Read(Path.Join("Views", "IPRangesView.xaml"))).Root;
         XNamespace x = "http://schemas.microsoft.com/winfx/2006/xaml";
         XElement column = page.Descendants().Single(e => (string)e.Attribute(x + "Name") == "PriorityColumn");
         Assert.Equal("{Binding Priority}", (string)column.Attribute("Binding"));
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
