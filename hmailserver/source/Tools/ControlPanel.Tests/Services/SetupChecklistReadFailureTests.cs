// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// Review finding F040: with the link lost, the setup checklist drew "4 need
   /// action, 7 cannot be told from here, 0 done, 4 not needed. Next step: MTA-STS
   /// policy hosting..." first, a tally counted from values that could not be read,
   /// and the notice that said so came after it and spoke of "the items above" while
   /// it sat above them. The notice is first, says the cards below may be incomplete
   /// with its reason last, and the tally says it counts what could be read; and
   /// while reads fail neither the tally nor a card names a next step.
   ///
   /// Scanned rather than rendered: this assembly has no WPF.
   /// </summary>
   public class SetupChecklistReadFailureTests
   {
      [Fact]
      public void TheFailure_IsAboveTheTally()
      {
         string code = Source();
         int failure = code.IndexOf("page.Children.Add(readFailure_);", StringComparison.Ordinal);
         int tally = code.IndexOf("page.Children.Add(summary_);", StringComparison.Ordinal);
         int items = code.IndexOf("page.Children.Add(body_);", StringComparison.Ordinal);
         Assert.True(failure >= 0 && tally >= 0 && items >= 0, "The checklist no longer adds its notices and items to the page.");
         Assert.True(failure < tally && tally < items, "The read failure is not above the tally, or the tally not above the items.");
      }

      [Fact]
      public void TheFailure_SaysTheCardsBelow_WithItsReasonLast()
      {
         string code = Source();
         Assert.DoesNotContain("above may be incomplete", code, StringComparison.Ordinal);
         Assert.Contains("F(\"Some values could not be read, so the cards below may be incomplete. Why: {0}\", checks_.FirstError)", code, StringComparison.Ordinal);
         Assert.Contains("F(\"Some values could not be read, so the cards below may be incomplete. The first reason: {0}\", checks_.FirstError)", code, StringComparison.Ordinal);
      }

      [Fact]
      public void WhileReadsFail_TheTallyCountsWhatCouldBeRead_AndNamesNoNextStep()
      {
         string render = WithheldRowTests.MemberText(Source(), "private void RenderItems(string dnsNote)");
         Assert.True(render != null, "ExternalSetupView.RenderItems is gone.");

         // A sentence of its own before the tally while values could not be read, and
         // the next step only while every value was.
         Assert.Contains("summary_.Text = Sentences(partial ? L(\"Only what could be read is counted.\") : null,", render, StringComparison.Ordinal);
         Assert.Contains("next != null ? F(\"Next step: {0}.\", next.Title) : null);", render, StringComparison.Ordinal);
      }

      [Fact]
      public void WhileReadsFail_NoCardIsTheNextStep()
      {
         // The tally named no next step while values could not be read, and the first
         // card that needed action kept its Next step pill, the page's primary button
         // and "Next step." in its name: the step is withheld where it is chosen, before
         // the cards are drawn, so that neither can name it.
         string render = WithheldRowTests.MemberText(Source(), "private void RenderItems(string dnsNote)");
         Assert.True(render != null, "ExternalSetupView.RenderItems is gone.");

         int partial = render.IndexOf("bool partial = checks_.FailedReads > 0;", StringComparison.Ordinal);
         int next = render.IndexOf("SetupItem next = partial ? null : checks_.Items.FirstOrDefault(i => i.State == SetupItemState.ActionNeeded);", StringComparison.Ordinal);
         int cards = render.IndexOf("body_.Children.Add(ItemCard(item, ++number, ReferenceEquals(item, next)));", StringComparison.Ordinal);
         Assert.True(partial >= 0, "RenderItems no longer asks whether values could not be read.");
         Assert.True(next > partial, "The next step is chosen without asking whether values could not be read, or before it is asked.");
         Assert.True(cards > next, "The cards are drawn before the next step is chosen, or no longer told which card it is.");
         Assert.Equal(1, CountOf(render, "SetupItem next ="));
         Assert.DoesNotContain("!partial", render, StringComparison.Ordinal);
      }

      private static int CountOf(string text, string part)
      {
         int count = 0;
         for (int at = text.IndexOf(part, StringComparison.Ordinal); at >= 0; at = text.IndexOf(part, at + part.Length, StringComparison.Ordinal))
            count++;
         return count;
      }

      private static string Source()
      {
         for (var directory = new DirectoryInfo(AppContext.BaseDirectory); directory != null; directory = directory.Parent)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", "Views", "ExternalSetupView.cs");
            if (File.Exists(candidate))
               return File.ReadAllText(candidate).Replace("\r\n", "\n");
         }

         Assert.Fail("ControlPanel/Views/ExternalSetupView.cs could not be found from the test binaries.");
         return null;
      }
   }
}
