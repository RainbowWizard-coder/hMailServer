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
   /// The answer of a settings page's action button - Test, Clear, the two-factor
   /// set-ups - and the never-ban list's status line are notices at the level of
   /// what they say. Both were a line of text in the success green or the danger
   /// red with no mark and no word, so the outcome was a colour alone, and a usage
   /// hint ("Select an entry first.", "Enter a host name and port first.") or a
   /// legitimate "off" ("Control-Panel-only two-factor authentication is off.")
   /// was drawn in the red of a failure.
   ///
   /// Scanned rather than rendered, as StatusWarningLevelTests is: this assembly
   /// has no WPF. Each test fails against the tree before the change.
   /// </summary>
   public class ActionOutcomeLevelTests
   {
      [Fact]
      public void TheActionResult_IsANoticeAtALevel_NotAColouredLine()
      {
         string source = ReadControlPanelFile(Path.Join("Views", "ServerSettingsView.xaml.cs"));
         string action = ClassText(source, "private class ComAction");

         Assert.Contains("private Scaffold.InlineNotice result_;", action, StringComparison.Ordinal);
         Assert.Contains("private void Report_(StatusLevel level, string text)", action, StringComparison.Ordinal);
         Assert.DoesNotContain("AppSuccessBrush", action, StringComparison.Ordinal);
         Assert.DoesNotContain("AppDangerBrush", action, StringComparison.Ordinal);

         // An action can name its level; a plain good-or-bad one is Good or
         // Critical, and the declined confirmation's "Nothing was changed." is
         // neither.
         Assert.Contains("public Func<(StatusLevel level, string text)> Outcome;", action, StringComparison.Ordinal);
         string levelOf = WithheldRowTests.MemberText(action, "internal static StatusLevel LevelOf(bool ok, string text)");
         Assert.True(levelOf != null, "ComAction.LevelOf is gone.");
         Assert.Contains("L(\"Nothing was changed.\")", levelOf, StringComparison.Ordinal);
         Assert.Contains("StatusLevel.Information", levelOf, StringComparison.Ordinal);
      }

      [Fact]
      public void TheNeverBanStatus_IsANoticeAtALevel_NotAColouredLine()
      {
         string source = ReadControlPanelFile(Path.Join("Views", "ServerSettingsView.NeverBanNames.cs"));
         Assert.Contains("private void Say_(StatusLevel level, string text)", source, StringComparison.Ordinal);
         Assert.DoesNotContain("AppSuccessBrush", source, StringComparison.Ordinal);
         Assert.DoesNotContain("AppDangerBrush", source, StringComparison.Ordinal);

         // A hint is Information; an address the list would not spare is a
         // Warning to read, not a failed check.
         Assert.Contains("Say_(StatusLevel.Information, L(\"Select an entry first.\"))", source, StringComparison.Ordinal);
         Assert.Contains("verdict.Spared ? StatusLevel.Good : StatusLevel.Warning", source, StringComparison.Ordinal);
      }

      /// <summary>
      /// The answers that are hints or a switch legitimately off are Information,
      /// and no hint is returned as a failure.
      /// </summary>
      [Theory]
      [InlineData("Enter a host name and port first.", 2)]
      [InlineData("Enter the clamscan.exe path first.", 1)]
      [InlineData("Enter the scanner command first.", 1)]
      [InlineData("ClamWin was not found in the standard install locations.", 1)]
      [InlineData("No log folder could be read from hMailServer.INI on this machine.", 1)]
      public void HintsAndLegitimateOffs_AreInformation(string english, int times)
      {
         string source = ReadControlPanelFile(Path.Join("Views", "ServerSettingsView.xaml.cs"));
         string literal = "L(\"" + english + "\")";
         Assert.Equal(times, Regex.Matches(source, Regex.Escape("(StatusLevel.Information, " + literal + ")")).Count);
         Assert.DoesNotContain("(false, " + literal + ")", source, StringComparison.Ordinal);
      }

      [Fact]
      public void IndexingSwitchedOff_IsInformation()
      {
         string source = ReadControlPanelFile(Path.Join("Views", "ServerSettingsView.xaml.cs"));
         string when = WithheldRowTests.MemberText(source, "private static (StatusLevel level, string text) WhenIndexingIsOn_(");
         Assert.True(when != null, "WhenIndexingIsOn_ is gone.");
         Assert.Contains("(StatusLevel.Information, IndexingOffNote)", when, StringComparison.Ordinal);
         Assert.Equal(2, Regex.Matches(source, @"Outcome = \(\) => WhenIndexingIsOn_\(").Count);
      }

      // ---- helpers -------------------------------------------------------------------------

      /// <summary>A class's text, from its declaration to the next class declaration at the same depth.</summary>
      private static string ClassText(string source, string declaration)
      {
         int at = source.IndexOf(declaration, StringComparison.Ordinal);
         Assert.True(at >= 0, declaration + " is gone.");
         int next = source.IndexOf("\n      private class ", at + declaration.Length, StringComparison.Ordinal);
         return next < 0 ? source.Substring(at) : source.Substring(at, next - at);
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
