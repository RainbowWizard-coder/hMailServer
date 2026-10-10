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
   /// The Active Directory picker's results were the one ListView with a
   /// GridView in the Control Panel, and so the one list that took WPF's stock
   /// look and not the theme's: a white body with light column headers,
   /// separators and an empty filler column inside the dark theme's dialog, a
   /// dark outline and centred headers in the light theme, and a fixed height of
   /// 300. It is a read-only DataGrid on the application's grid styles now, its
   /// columns sharing the width, seven rows at least, and the keyboard's Enter
   /// still chooses the row. The dialog is built in code, so these read its source.
   /// </summary>
   public class ActiveDirectoryPickerTests
   {
      [Fact]
      public void TheResults_AreAReadOnlyDataGrid_NotAGridView()
      {
         string source = Source();
         if (source == null)
            return;

         string code = Code(source);
         Assert.DoesNotContain("GridView", code, StringComparison.Ordinal);
         Assert.DoesNotContain("ListView", code, StringComparison.Ordinal);
         Assert.Matches(new Regex(@"private readonly DataGrid list_ = new\(\)"), source);
         Assert.Contains("IsReadOnly = true", source, StringComparison.Ordinal);
         Assert.Contains("AutoGenerateColumns = false", source, StringComparison.Ordinal);
         Assert.Contains("CanUserAddRows = false", source, StringComparison.Ordinal);
         Assert.Contains("SelectionUnit = DataGridSelectionUnit.FullRow", source, StringComparison.Ordinal);
      }

      [Fact]
      public void TheColumns_ShareTheWidth_AndTrimWhatDoesNotFit()
      {
         string source = Source();
         if (source == null)
            return;

         MatchCollection columns = Regex.Matches(source, @"list_\.Columns\.Add\(Column\(L\(""([^""]+)""\), nameof\(AdUser\.(\w+)\), ([\d.]+)\)\);");
         Assert.Equal(3, columns.Count);
         Assert.Equal("Account", columns[0].Groups[1].Value);
         Assert.Equal("SamAccountName", columns[0].Groups[2].Value);
         Assert.Equal("Name", columns[1].Groups[1].Value);
         Assert.Equal("DisplayName", columns[1].Groups[2].Value);
         Assert.Equal("Email", columns[2].Groups[1].Value);
         Assert.Equal("Email", columns[2].Groups[2].Value);

         Assert.Contains("DataGridLengthUnitType.Star", source, StringComparison.Ordinal);
         Assert.Contains("GridStyles.Trimmed(", source, StringComparison.Ordinal);
         Assert.Contains("ScrollViewer.SetHorizontalScrollBarVisibility(list_, ScrollBarVisibility.Disabled)", source, StringComparison.Ordinal);
      }

      [Fact]
      public void TheGrid_HasAFloorOfSevenRows_AndNoFixedHeight()
      {
         string source = Source();
         if (source == null)
            return;

         Assert.Contains("MinHeight = 240", source, StringComparison.Ordinal);
         Assert.Contains("MaxHeight = 400", source, StringComparison.Ordinal);
         Assert.DoesNotMatch(new Regex(@"\bHeight = 300\b"), source);
      }

      /// <summary>
      /// A DataGrid takes Enter itself, to move to the next row, and marks it
      /// handled before a handler on KeyDown sees it; the dialog's Enter has to
      /// be read on the preview, or the keyboard loses the way to choose a row.
      /// </summary>
      [Fact]
      public void Enter_OnTheGrid_ChoosesTheRow_OnThePreview()
      {
         string source = Source();
         if (source == null)
            return;

         Assert.Contains("list_.PreviewKeyDown +=", source, StringComparison.Ordinal);
         Assert.DoesNotContain("list_.KeyDown +=", source, StringComparison.Ordinal);
         Assert.Contains("list_.MouseDoubleClick +=", source, StringComparison.Ordinal);
         Assert.Contains("list_.SelectionChanged +=", source, StringComparison.Ordinal);
      }

      /// <summary>The source without its comments, which name what the grid replaced.</summary>
      private static string Code(string source)
         => Regex.Replace(source, @"^\s*//.*$", "", RegexOptions.Multiline);

      private static string Source()
      {
         for (var directory = new DirectoryInfo(AppContext.BaseDirectory); directory != null; directory = directory.Parent)
         {
            string path = Path.Join(directory.FullName, "ControlPanel", "Views", "ActiveDirectoryPickerDialog.cs");
            if (File.Exists(path))
               return File.ReadAllText(path);
         }

         return null;
      }
   }
}
