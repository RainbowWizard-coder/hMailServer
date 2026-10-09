// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The Ctrl+K palette says the row its arrow keys reach (review finding F018).
   /// The search box keeps the keyboard for the palette's whole life and the list
   /// cannot take it, so a screen reader that follows the keyboard heard only the
   /// typed letters while the selection moved; each row's name was set and never
   /// said. The palette now raises a notification, "3 of 12:" and the row's name,
   /// as the selection moves and as typing changes the results, and the search
   /// box's help text is the footer's key legend.
   /// </summary>
   public class PaletteAnnouncementTests
   {
      [Fact]
      public void APosition_CountsTheRowsThatCanBeChosen_NotTheCaptions()
      {
         IReadOnlyList<PaletteRow> rows = PaletteSearch.Query("", null, null);
         int first = PaletteSearch.FirstSelectable(rows);
         int last = PaletteSearch.LastSelectable(rows);
         int count = rows.Count(r => r.IsSelectable);

         Assert.Equal((1, count), PaletteSearch.PositionOf(rows, first));
         Assert.Equal((count, count), PaletteSearch.PositionOf(rows, last));
         Assert.Equal((2, count), PaletteSearch.PositionOf(rows, PaletteSearch.NextSelectable(rows, first, 1)));

         // A caption is not a place the arrows stop, and has none.
         int header = rows.ToList().FindIndex(r => r.Kind == PaletteRowKind.Header);
         Assert.Equal((0, 0), PaletteSearch.PositionOf(rows, header));
         Assert.Equal((0, 0), PaletteSearch.PositionOf(rows, -1));
         Assert.Equal((0, 0), PaletteSearch.PositionOf(rows, rows.Count));
      }

      [Fact]
      public void TheAnnouncement_IsThePositionThenTheRowsName()
      {
         IReadOnlyList<PaletteRow> rows = PaletteSearch.Query("", null, null);
         int first = PaletteSearch.FirstSelectable(rows);
         int count = rows.Count(r => r.IsSelectable);

         Assert.Equal("1 of " + count + ": Domains. in Accounts & domains", PaletteSearch.Announcement(rows, first, "Domains. in Accounts & domains"));
         Assert.Equal("", PaletteSearch.Announcement(rows, -1, "anything"));
      }

      [Fact]
      public void ThePalette_SaysTheRowAsTheSelectionMovesAndAsTypingChangesIt()
      {
         string palette = Read(Path.Join("Views", "NavigationPalette.cs"));

         Assert.Matches(new Regex(@"private void Select\(int index\)[\s\S]{0,300}?Announce_\(index\);"), palette);
         Assert.Matches(new Regex(@"private void Filter\(\)[\s\S]{0,2000}?Announce_\(anything \? first : -1\);"), palette);
         Assert.Contains("ScreenReaderNotice.Say(searchBox_, sentence, \"hMailServerPaletteRow\");", palette, StringComparison.Ordinal);
         Assert.Contains("PaletteSearch.Announcement(rows_, index, AutomationProperties.GetName(", palette, StringComparison.Ordinal);
         Assert.Contains("AutomationProperties.SetHelpText(searchBox_, L(\"Up and down arrows to move, Enter to open, Escape to close\"));", palette, StringComparison.Ordinal);
      }

      /// <summary>A notification, replacing the last of its kind, never an exception when nothing listens.</summary>
      [Fact]
      public void TheNotice_IsANotificationThatReplacesTheLast()
      {
         string notice = Read(Path.Join("Services", "ScreenReaderNotice.cs"));

         Assert.Contains("RaiseNotificationEvent(AutomationNotificationKind.ActionCompleted,", notice, StringComparison.Ordinal);
         Assert.Contains("AutomationNotificationProcessing.MostRecent", notice, StringComparison.Ordinal);
         Assert.Contains("catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))", notice, StringComparison.Ordinal);
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
