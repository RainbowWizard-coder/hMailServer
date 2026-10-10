// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Linq;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The "no information conveyed by colour alone" half of the accessibility work.
   ///
   /// Why these fail against the unfixed Control Panel: the dashboard's only piece
   /// of judgement - "this queue is not draining" - was expressed by setting the
   /// queue counter's foreground to the warning brush and nothing else. There was no
   /// shape, no word, and nothing in the accessible name, so the entire message was
   /// the colour. Neither the threshold nor the mapping from severity to appearance
   /// existed anywhere a test could reach: the comparison was a bare "&gt; 100"
   /// inside a refresh method and the colour was a resource key beside it.
   /// </summary>
   public class StatusSemanticsTests
   {
      [Fact]
      public void EveryLevelCarriesAShapeAWordAndABrush()
      {
         foreach (StatusLevel level in StatusSemantics.AllLevels)
         {
            StatusPresentation status = StatusSemantics.For(level);

            Assert.Equal(level, status.Level);
            Assert.False(string.IsNullOrWhiteSpace(status.SeverityWord), level + " has no severity word.");
            Assert.False(string.IsNullOrWhiteSpace(status.BrushKey), level + " has no brush key.");
            Assert.True(Enum.IsDefined(status.Shape), level + " has no shape.");
         }
      }

      [Fact]
      public void NoTwoLevelsShareAShapeOrAWord()
      {
         // This is the whole point of the shape: if two levels drew the same one,
         // the colour would be back to being the only thing that told them apart.
         Assert.Equal(StatusSemantics.AllLevels.Count,
            StatusSemantics.AllLevels.Select(l => StatusSemantics.For(l).Shape).Distinct().Count());

         Assert.Equal(StatusSemantics.AllLevels.Count,
            StatusSemantics.AllLevels.Select(l => StatusSemantics.For(l).SeverityWord).Distinct().Count());
      }

      [Fact]
      public void EveryLevelIsCoveredByAllLevels()
      {
         // Adding a level without describing it would leave it silently rendering as
         // Normal, which is the failure mode this whole class exists to prevent.
         foreach (StatusLevel level in Enum.GetValues<StatusLevel>())
            Assert.Contains(level, StatusSemantics.AllLevels);
      }

      /// <summary>
      /// Normal is the level that says nothing, and its colour is the neutral grey,
      /// which no other level takes: a mark at Normal - the certificate details' "No
      /// passphrase needed" - was the primary text, a black dot in the light theme and
      /// a white one in the dark (review finding F326). A mark beside its word takes the
      /// level's fill-text shade at every other level, as the word does, and the grey at
      /// Normal, whose fill-text shade is made for the pill's grey box.
      /// </summary>
      [Fact]
      public void NormalIsTheNeutralGrey_AndItsMarkIsToo()
      {
         StatusPresentation normal = StatusSemantics.For(StatusLevel.Normal);

         Assert.Equal("AppNeutralBrush", normal.BrushKey);
         Assert.Equal("AppNeutralBrush", normal.MarkBrushKey);
         Assert.DoesNotContain("TextFillColorPrimaryBrush", StatusSemantics.AllLevels.Select(l => StatusSemantics.For(l).BrushKey));
         Assert.DoesNotContain("AppNeutralBrush",
            StatusSemantics.AllLevels
               .Where(l => l != StatusLevel.Normal)
               .Select(l => StatusSemantics.For(l).BrushKey));

         foreach (StatusLevel level in StatusSemantics.AllLevels.Where(l => l != StatusLevel.Normal))
            Assert.Equal(StatusSemantics.For(level).FillTextBrushKey, StatusSemantics.For(level).MarkBrushKey);
      }

      /// <summary>
      /// The dashboard's queue length keeps the ordinary text colour while the queue
      /// is normal, as every other figure there does: the neutral grey is a mark's.
      /// </summary>
      [Fact]
      public void TheDashboardsQueueLength_IsTheOrdinaryTextWhileTheQueueIsNormal()
      {
         string code = SaveStateModelTests.ReadControlPanelFile("Views/DashboardView.xaml.cs");
         Assert.Contains("KpiQueue.SetResourceReference(TextBlock.ForegroundProperty, level == StatusLevel.Normal ? \"TextFillColorPrimaryBrush\" : status.BrushKey);", code, StringComparison.Ordinal);
      }

      /// <summary>The certificate details' and the notice's marks take the mark's key, not the fill-text one.</summary>
      [Fact]
      public void TheCertificateDetailsAndTheNoticeDrawTheirMarksInTheMarksColour()
      {
         string certificates = SaveStateModelTests.ReadControlPanelFile("Views/SslCertificatesView.xaml.cs");
         Assert.Equal(2, certificates.Split("ShapeMarkVisuals.ApplyMark(mark, presentation.Shape, presentation.MarkBrushKey);").Length - 1);
         Assert.DoesNotContain("ApplyMark(mark, presentation.Shape, presentation.FillTextBrushKey)", certificates, StringComparison.Ordinal);

         string notice = SaveStateModelTests.ReadControlPanelFile("Views/Scaffold/InlineNotice.cs");
         Assert.Contains("ShapeMarkVisuals.ApplyMark(mark, status.Shape, status.MarkBrushKey);", notice, StringComparison.Ordinal);
      }

      [Fact]
      public void AnUndefinedLevelFallsBackToNormalRatherThanThrowing()
      {
         StatusPresentation status = StatusSemantics.For((StatusLevel) 99);

         Assert.Equal(StatusLevel.Normal, status.Level);
      }

      [Theory]
      [InlineData(0, StatusLevel.Normal)]
      [InlineData(1, StatusLevel.Normal)]
      [InlineData(100, StatusLevel.Normal)]
      [InlineData(101, StatusLevel.Warning)]
      [InlineData(10000, StatusLevel.Warning)]
      public void TheQueueBacklogThresholdIsExclusive(int queueLength, StatusLevel expected)
      {
         // Exactly the behaviour the dashboard has always had - a queue sitting on
         // a hundred messages is busy, not broken - now in one place where it can be
         // read and changed.
         Assert.Equal(expected, StatusSemantics.ForQueueLength(queueLength));
      }

      [Fact]
      public void ANegativeQueueLengthIsNotABacklog()
      {
         // The queue length is derived from splitting a string the server returns,
         // so it cannot go negative today; if it ever does, the answer is not to
         // report a backlog.
         Assert.Equal(StatusLevel.Normal, StatusSemantics.ForQueueLength(-1));
      }

      [Fact]
      public void TheThresholdCanBeRaisedForABusierServer()
      {
         Assert.Equal(StatusLevel.Normal, StatusSemantics.ForQueueLength(500, 1000));
         Assert.Equal(StatusLevel.Warning, StatusSemantics.ForQueueLength(1001, 1000));
      }
   }
}
