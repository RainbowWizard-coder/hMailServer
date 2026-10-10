// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// Review finding F044: by design the tour never takes the keyboard - F6 moves into
   /// its card, Escape ends the tour from inside it (MainWindow.HandleTourKey_) - and
   /// nothing on the card, in its accessible name or in its first announcement said
   /// so. The card's help text says it at every step; its first step shows it as a
   /// tertiary line under the buttons and adds it to the announcement.
   ///
   /// Scanned rather than rendered: this assembly has no WPF.
   /// </summary>
   public class TourKeysHintTests
   {
      private const string Keys = "Press F6 to reach the tour's buttons; while one of them has the keyboard, Escape ends the tour.";

      [Fact]
      public void TheCard_SaysHowTheKeyboardReachesIt()
      {
         string code = Source();
         Assert.Contains("public static string KeysSentence => L(\"" + Keys + "\");", code, StringComparison.Ordinal);
         Assert.Contains("AutomationProperties.SetHelpText(card_, KeysSentence);", code, StringComparison.Ordinal);
         Assert.Contains("keys_.SetResourceReference(StyleProperty, \"TextCaptionTertiary\");", code, StringComparison.Ordinal);
         Assert.Contains("body.Children.Add(keys_);", code, StringComparison.Ordinal);
      }

      [Fact]
      public void OnlyTheFirstStepOfARun_ShowsAndAnnouncesIt()
      {
         string show = WithheldRowTests.MemberText(Source(), "public void ShowStep(FrameworkElement target, string tourName, string position, string sentence,");
         Assert.True(show != null, "TourOverlay.ShowStep is gone.");

         // The card was not showing before the first step of a run was first drawn,
         // and is between steps; the step keeps the line when the shell draws it again.
         int opened = show.IndexOf("if (!Showing)\n            firstPosition_ = position;", StringComparison.Ordinal);
         int first = show.IndexOf("bool first = string.Equals(position, firstPosition_, StringComparison.Ordinal);", StringComparison.Ordinal);
         int shown = show.IndexOf("Visibility = Visibility.Visible;", StringComparison.Ordinal);
         Assert.True(opened >= 0 && first > opened && shown > first, "ShowStep does not tell the first step of a run before it shows the card.");
         string hide = WithheldRowTests.MemberText(Source(), "public void Hide()");
         Assert.True(hide != null, "TourOverlay.Hide is gone.");
         Assert.Contains("firstPosition_ = null;", hide, StringComparison.Ordinal);
         Assert.Contains("keys_.Visibility = first ? Visibility.Visible : Visibility.Collapsed;", show, StringComparison.Ordinal);
         Assert.Contains("Announce(first ? Sentences(sentence, KeysSentence) : sentence);", show, StringComparison.Ordinal);
      }

      /// <summary>The sentence says what the shell does: F6 into the card, Escape only from inside it.</summary>
      [Fact]
      public void TheSentence_IsWhatTheShellDoes()
      {
         string window = null;
         for (var directory = new DirectoryInfo(AppContext.BaseDirectory); directory != null && window == null; directory = directory.Parent)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", "MainWindow.Tour.cs");
            if (File.Exists(candidate))
               window = File.ReadAllText(candidate);
         }

         Assert.True(window != null, "MainWindow.Tour.cs could not be found from the test binaries.");
         string keys = WithheldRowTests.MemberText(window, "private bool HandleTourKey_(KeyEventArgs e)");
         Assert.True(keys != null, "MainWindow.HandleTourKey_ is gone.");
         Assert.Contains("e.Key == Key.F6 && Keyboard.Modifiers == ModifierKeys.None && !TourLayer.HasFocusWithin", keys, StringComparison.Ordinal);
         Assert.Contains("e.Key == Key.Escape && TourLayer.HasFocusWithin", keys, StringComparison.Ordinal);
      }

      private static string Source()
      {
         for (var directory = new DirectoryInfo(AppContext.BaseDirectory); directory != null; directory = directory.Parent)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", "Views", "Scaffold", "TourOverlay.cs");
            if (File.Exists(candidate))
               return File.ReadAllText(candidate).Replace("\r\n", "\n");
         }

         Assert.Fail("ControlPanel/Views/Scaffold/TourOverlay.cs could not be found from the test binaries.");
         return null;
      }
   }
}
