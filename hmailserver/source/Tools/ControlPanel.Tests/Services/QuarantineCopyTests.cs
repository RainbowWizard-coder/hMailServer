// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The quarantine's outcomes in one voice and counted (review finding F319):
   /// "Released 1 message to its recipients." beside "Allowed. List entries: 1.
   /// Messages released: 1."; the sweep's button named Remove expired under a
   /// setting called Delete quarantined messages after; and the digest's address
   /// captioned by a whole sentence with its rule in brackets.
   /// </summary>
   public class QuarantineCopyTests
   {
      private static string Page() => ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "QuarantineView.cs"));

      [Fact]
      public void TheListOutcomesAreSentencesCountedAsTheOthersAre()
      {
         string page = Page();
         Assert.Contains("\"allow\" => Sentences(Plural(entries, \"Added {0} allow entry.\", \"Added {0} allow entries.\"),", page, StringComparison.Ordinal);
         Assert.Contains("_ => Sentences(Plural(entries, \"Added {0} block entry.\", \"Added {0} block entries.\"),", page, StringComparison.Ordinal);
         Assert.DoesNotContain("List entries: {0}", page, StringComparison.Ordinal);

         // The second sentence of each is the release's or the delete's own outcome,
         // word for word.
         Assert.Equal(2, page.Split("Plural(done, \"Released {0} message to its recipients.\", \"Released {0} messages to their recipients.\")").Length - 1);
         Assert.Equal(2, page.Split("Plural(done, \"Deleted {0} message.\", \"Deleted {0} messages.\")").Length - 1);
      }

      [Fact]
      public void TheSweepIsNamedByWhatItDoes()
      {
         string page = Page();
         Assert.Contains("removeExpired_ = MakeButton(L(\"Delete _expired messages…\")", page, StringComparison.Ordinal);
         Assert.DoesNotContain("L(\"Remove _expired\")", page, StringComparison.Ordinal);
      }

      [Fact]
      public void TheDigestAddressIsACaptionAndAHint()
      {
         Assert.Contains("Prop = \"QuarantineDigestBaseUrl\", Label = L(\"Release link address\"), Hint = L(\"Such as https://mail.example.com. Leave empty to send no digest.\")",
            Page(), StringComparison.Ordinal);
      }
   }
}
