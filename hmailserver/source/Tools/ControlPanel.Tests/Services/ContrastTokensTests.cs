// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The colours that sit on the dark theme's cards and rows, measured on the
   /// surfaces they are drawn on rather than on the #1B1B1B the status palette
   /// was chosen against. The dark card is #2D2D2D and a hovered or selected
   /// row on it #393939 or the selection tint; there the danger and
   /// information colours measured 4.1:1 as words and 3.5:1 on a row, and the
   /// live log's error, IMAP and application lines 4.1 to 4.3:1. These hold
   /// every pair to its floor - 4.5:1 for text - and the sources to the shades
   /// that clear it, so that a status word set in a status colour again fails
   /// here.
   /// </summary>
   public class ContrastTokensTests
   {
      private const uint LightPage = 0xFFFAFAFA;
      private const uint DarkPage = 0xFF202020;

      /// <summary>The surfaces text is drawn on in a theme: the page, a card, a dialog, a hovered row and a selected row.</summary>
      private static IEnumerable<(string Name, uint Colour)> Surfaces(ChartTheme theme)
      {
         bool light = theme == ChartTheme.Light;
         uint page = light ? LightPage : DarkPage;
         uint card = Over(DesignTokens.Surfaces(theme, ChartSystemColors.Fallback).CardBackground, page);

         yield return ("the page", page);
         yield return ("a card", card);
         yield return ("a dialog", light ? 0xFFF3F3F3u : 0xFF202020u);
         yield return ("a dialog's darkest", light ? 0xFFEEEEEEu : 0xFF1C1C1Cu);
         // A hovered row: the theme's subtle fill (SubtleFillColorSecondary) over the card.
         yield return ("a hovered row", Over(light ? 0x09000000u : 0x0FFFFFFFu, card));
         yield return ("a selected row", Over(DesignTokens.Selection(theme, ChartSystemColors.Fallback).Fill, card));
      }

      public static TheoryData<ChartTheme> LightAndDark => new() { ChartTheme.Light, ChartTheme.Dark };

      // ---- Status words on surfaces (F181) ---------------------------------------

      /// <summary>
      /// A status word or sentence on a surface - a certificate's verdict, a
      /// scanner's problem, a field's error, an action's result - is drawn in its
      /// level's fill-text shade, and every shade clears 4.5:1 on every surface
      /// its theme draws text on, a hovered and a selected row included.
      /// </summary>
      [Theory]
      [MemberData(nameof(LightAndDark))]
      public void EveryFillTextShadeClearsFourPointFiveToOneOnEverySurface(ChartTheme theme)
      {
         foreach (StatusLevel level in StatusSemantics.AllLevels)
         {
            uint word = DesignTokens.StatusFill(level, theme, ChartSystemColors.Fallback).Text;

            foreach ((string name, uint surface) in Surfaces(theme))
            {
               double ratio = ChartPalette.ContrastRatio(word, surface);
               Assert.True(ratio >= 4.5, $"{theme} {level}: the word is {ratio:F2}:1 on {name} ({surface:X8}), under 4.5:1.");
            }
         }
      }

      /// <summary>
      /// Why the words do not take the status colours: in the dark theme the
      /// danger and information colours are under 4.5:1 on a card. If the
      /// palette ever clears it everywhere this test says so, and the rule can
      /// be revisited; until then a word in a status colour is a defect.
      /// </summary>
      [Fact]
      public void TheDarkStatusColoursThemselvesAreUnderFourPointFiveToOneOnACard()
      {
         DesignTokens.StatusArgb status = DesignTokens.Status(ChartTheme.Dark, ChartSystemColors.Fallback);
         uint card = Surfaces(ChartTheme.Dark).Single(s => s.Name == "a card").Colour;

         Assert.True(ChartPalette.ContrastRatio(status.Danger, card) < 4.5);
         Assert.True(ChartPalette.ContrastRatio(status.Info, card) < 4.5);
      }

      /// <summary>
      /// The live log's lines clear 4.5:1 on the card the log sits on and on a
      /// line under the pointer, in both themes.
      /// </summary>
      [Theory]
      [MemberData(nameof(LightAndDark))]
      public void EveryLogColourClearsFourPointFiveToOneOnItsCardAndOnAHoveredLine(ChartTheme theme)
      {
         DesignTokens.LogArgb log = DesignTokens.Log(theme, ChartSystemColors.Fallback);
         var lines = new[]
         {
            ("other", log.Other), ("SMTP", log.Smtp), ("IMAP", log.Imap), ("POP3", log.Pop3),
            ("application", log.Application), ("error", log.Error)
         };

         foreach ((string kind, uint colour) in lines)
         {
            Assert.Equal(0xFFu, colour >> 24);

            foreach ((string name, uint surface) in Surfaces(theme).Where(s => s.Name is "the page" or "a card" or "a hovered row"))
            {
               double ratio = ChartPalette.ContrastRatio(colour, surface);
               Assert.True(ratio >= 4.5, $"{theme}: the log's {kind} line is {ratio:F2}:1 on {name} ({surface:X8}), under 4.5:1.");
            }
         }
      }

      /// <summary>Under High Contrast the log's colours are the system's and none is invented.</summary>
      [Fact]
      public void UnderHighContrastTheLogTakesTheSystemPalette()
      {
         var system = new ChartSystemColors(0xFF112233, 0xFFEEDDCC, 0xFF445566, 0xFF778899, 0xFFAABBCC);
         DesignTokens.LogArgb log = DesignTokens.Log(ChartTheme.HighContrast, system);

         Assert.Equal(system.GrayText, log.Other);
         Assert.Equal(system.WindowText, log.Smtp);
         Assert.Equal(system.WindowText, log.Imap);
         Assert.Equal(system.WindowText, log.Pop3);
         Assert.Equal(system.Highlight, log.Application);
         Assert.Equal(system.HotTrack, log.Error);
      }

      /// <summary>
      /// ThemeTokens takes the log's colours from DesignTokens, where they are
      /// measured, and keeps no palette of its own.
      /// </summary>
      [Fact]
      public void ThemeTokensTakesTheLogPaletteFromDesignTokens()
      {
         string source = ReadControlPanelFile(Path.Join("Services", "ThemeTokens.cs"));
         if (source == null)
            return;

         Assert.Contains("DesignTokens.Log(CurrentChartTheme, CurrentSystemColors)", source, StringComparison.Ordinal);
         Assert.DoesNotMatch(new Regex("Hex\\(\"#"), source);
      }

      /// <summary>
      /// No view sets a text's colour to a status colour: a word on a surface
      /// takes its level's fill-text shade (<c>FillTextBrushKey</c>,
      /// <c>App*FillTextBrush</c>), and the status colour (<c>BrushKey</c>,
      /// <c>App*Brush</c>) is left to marks - the shapes, glyphs and icons whose
      /// floor is 3:1 - and to an InlineNotice's tint and outline. The one text
      /// allowed it is the dashboard's queue length, a 28-pixel number, which is
      /// large text with a floor of 3:1.
      /// </summary>
      [Fact]
      public void NoViewDrawsAWordInAStatusColour()
      {
         string controlPanel = FindControlPanelDirectory();
         if (controlPanel == null)
            return;

         var textForeground = new Regex(
            @"(?<target>\w+)\.SetResourceReference\(\s*(?:[\w.]*\.)?(?:TextBlock|TextElement|Run|Control)\.ForegroundProperty\s*,\s*(?<key>[^;]*?)\);",
            RegexOptions.Singleline);
         var statusKey = new Regex(@"""App(?:Success|Warning|Danger|Info|Neutral)Brush""|\bBrushKey\b");
         var xamlForeground = new Regex(@"Foreground=""\{DynamicResource App(?:Success|Warning|Danger|Info|Neutral)Brush\}""");

         var offences = new List<string>();
         foreach (string file in Directory.GetFiles(Path.Join(controlPanel, "Views"), "*.*", SearchOption.AllDirectories))
         {
            string name = Path.GetRelativePath(controlPanel, file);
            string text = File.ReadAllText(file);

            if (file.EndsWith(".cs", StringComparison.Ordinal))
            {
               foreach (Match match in textForeground.Matches(text))
               {
                  if (!statusKey.IsMatch(match.Groups["key"].Value))
                     continue;
                  if (name.EndsWith("DashboardView.xaml.cs", StringComparison.Ordinal) && match.Groups["target"].Value == "KpiQueue")
                     continue;
                  offences.Add(name + ": " + match.Value);
               }

               // A status colour's key taken anywhere else - into a variable a
               // text's colour is then set from, as the password strength line
               // once did - is a mark's (ApplyMark), a notice's tint and
               // outline (InlineNotice) or the queue length's.
               foreach (string line in text.Split('\n'))
               {
                  if (!Regex.IsMatch(line, @"\.BrushKey\b") || line.Contains("ApplyMark(", StringComparison.Ordinal)
                      || line.Contains("KpiQueue.SetResourceReference", StringComparison.Ordinal)
                      || name.EndsWith("InlineNotice.cs", StringComparison.Ordinal)
                      || line.TrimStart().StartsWith("//", StringComparison.Ordinal))
                     continue;
                  offences.Add(name + ": " + line.Trim());
               }
            }
            else if (file.EndsWith(".xaml", StringComparison.Ordinal))
            {
               foreach (Match match in xamlForeground.Matches(text))
                  offences.Add(name + ": " + match.Value);
            }
         }

         Assert.True(offences.Count == 0, "A word is drawn in a status colour, which is under 4.5:1 on the dark card; use the level's fill-text shade:\n" + string.Join("\n", offences));
      }

      /// <summary>The pages the review measured, each drawing its words in the fill-text shade.</summary>
      [Fact]
      public void TheMeasuredStatusWordsTakeTheFillTextShade()
      {
         string certificates = ReadControlPanelFile(Path.Join("Views", "SslCertificatesView.xaml.cs"));
         string virus = ReadControlPanelFile(Path.Join("Views", "VirusOverviewView.cs"));
         string scaffold = ReadControlPanelFile(Path.Join("Views", "Scaffold", "Scaffold.xaml"));
         string fieldRow = ReadControlPanelFile(Path.Join("Views", "Scaffold", "FieldRow.cs"));
         if (certificates == null || virus == null || scaffold == null || fieldRow == null)
            return;

         // The certificate's badge and its detail line: the word and its mark.
         Assert.Equal(4, Regex.Matches(certificates, @"presentation\.FillTextBrushKey\)").Count);
         Assert.DoesNotContain("presentation.BrushKey", certificates, StringComparison.Ordinal);

         // A scanner's problem.
         Assert.Contains("StatusSemantics.For(StatusLevel.Critical).FillTextBrushKey", virus, StringComparison.Ordinal);

         // A field's error and its mark.
         Assert.Contains("Foreground=\"{DynamicResource AppDangerFillTextBrush}\"", scaffold, StringComparison.Ordinal);
         Assert.Contains("critical.FillTextBrushKey", fieldRow, StringComparison.Ordinal);
      }

      // ---- helpers ---------------------------------------------------------------

      /// <summary>A colour laid over a surface at its own alpha, as the screen composites it.</summary>
      internal static uint Over(uint colour, uint surface)
      {
         double opacity = (colour >> 24) / 255.0;

         uint Channel(int shift)
         {
            double top = (colour >> shift) & 0xFF;
            double bottom = (surface >> shift) & 0xFF;
            return (uint)Math.Round(top * opacity + bottom * (1 - opacity));
         }

         return 0xFF000000u | (Channel(16) << 16) | (Channel(8) << 8) | Channel(0);
      }

      internal static string ReadControlPanelFile(string relativePath)
      {
         string controlPanel = FindControlPanelDirectory();
         if (controlPanel == null)
            return null;

         string path = Path.Join(controlPanel, relativePath);
         return File.Exists(path) ? File.ReadAllText(path) : null;
      }

      /// <summary>Walks up from the test binaries to the Control Panel sources.</summary>
      internal static string FindControlPanelDirectory()
      {
         for (var directory = new DirectoryInfo(AppContext.BaseDirectory); directory != null; directory = directory.Parent)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", "MainWindow.xaml");
            if (File.Exists(candidate))
               return Path.Join(directory.FullName, "ControlPanel");
         }

         return null;
      }
   }
}
