// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The Welcome page's sample domain is a card under the intents (review finding
   /// F275). It was a second Information notice above them, four lines that began
   /// "Nothing on this server to look at yet?" on a server of four domains and
   /// eleven accounts and repeated the password sentence its dialog shows; it
   /// arrived after the first paint and moved the intents about 80 pixels under
   /// the pointer, and at 760 pixels no intent was on screen. The walk's offer is
   /// now the one notice above the intents; the sample is a card titled A sample
   /// domain, drawn as the page is, with its button in the footer, not pressable
   /// until the read lands. Scanned rather than rendered: this assembly has no WPF.
   /// Each test fails against the page before the change.
   /// </summary>
   public class WelcomeSampleCardTests
   {
      [Fact]
      public void TheSample_IsACardUnderTheIntents_AndTheWalkTheOneNoticeAboveThem()
      {
         string source = ReadView();
         Assert.Contains("private readonly Card sample_ = new()", source, StringComparison.Ordinal);
         Assert.Contains("Title = L(\"A sample domain\")", source, StringComparison.Ordinal);

         int intents = source.IndexOf("Heading = L(\"What do you want to do?\")", StringComparison.Ordinal);
         int sample = source.IndexOf("panel.Children.Add(sample_);", StringComparison.Ordinal);
         int areas = source.IndexOf("Heading = L(\"Or browse by area\")", StringComparison.Ordinal);
         Assert.True(intents > 0 && sample > intents && areas > sample,
            "The sample domain's card is not between the intents and the areas.");

         // Above the intents, the walk's notice alone.
         string above = source.Substring(0, intents);
         int constructor = above.IndexOf("public WelcomeView()", StringComparison.Ordinal);
         Assert.True(constructor > 0, "WelcomeView's constructor is gone.");
         string built = above.Substring(constructor);
         Assert.Contains("panel.Children.Add(tourOffer_);", built, StringComparison.Ordinal);
         Assert.DoesNotContain("panel.Children.Add(sample_", built, StringComparison.Ordinal);
      }

      [Fact]
      public void TheCard_IsDrawnAsThePageIs_WithItsButtonInTheFooter_UntilTheReadLands()
      {
         string source = ReadView();
         string declaration = WithheldRowTests.MemberText(source, "private readonly Wpf.Ui.Controls.Button sampleAction_ =");
         Assert.True(declaration != null, "The sample's button is gone.");
         Assert.Contains("IsEnabled = false", declaration, StringComparison.Ordinal);
         Assert.Contains("sample_.Footer = sampleAction_;", source, StringComparison.Ordinal);

         // The card is not collapsed before the read: only a read that failed hides it.
         string card = WithheldRowTests.MemberText(source, "private readonly Card sample_ =");
         Assert.DoesNotContain("Visibility.Collapsed", card, StringComparison.Ordinal);
      }

      [Fact]
      public void TheCard_DoesNotAssumeAnEmptyServer_NorRepeatTheDialogsPasswordSentence()
      {
         string source = ReadView();
         Assert.DoesNotContain("F(\"Nothing on this server to look at yet?", source, StringComparison.Ordinal);
         Assert.DoesNotContain("which this server mints now and shows once", source, StringComparison.Ordinal);
         // The password is the creation dialog's: it is still said there.
         Assert.Contains("which is shown here once - write it down", source, StringComparison.Ordinal);
      }

      [Fact]
      public void WhatTheReadFound_IsANoticeInTheCard_AtItsLevel()
      {
         string source = ReadView();
         string show = WithheldRowTests.MemberText(source, "private void ShowSample_(string json)");
         Assert.True(show != null, "WelcomeView.ShowSample_ is gone.");
         Assert.Contains("sampleNotice_.Show(StatusLevel.Good, F(\"{0} is here with the mailboxes", show, StringComparison.Ordinal);
         Assert.Contains("sampleNotice_.Show(StatusLevel.Warning, F(\"A domain named {0} is already on this server", show, StringComparison.Ordinal);
         Assert.Contains("sampleNotice_.Show(StatusLevel.Warning, L(\"This server could not read its domain table", show, StringComparison.Ordinal);
         // A domain of that name that is not the sample: the page that deletes it.
         Assert.Contains("NoticeActions.OpenPage(\"domains\"", show, StringComparison.Ordinal);
      }

      private static string ReadView()
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", "Views", "WelcomeView.cs");
            if (File.Exists(candidate))
               return File.ReadAllText(candidate);
            directory = directory.Parent;
         }

         Assert.Fail("ControlPanel/Views/WelcomeView.cs could not be found from the test binaries.");
         return null;
      }
   }
}
