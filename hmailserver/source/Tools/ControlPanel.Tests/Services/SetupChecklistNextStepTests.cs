// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The Setup checklist leads to the step it marks (review finding F016). On a
   /// server that had done the first four items the tally said "Next step:
   /// Passwords only over TLS…" with nothing to press, and the step was under four
   /// full-size Done cards - each its description and its Open button - that a
   /// returning administrator scrolled past on every visit. The tally now carries
   /// Go to the next step, which brings the marked card on screen with the keyboard
   /// on its Open button, and a finished item - Done or Not needed - is one line,
   /// its number, its pill, its title and its Open button, open to the rest on a
   /// click. Scanned rather than rendered, as the other page tests are: this
   /// assembly has no WPF. The first three tests fail against the page before the
   /// change; the fourth holds what the change must not fold.
   /// </summary>
   public class SetupChecklistNextStepTests
   {
      [Fact]
      public void TheTally_CarriesGoToTheNextStep_WhileThereIsOne()
      {
         string source = ReadView();
         Assert.Contains("L(\"_Go to the next step\")", source, StringComparison.Ordinal);
         Assert.Contains("summary_.Content = next != null ? goToNext_ : null;", source, StringComparison.Ordinal);
         Assert.Contains("goToNext_.Click += (s, e) => GoToNextStep_();", source, StringComparison.Ordinal);
      }

      [Fact]
      public void GoToTheNextStep_BringsTheCardOnScreen_AndPutsTheKeyboardOnItsOpenButton()
      {
         string source = ReadView();
         string go = WithheldRowTests.MemberText(source, "private void GoToNextStep_()");
         Assert.True(go != null, "ExternalSetupView.GoToNextStep_ is gone.");
         Assert.Contains("nextCard_.BringIntoView(", go, StringComparison.Ordinal);
         Assert.Contains("Keyboard.Focus(link)", go, StringComparison.Ordinal);

         // The card and its button are the next step's, from the drawing that marked it.
         string render = WithheldRowTests.MemberText(source, "private void RenderItems(string dnsNote)");
         Assert.Contains("nextCard_ = card;", render, StringComparison.Ordinal);
         Assert.Contains("nextLink_ = link;", render, StringComparison.Ordinal);
      }

      [Fact]
      public void AFinishedItem_IsOneLine_AndOpensOnAClick()
      {
         string source = ReadView();
         Assert.Contains("bool compact = item.State == SetupItemState.Done || item.State == SetupItemState.NotNeeded;", source, StringComparison.Ordinal);
         string folded = source.Replace("\r\n", "\n", StringComparison.Ordinal);
         Assert.Contains("if (compact)\n            FoldDetails_(content, header, title, item, number);", folded, StringComparison.Ordinal);

         string fold = WithheldRowTests.MemberText(source, "private void FoldDetails_(");
         Assert.True(fold != null, "ExternalSetupView.FoldDetails_ is gone.");
         // Everything under the header goes into the panel a click shows; the
         // header - with the Open button - stays.
         Assert.Contains("while (content.Children.Count > 1)", fold, StringComparison.Ordinal);
         Assert.Contains("details.Visibility = open ? Visibility.Visible : Visibility.Collapsed;", fold, StringComparison.Ordinal);
         // The chevron is a button the keyboard reaches, named for its item; the
         // title opens it as well.
         Assert.Contains("F(\"Show the details of {0}\", item.Title)", fold, StringComparison.Ordinal);
         Assert.Contains("F(\"Hide the details of {0}\", item.Title)", fold, StringComparison.Ordinal);
         Assert.Contains("title.MouseLeftButtonUp += (s, e) => Toggle();", fold, StringComparison.Ordinal);
         // A card opened stays open as the page draws it again after the DNS pass.
         Assert.Contains("Show(opened_.Contains(item.Title));", fold, StringComparison.Ordinal);
      }

      [Fact]
      public void ActionNeededAndCannotTell_KeepTheirWholeText()
      {
         string source = ReadView();
         Assert.DoesNotContain("SetupItemState.ActionNeeded || item.State == SetupItemState.CannotTell", source, StringComparison.Ordinal);
         Assert.DoesNotContain("compact = true", source, StringComparison.Ordinal);
      }

      private static string ReadView()
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", "Views", "ExternalSetupView.cs");
            if (File.Exists(candidate))
               return File.ReadAllText(candidate);
            directory = directory.Parent;
         }

         Assert.Fail("ControlPanel/Views/ExternalSetupView.cs could not be found from the test binaries.");
         return null;
      }
   }
}
