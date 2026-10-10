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
   /// Review finding F262: the quarantine's allow chose the sender's address
   /// whatever had held the messages, though the address's own note said that a
   /// message the server-wide spam score holds is still held - how most messages in
   /// the quarantine got there; and the plan, a failed preview, the messages that
   /// cannot be listed and the public-suffix warning were one block of plain text,
   /// the Primary disabled with no reason. Which messages the score held is
   /// QuarantineReview's, held here; the dialog's wiring is read from its source.
   /// </summary>
   public class QuarantineListDialogTests
   {
      [Fact]
      public void AHoldAtOrOverTheServersThreshold_WasTheServerWideScores_WhateverItsHelo()
      {
         // The hold during the SMTP conversation records the score that reached the
         // threshold, with the HELO name or without it.
         Assert.True(QuarantineReview.HeldByServerScore(new HeldMessage { Helo = "mail.mass-mail.example", Score = 21, Reason = "SPF failed" }, 20));
         Assert.True(QuarantineReview.HeldByServerScore(new HeldMessage { Score = 25, Reason = "SPF failed" }, 20));
         Assert.True(QuarantineReview.HeldByServerScore(new HeldMessage { Score = 20 }, 20));
         Assert.False(QuarantineReview.HeldByServerScore(new HeldMessage { Score = 19 }, 20));
         Assert.False(QuarantineReview.HeldByServerScore(new HeldMessage { Score = 25 }, null));
         Assert.False(QuarantineReview.HeldByServerScore(new HeldMessage { Score = 25 }, 0));

         // A rule's hold and the sandbox's are not the score's, whatever they score.
         Assert.False(QuarantineReview.HeldByServerScore(new HeldMessage { Helo = "x.example", Score = 99, Reason = "Rule: Hold invoices" }, 20));
         Assert.False(QuarantineReview.HeldByServerScore(new HeldMessage { Score = 99, Reason = "Sandbox: malicious (2 files)" }, 20));
         Assert.False(QuarantineReview.HeldByServerScore(null, 20));
      }

      [Fact]
      public void EveryOtherHold_CarriesAHeloName_AndIsNotTheServerWideScores()
      {
         // Every hold records the HELO name, read from the Received header this server
         // wrote (QuarantineStore::HeloOfStoredCopy_): a block-list hold, scored 0,
         // whose domain allowed would leave the address's block entry and hold the
         // next message again (LocalDelivery::ApplySenderLists_).
         Assert.False(QuarantineReview.HeldByServerScore(new HeldMessage
         {
            Helo = "mail.example.net",
            Score = 0,
            Reason = "news@example.net is on the server block list as news@example.net."
         }, 20));

         // An impersonation hold, scored 0, whose domain may be gmail.com
         // (ImpersonationCheck).
         Assert.False(QuarantineReview.HeldByServerScore(new HeldMessage
         {
            Helo = "mail-ed1-f41.google.com",
            Score = 0,
            Reason = "name \"Chief Executive\" matches a member of staff (ceo@example.com); from someone@gmail.com"
         }, 20));

         // An account's own threshold holds at delivery, after the address list - which
         // stops it - below the server's threshold or over it.
         Assert.False(QuarantineReview.HeldByServerScore(new HeldMessage
         {
            Helo = "mail.example.org",
            Score = 12,
            Reason = "The score 12 reached the delete threshold 10 configured for user@example.com."
         }, 20));
         Assert.False(QuarantineReview.HeldByServerScore(new HeldMessage
         {
            Helo = "mail.example.org",
            Score = 30,
            Reason = "The score 30 reached the delete threshold 10 configured for user@example.com."
         }, 20));
      }

      [Fact]
      public void TheDialog_ChoosesTheListThatStopsTheNextHold_AndSaysItsFailuresAsNotices()
      {
         string code = ReadControlPanelFile(Path.Join("Views", "QuarantineListDialog.cs"));
         string page = ReadControlPanelFile(Path.Join("Views", "QuarantineView.cs"));

         // The page says whether the score held any of them, and the threshold.
         Assert.Contains("bool heldByScore = review_.MessagesWith(ids).Any(m => QuarantineReview.HeldByServerScore(m, threshold));", page, StringComparison.Ordinal);
         Assert.Contains("new QuarantineListDialog(Window.GetWindow(this), action == \"block\", ids, heloGrouping, Bulk_, heldByScore, threshold)", page, StringComparison.Ordinal);

         // An allow of messages the score held takes the domain list; otherwise the
         // address, with what it does not stop said under it.
         Assert.Contains("if (!block && heldByScore)\r\n            domain_.IsChecked = true;", code.Replace("\r\n", "\n").Replace("\n", "\r\n"), StringComparison.Ordinal);
         Assert.Contains("F(\"This will not stop the next message from this sender being held if it scores {0} or more.\", deleteThreshold.Value)", code, StringComparison.Ordinal);
         Assert.Contains("stillHeld_.Visibility = !block_ && address_.IsChecked == true ? Visibility.Visible : Visibility.Collapsed;", code, StringComparison.Ordinal);

         // A failed preview is Critical with Try again; the refused messages and a
         // public suffix are Warnings, the suffix's carrying the box that lists it.
         Assert.Contains("private readonly InlineNotice failure_ = new() { Level = StatusLevel.Critical", code, StringComparison.Ordinal);
         Assert.Contains("failure_.Content = tryAgain ? ReadFailure.TryAgainButton(RefreshPlan_) : null;", code, StringComparison.Ordinal);
         Assert.Contains("private readonly InlineNotice refused_ = new() { Level = StatusLevel.Warning", code, StringComparison.Ordinal);
         Assert.Contains("suffix_.Content = suffixOk_;", code, StringComparison.Ordinal);
         Assert.DoesNotContain("\"Warning: {0} is a public suffix", code, StringComparison.Ordinal);
         Assert.DoesNotContain("plan_.Text = ServerSession.DescribeComError(ex);", code, StringComparison.Ordinal);
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
