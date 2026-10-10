// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The tour card is 340 pixels wide, and from its second step Back, Next and Skip
   /// the tour were a row that could not wrap: in German "Rundgang überspringen" ran
   /// off the card's right edge and was cut at "überspringer" (found rendering the
   /// second step for review finding F044). The row wraps, and each button keeps a
   /// small gap under it for the line it may wrap to.
   ///
   /// Scanned rather than rendered: this assembly has no WPF.
   /// </summary>
   public class TourCardButtonsTests
   {
      [Fact]
      public void TheButtons_WrapInsteadOfRunningOffTheCard()
      {
         string code = Source();
         Assert.Contains("var buttons = new WrapPanel { Orientation = Orientation.Horizontal };", code, StringComparison.Ordinal);
         Assert.DoesNotContain("var buttons = new StackPanel", code, StringComparison.Ordinal);

         foreach (string button in new[] { "back_", "next_", "finish_" })
            Assert.Contains(button + ".Margin = new Thickness(0, 0, DesignTokens.Space.Sm, DesignTokens.Space.Xs);", code, StringComparison.Ordinal);
         Assert.Contains("skip_.Margin = new Thickness(0, 0, 0, DesignTokens.Space.Xs);", code, StringComparison.Ordinal);
      }

      private static string Source()
      {
         for (var directory = new DirectoryInfo(AppContext.BaseDirectory); directory != null; directory = directory.Parent)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", "Views", "Scaffold", "TourOverlay.cs");
            if (File.Exists(candidate))
               return File.ReadAllText(candidate);
         }

         Assert.Fail("ControlPanel/Views/Scaffold/TourOverlay.cs could not be found from the test binaries.");
         return null;
      }
   }
}
