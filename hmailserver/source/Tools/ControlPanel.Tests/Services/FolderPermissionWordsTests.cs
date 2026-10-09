// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// Review finding F204: the folder permissions list summed an entry's rights up
   /// with the first word of each right's caption, so Delete folder and Delete
   /// messages both read "Delete", and in German the rights were "Ordner",
   /// "Nachrichten" and "Gelesen/Ungelesen-Status". Each right now has a catalogued
   /// short name of its own (FolderPermissionsDialog.RightWords), and every right the
   /// entry dialog offers has one. Scanned rather than rendered: this assembly has
   /// no WPF. The test fails against the tree before the fix.
   /// </summary>
   public class FolderPermissionWordsTests
   {
      [Fact]
      public void EveryRight_HasAShortNameOfItsOwn_AndTheRowUsesIt()
      {
         string source = ReadControlPanelFile(Path.Join("Views", "FolderPermissionsDialog.cs"));

         string describe = WithheldRowTests.MemberText(source, "private static string Describe(dynamic p)");
         Assert.True(describe != null, "FolderPermissionsDialog.Describe is gone.");
         Assert.DoesNotContain(".Split(' ')[0]", describe, StringComparison.Ordinal);
         Assert.Contains("in RightWords", describe, StringComparison.Ordinal);

         int[] flagBits = Bits(Block(source, "Flags ="));
         int[] wordBits = Bits(Block(source, "RightWords ="));
         Assert.Equal(11, flagBits.Length);
         Assert.Equal(flagBits.OrderBy(b => b), wordBits.OrderBy(b => b));

         string[] words = Regex.Matches(Block(source, "RightWords ="), @"L\(""([^""]+)""\)").Select(m => m.Groups[1].Value).ToArray();
         Assert.Equal(words.Length, words.Distinct(StringComparer.Ordinal).Count());
         Assert.Contains("Delete folder", words);
         Assert.Contains("Delete messages", words);
      }

      private static string Block(string source, string start)
      {
         int at = source.IndexOf(start, StringComparison.Ordinal);
         Assert.True(at > 0, "FolderPermissionsDialog has no " + start.TrimEnd(' ', '='));
         int open = source.IndexOf('{', at);
         int close = source.IndexOf("};", open, StringComparison.Ordinal);
         return source.Substring(open, close - open);
      }

      private static int[] Bits(string block)
         => Regex.Matches(block, @"(?:\(|,\s*)(\d+)\s*,").Select(m => int.Parse(m.Groups[1].Value, System.Globalization.CultureInfo.InvariantCulture)).ToArray();

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
