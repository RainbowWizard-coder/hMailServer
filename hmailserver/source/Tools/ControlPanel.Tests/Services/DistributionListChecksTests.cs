// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The distribution list dialog checks its address and its two numbers on their
   /// fields before it writes, and a refusal of the server is brought into view
   /// (review finding F077). "Keep archived messages for" and "Digest hour (0-23)"
   /// were plain text boxes whose text, when it was no number, went to the server as
   /// -1; the refusal's notice was the first thing in a body that had been scrolled
   /// to the archive fields, so that a refused Save looked like a Save that did
   /// nothing; and a new List address renamed the list without a word.
   /// </summary>
   public class DistributionListChecksTests
   {
      [Fact]
      public void TheNumbers_AreCheckedOnTheirFields_InTheServersRanges()
      {
         string save = Member("private void Save()");
         Assert.Contains("DialogFields.ClearErrors(addressRow_, archiveDaysRow_, digestHourRow_);", save, StringComparison.Ordinal);
         Assert.Contains("if (!InlineFormChecks.TryWholeNumber(archiveDays_.Text, 0, 3650, out int archiveDays))", save, StringComparison.Ordinal);
         Assert.Contains("archiveDaysRow_.Error = L(\"Enter a number of days from 0 (for ever) to 3,650.\");", save, StringComparison.Ordinal);
         Assert.Contains("if (!InlineFormChecks.TryWholeNumber(digestHour_.Text, 0, 23, out int digestHour))", save, StringComparison.Ordinal);
         Assert.Contains("digestHourRow_.Error = L(\"Enter an hour from 0 to 23.\");", save, StringComparison.Ordinal);
         Assert.Contains("addressRow_.Error = L(\"Enter the list's address.\");", save, StringComparison.Ordinal);

         // Checked before anything is written, and written as checked.
         int checks = save.IndexOf("if (DialogFields.ShowFirstError(addressRow_, archiveDaysRow_, digestHourRow_))", StringComparison.Ordinal);
         int opens = save.IndexOf("OpenList(domains)", StringComparison.Ordinal);
         Assert.True(checks >= 0 && opens > checks, "The dialog writes before it checks.");
         Assert.Contains("l.ArchiveDays = archiveDays;", save, StringComparison.Ordinal);
         Assert.Contains("l.DigestHour = digestHour;", save, StringComparison.Ordinal);

         // No -1 for the server to refuse: the helper that made one is gone.
         Assert.DoesNotContain("WholeNumber(", Source().Replace("TryWholeNumber(", ""), StringComparison.Ordinal);
      }

      [Theory]
      [InlineData("0", 0, 3650, true)]
      [InlineData("3650", 0, 3650, true)]
      [InlineData("3651", 0, 3650, false)]
      [InlineData("a year", 0, 3650, false)]
      [InlineData("-1", 0, 3650, false)]
      [InlineData("23", 0, 23, true)]
      [InlineData("24", 0, 23, false)]
      [InlineData("", 0, 23, false)]
      public void TheRanges_AreTheServersOwn(string text, int min, int max, bool taken)
      {
         Assert.Equal(taken, InlineFormChecks.TryWholeNumber(text, min, max, out _));
      }

      [Fact]
      public void ARefusalOfTheServer_IsBroughtIntoView_AndARenameIsSaid()
      {
         string save = Member("private void Save()");
         int shown = save.IndexOf("notice_.Show(StatusLevel.Critical, F(\"Could not save the list: {0}\"", StringComparison.Ordinal);
         int inView = save.IndexOf("notice_.BringIntoView();", StringComparison.Ordinal);
         Assert.True(shown >= 0 && inView > shown, "A refusal of the server is not brought into view.");

         string page = Source();
         Assert.Contains(".WithHint(L(\"Changing it renames the list; its members and settings stay.\"));", page, StringComparison.Ordinal);
         Assert.Contains("KeepRoomForFieldErrors(3);", page, StringComparison.Ordinal);

         // A field refused is brought into view with its message, in every dialog.
         string fields = FormLineAssert.ReadControlPanelFile(Path.Join("Views", "DialogFields.cs"));
         Assert.Contains("row.BringIntoView();", fields, StringComparison.Ordinal);
      }

      private static string Source() => FormLineAssert.ReadControlPanelFile(Path.Join("Views", "DistributionListDialog.cs"));

      private static string Member(string signature)
      {
         string member = WithheldRowTests.MemberText(Source(), signature);
         Assert.True(member != null, "DistributionListDialog." + signature + " is gone.");
         return member;
      }
   }
}
