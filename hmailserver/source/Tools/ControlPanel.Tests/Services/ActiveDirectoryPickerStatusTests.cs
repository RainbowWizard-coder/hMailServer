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
   /// Review finding F124: the Browse Active Directory dialog said every state - Active
   /// Directory not reachable, no domain, a list or a search that failed, a search
   /// under way, what it found - in one line of grey caption text with no mark and no
   /// next step, the reason Windows gave joined on with a dash; Search stayed live
   /// where nothing could be searched, and pressing it said "Select a domain first.";
   /// Domain was a 12-pixel caption beside its list; and the search box had no caption
   /// at all. The status is a notice at the level of what it says, Search follows the
   /// domain list and the name box, and both are captioned fields, the name box saying
   /// what an empty box does.
   /// </summary>
   public class ActiveDirectoryPickerStatusTests
   {
      [Fact]
      public void TheStatus_IsANotice_AtTheLevelOfWhatItSays()
      {
         string dialog = Read("Views", "ActiveDirectoryPickerDialog.cs");
         Assert.Contains("private readonly InlineNotice status_ = new()", dialog, StringComparison.Ordinal);
         Assert.DoesNotMatch(new Regex(@"status_\.Text = [LF]\("), dialog);
         Assert.DoesNotMatch(new Regex(@"status_\.Text = reason"), dialog);
         Assert.DoesNotContain("DialogFields.Note(", dialog, StringComparison.Ordinal);

         Assert.Contains("Say_(StatusLevel.Warning, reason);", dialog, StringComparison.Ordinal);
         Assert.Matches(new Regex(@"Say_\(StatusLevel\.Warning, L\(""No Active Directory domains could be found"), dialog);
         Assert.Matches(new Regex(@"Say_\(StatusLevel\.Critical, F\(""Could not list domains: \{0\}"""), dialog);
         Assert.Matches(new Regex(@"Say_\(StatusLevel\.Critical, F\(""Search failed: \{0\}"""), dialog);
         Assert.Matches(new Regex(@"Say_\(StatusLevel\.Information, F\(""Searching \{0\}…"""), dialog);
      }

      [Fact]
      public void Search_FollowsWhetherThereIsADomainToSearch()
      {
         string dialog = Read("Views", "ActiveDirectoryPickerDialog.cs");
         Match body = Regex.Match(dialog, @"private void QueryCanRun_\(bool canRun\)\s*\{(.*?)\n      \}", RegexOptions.Singleline);
         Assert.True(body.Success, "QueryCanRun_ is gone.");
         foreach (string control in new[] { "domainBox_", "searchBox_", "searchButton_" })
            Assert.Contains(control + ".IsEnabled = canRun;", body.Groups[1].Value, StringComparison.Ordinal);

         Assert.Equal(3, Regex.Matches(dialog, @"QueryCanRun_\(false\);").Count);
         Assert.DoesNotContain("L(\"Select a domain first.\")", dialog, StringComparison.Ordinal);
      }

      [Fact]
      public void TheDomainAndTheName_AreCaptionedFields_OnOneLine()
      {
         string dialog = Read("Views", "ActiveDirectoryPickerDialog.cs");
         Assert.Contains("FieldRow domainRow = DialogFields.Field(L(\"Active Directory _domain\"), domainBox_);", dialog, StringComparison.Ordinal);
         Assert.Contains("FieldRow searchRow = DialogFields.Field(L(\"_Name or account\"), searchBox_);", dialog, StringComparison.Ordinal);
         Assert.Equal(2, Regex.Matches(dialog, @"Row\.LabelGroup = ""AdQueryLine"";").Count);
         Assert.Contains("SharedSizeGroup = \"AdQueryLine\"", dialog, StringComparison.Ordinal);
         // What an empty box does, in the caption form's words for it (review finding F148).
         Assert.Contains("PlaceholderText = L(\"Part of a name (empty = everyone)\")", dialog, StringComparison.Ordinal);
         // The caption names the box now, not a sentence put in its place.
         Assert.DoesNotContain("AutomationProperties.SetName(searchBox_", dialog, StringComparison.Ordinal);
      }

      [Fact]
      public void WindowsReason_IsLast_AfterWhatItMeansHere()
      {
         string service = Read("Services", "ActiveDirectoryService.cs");
         Assert.Contains("F(\"Active Directory cannot be reached from this computer, so its accounts cannot be browsed here. Windows reported: {0}\", ServerSession.DescribeComError(ex))", service, StringComparison.Ordinal);
         Assert.DoesNotContain("F(\"Active Directory is not reachable from this computer: {0}\"", service, StringComparison.Ordinal);
      }

      private static string Read(string folder, string file)
         => FormLineAssert.ReadControlPanelFile(Path.Join(folder, file));
   }
}
