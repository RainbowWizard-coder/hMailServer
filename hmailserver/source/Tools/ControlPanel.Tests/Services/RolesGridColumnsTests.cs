// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The Administrator roles page's grids share their card's width and never scroll
   /// sideways (review finding F068). The roles grid's four fixed columns, 150, 300,
   /// 80 and 380 pixels, sat in a card capped at 980 with about 900 for them, so a
   /// scroll bar ran under the rows and the sentences the grid is for read "Reset
   /// passwords and unlock accounts in one d..." at 1280; the assignments grid, 260,
   /// 150 and 260, ended some 230 pixels short of its card; and the cards ended short
   /// of the header's button.
   /// </summary>
   public class RolesGridColumnsTests
   {
      // The narrowest a card's grid is: a 760-pixel window, less the rail, the page's
      // padding, the scroll bar's inset and the card's padding.
      private const double NarrowestGrid = 760 - 48 - 48 - 12 - 40;

      [Fact]
      public void TheRolesGrid_IsTheRoleAndBuiltInAsWideAsTheirWords_AndTheSentencesSharingTheRest()
      {
         string page = Read();
         Assert.Contains("roleGrid_.Columns.Add(TextColumn_(L(\"Role\"), nameof(RoleRow.Name), FixedWidth_(L(\"Role\"), 140)));", page, StringComparison.Ordinal);
         Assert.Contains("roleGrid_.Columns.Add(TextColumn_(L(\"What it is for\"), nameof(RoleRow.Purpose), StarWidth_(2), wrap: true));", page, StringComparison.Ordinal);
         Assert.Contains("roleGrid_.Columns.Add(TextColumn_(L(\"Built in\"), nameof(RoleRow.BuiltIn), FixedWidth_(L(\"Built in\"), 80, L(\"Yes\"), L(\"No\"))));", page, StringComparison.Ordinal);
         Assert.Contains("roleGrid_.Columns.Add(TextColumn_(L(\"Permissions\"), nameof(RoleRow.Permissions), StarWidth_(3), wrap: true));", page, StringComparison.Ordinal);
      }

      [Fact]
      public void TheAssignmentsGrid_SharesItsCard_TheRoleBetweenAsWideAsItsWords()
      {
         string page = Read();
         Assert.Contains("assignmentGrid_.Columns.Add(TextColumn_(L(\"Account\"), nameof(AssignmentRow.Address), StarWidth_(2)));", page, StringComparison.Ordinal);
         Assert.Contains("assignmentGrid_.Columns.Add(TextColumn_(L(\"Role\"), nameof(AssignmentRow.Role), FixedWidth_(L(\"Role\"), 140)));", page, StringComparison.Ordinal);
         Assert.Contains("assignmentGrid_.Columns.Add(TextColumn_(L(\"Domains\"), nameof(AssignmentRow.Domains), StarWidth_(2)));", page, StringComparison.Ordinal);
      }

      [Fact]
      public void NeitherGrid_ScrollsSideways_AndBothFitTheNarrowestWindow()
      {
         string page = Read();
         Assert.Contains("ScrollViewer.SetHorizontalScrollBarVisibility(roleGrid_, ScrollBarVisibility.Disabled);", page, StringComparison.Ordinal);
         Assert.Contains("ScrollViewer.SetHorizontalScrollBarVisibility(assignmentGrid_, ScrollBarVisibility.Disabled);", page, StringComparison.Ordinal);

         // Every column's floor together, the fixed ones at their least, fits the
         // narrowest card, so that no column is squeezed below its floor there.
         double floor = double.Parse(Regex.Match(page, @"private const double SentenceColumnFloor = (\d+);").Groups[1].Value, CultureInfo.InvariantCulture);
         Assert.True(140 + 80 + 2 * floor <= NarrowestGrid, "The roles grid's floors are wider than its card in a 760-pixel window.");
         Assert.True(2 * floor + 140 <= NarrowestGrid, "The assignments grid's floors are wider than its card in a 760-pixel window.");
         Assert.Contains("MinWidth = size.Floor", page, StringComparison.Ordinal);
         // The sentences wrap, the rest are trimmed with the whole in a tool tip.
         Assert.Contains("lines.Setters.Add(new Setter(TextBlock.TextWrappingProperty, TextWrapping.Wrap));", page, StringComparison.Ordinal);
         Assert.Contains("return GridStyles.Trimmed(column);", page, StringComparison.Ordinal);
      }

      [Fact]
      public void TheCards_AreAsWideAsThePage_LessTheScrollBarsRoom()
      {
         string page = Read();
         Assert.Contains("var cards = new StackPanel { Margin = new Thickness(0, 0, DesignTokens.Space.Md, 0) };", page, StringComparison.Ordinal);
         Assert.DoesNotContain("MaxWidth = 980", page, StringComparison.Ordinal);
      }

      private static string Read()
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", "Views", "RolesView.cs");
            if (File.Exists(candidate))
               return File.ReadAllText(candidate).Replace("\r\n", "\n");
            directory = directory.Parent;
         }

         Assert.Fail("ControlPanel/Views/RolesView.cs could not be found from the test binaries.");
         return null;
      }
   }
}
