// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using System.Xml.Linq;
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
      /// The live log's lines clear 4.5:1 on the page, on the card the log sits
      /// on, on a line under the pointer and on a selected line, in both themes:
      /// the surfaces the application's list row draws, which every line of the
      /// log is (<see cref="TheLiveLogsLinesAreTheApplicationsListRow"/>). The
      /// light SMTP line, #1A7F37, measured 4.3:1 on the selection fill; until
      /// 9 October 2026 the log's lines took the system's row, whose blue
      /// selection put the dark lines at 3.8 to 3.9:1 and whose grey selection,
      /// once the list lost the keyboard, at 2.9 to 3.0:1, and this test, which
      /// measured a hovered line alone, passed.
      /// </summary>
      [Theory]
      [MemberData(nameof(LightAndDark))]
      public void EveryLogColourClearsFourPointFiveToOneOnItsCardAndOnAHoveredOrSelectedLine(ChartTheme theme)
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

            foreach ((string name, uint surface) in Surfaces(theme).Where(s => s.Name is "the page" or "a card" or "a hovered row" or "a selected row"))
            {
               double ratio = ChartPalette.ContrastRatio(colour, surface);
               Assert.True(ratio >= 4.5, $"{theme}: the log's {kind} line is {ratio:F2}:1 on {name} ({surface:X8}), under 4.5:1.");
            }
         }
      }

      /// <summary>
      /// The surfaces the log's colours are measured on are the ones its lines
      /// draw: the log's item style is based on App.xaml's implicit list row -
      /// the subtle grey under the pointer, the selection fill when selected,
      /// in and out of focus alike (SelectionLanguageTests holds that row) - and
      /// changes the row's inset, height floor and gap alone, painting no
      /// background and replacing no template. With a style of its own the log
      /// took the system's row, the blue #1F26A0DA under the pointer, #3D26A0DA
      /// when selected and #3DDADADA when selected out of focus, surfaces no
      /// test measured.
      /// </summary>
      [Fact]
      public void TheLiveLogsLinesAreTheApplicationsListRow()
      {
         string xaml = ReadControlPanelFile(Path.Join("Views", "LogsView.xaml"));
         if (xaml == null)
            return;

         XElement list = XDocument.Parse(xaml).Root.Descendants(Wpf + "ListBox").Single(e => (string)e.Attribute(X + "Name") == "LogList");
         XElement style = list.Element(Wpf + "ListBox.ItemContainerStyle")?.Element(Wpf + "Style");
         Assert.True(style != null, "The live log's list has no item style of its own; the dense line it needs is lost.");
         Assert.Equal("{StaticResource {x:Type ListBoxItem}}", (string)style.Attribute("BasedOn"));

         string[] setters = style.Descendants(Wpf + "Setter").Select(s => (string)s.Attribute("Property")).ToArray();
         Assert.All(setters, property => Assert.Contains(property, new[] { "Padding", "MinHeight", "Margin", "BorderThickness" }));
         Assert.Empty(style.Elements(Wpf + "Style.Triggers"));

         // A line's words take its colour and nothing behind them.
         XElement line = list.Element(Wpf + "ListBox.ItemTemplate").Descendants(Wpf + "TextBlock").Single();
         Assert.Null(line.Attribute("Background"));
      }

      /// <summary>
      /// No list in the Control Panel draws its rows on the system's template:
      /// an item style a view gives a list box or a list view is based on the
      /// application's row, in XAML and in code, so that the hover and
      /// selection every colour here is measured on are the ones on screen.
      /// The live log's style was the one that was not.
      /// </summary>
      [Fact]
      public void EveryListItemStyleIsBasedOnTheApplicationsRow()
      {
         string controlPanel = FindControlPanelDirectory();
         if (controlPanel == null)
            return;

         var offenders = new List<string>();
         foreach (string path in Directory.EnumerateFiles(controlPanel, "*.*", SearchOption.AllDirectories))
         {
            string relative = Path.GetRelativePath(controlPanel, path);
            if (relative.StartsWith("bin" + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase) ||
                relative.StartsWith("obj" + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase) ||
                relative.Equals("App.xaml", StringComparison.OrdinalIgnoreCase))
               continue;

            if (path.EndsWith(".xaml", StringComparison.OrdinalIgnoreCase))
            {
               foreach (XElement style in XDocument.Parse(File.ReadAllText(path)).Descendants(Wpf + "Style"))
               {
                  string target = ((string)style.Attribute("TargetType") ?? string.Empty).Replace("{x:Type ", string.Empty).TrimEnd('}');
                  if ((target is "ListBoxItem" or "ListViewItem") && style.Attribute("BasedOn") == null)
                     offenders.Add(relative + ": a " + target + " style with no BasedOn");
               }
            }
            else if (path.EndsWith(".cs", StringComparison.OrdinalIgnoreCase) &&
                     Regex.IsMatch(File.ReadAllText(path), @"new\s+Style\s*\(\s*typeof\s*\(\s*(?:ListBoxItem|ListViewItem)\s*\)\s*\)"))
            {
               offenders.Add(relative + ": a list item style built in code with no BasedOn");
            }
         }

         Assert.True(offenders.Count == 0, "These lists draw their rows on the system's template:\n" + string.Join("\n", offenders));
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

      // ---- The filled destructive button (F182) ---------------------------------

      /// <summary>
      /// The filled Danger button's words clear 4.5:1 on its fill at rest, under
      /// the pointer and pressed. WPF-UI's own was #F44336 in both themes with
      /// the theme's primary text on it: white, 3.7:1, in the dark theme.
      /// </summary>
      [Theory]
      [MemberData(nameof(LightAndDark))]
      public void TheFilledDangerButtonsWordsClearFourPointFiveToOneInEveryState(ChartTheme theme)
      {
         DesignTokens.DangerSolidArgb danger = DesignTokens.DangerSolid(theme, ChartSystemColors.Fallback);
         Assert.Equal(0xFFu, danger.Text >> 24);

         foreach ((string state, uint fill) in new[] { ("at rest", danger.Fill), ("under the pointer", danger.Hover), ("pressed", danger.Pressed) })
         {
            Assert.Equal(0xFFu, fill >> 24);
            double ratio = ChartPalette.ContrastRatio(danger.Text, fill);
            Assert.True(ratio >= 4.5, $"{theme}: the Danger button's words are {ratio:F2}:1 on its fill {state}, under 4.5:1.");
         }

         // At rest it is the palette's own danger colour, filled.
         Assert.Equal(DesignTokens.Status(theme, ChartSystemColors.Fallback).Danger, danger.Fill);
      }

      /// <summary>Under High Contrast the button is the window text with the window colour on it, in every state.</summary>
      [Fact]
      public void UnderHighContrastTheFilledDangerButtonIsTheWindowTextWithTheWindowOnIt()
      {
         var system = new ChartSystemColors(0xFF112233, 0xFFEEDDCC, 0xFF445566, 0xFF778899, 0xFFAABBCC);
         DesignTokens.DangerSolidArgb danger = DesignTokens.DangerSolid(ChartTheme.HighContrast, system);

         Assert.Equal(system.WindowText, danger.Fill);
         Assert.Equal(system.WindowText, danger.Hover);
         Assert.Equal(system.WindowText, danger.Pressed);
         Assert.Equal(system.Window, danger.Text);
      }

      /// <summary>
      /// App.xaml gives the Danger appearance the tokens through an implicit
      /// style on WPF-UI's button, based on WPF-UI's own: style triggers, which
      /// outrank the template triggers WPF-UI sets #F44336 in, and the disabled
      /// words again after them. ThemeTokens publishes the four brushes, and
      /// Tokens.xaml declares them with the dark values.
      /// </summary>
      [Fact]
      public void TheDangerAppearanceTakesItsColoursFromTheTokens()
      {
         string app = ReadControlPanelFile("App.xaml");
         string themeTokens = ReadControlPanelFile(Path.Join("Services", "ThemeTokens.cs"));
         string tokensXaml = ReadControlPanelFile(Path.Join("Views", "Scaffold", "Tokens.xaml"));
         if (app == null || themeTokens == null || tokensXaml == null)
            return;

         XElement style = XDocument.Parse(app).Root.Descendants(Wpf + "Style")
            .Single(s => (string)s.Attribute("TargetType") == "{x:Type ui:Button}" && s.Attribute(X + "Key") == null);
         Assert.Equal("{StaticResource DefaultUiButtonStyle}", (string)style.Attribute("BasedOn"));

         XElement danger = style.Element(Wpf + "Style.Triggers").Elements(Wpf + "Trigger")
            .Single(t => (string)t.Attribute("Property") == "Appearance" && (string)t.Attribute("Value") == "Danger");
         var setters = danger.Elements(Wpf + "Setter").ToDictionary(s => (string)s.Attribute("Property"), s => (string)s.Attribute("Value"));
         Assert.Equal("{DynamicResource AppDangerSolidBrush}", setters["Background"]);
         Assert.Equal("{DynamicResource AppDangerSolidHoverBrush}", setters["MouseOverBackground"]);
         Assert.Equal("{DynamicResource AppDangerSolidPressedBrush}", setters["PressedBackground"]);
         Assert.Equal("{DynamicResource AppOnDangerSolidBrush}", setters["Foreground"]);

         XElement disabled = style.Element(Wpf + "Style.Triggers").Elements(Wpf + "MultiTrigger").Single();
         Assert.Contains(disabled.Descendants(Wpf + "Condition"), c => (string)c.Attribute("Property") == "IsEnabled" && (string)c.Attribute("Value") == "False");
         Assert.Equal("{DynamicResource ButtonForegroundDisabled}",
            (string)disabled.Elements(Wpf + "Setter").Single(s => (string)s.Attribute("Property") == "Foreground").Attribute("Value"));

         DesignTokens.DangerSolidArgb dark = DesignTokens.DangerSolid(ChartTheme.Dark, ChartSystemColors.Fallback);
         foreach ((string key, uint colour) in new[]
         {
            ("AppDangerSolidBrush", dark.Fill), ("AppDangerSolidHoverBrush", dark.Hover),
            ("AppDangerSolidPressedBrush", dark.Pressed), ("AppOnDangerSolidBrush", dark.Text)
         })
         {
            Assert.Contains("Publish(\"" + key + "\"", themeTokens, StringComparison.Ordinal);
            Assert.Equal(colour.ToString("X8"), DeclaredColour(tokensXaml, key));
         }
      }

      // ---- A text field at rest (F189) --------------------------------------------

      /// <summary>
      /// A text field's bottom edge clears 3:1 (WCAG 2.2, SC 1.4.11) against
      /// every surface a field sits on and against the field's own fill, at rest
      /// and under the pointer - the line is drawn over what is behind the
      /// field, and the fill is inside it. Fluent's own measured 1.5 to 1.7:1,
      /// and its outline 1.1:1. The outline round the top and sides stays the
      /// theme's subtle stroke.
      /// </summary>
      [Theory]
      [MemberData(nameof(LightAndDark))]
      public void ATextFieldsBottomEdgeClearsThreeToOneOnEverySurfaceAndOnItsFill(ChartTheme theme)
      {
         DesignTokens.TextFieldStrokeArgb? stroke = DesignTokens.TextFieldStroke(theme);
         Assert.True(stroke.HasValue, theme + " has no text field border of its own.");

         bool light = theme == ChartTheme.Light;
         // WPF-UI's TextControlBackground and TextControlBackgroundPointerOver.
         (uint rest, uint pointer) = light ? (0xB3FFFFFFu, 0x80F9F9F9u) : (0x0FFFFFFFu, 0x15FFFFFFu);

         foreach ((string name, uint surface) in Surfaces(theme).Where(s => s.Name is "the page" or "a card" or "a dialog" or "a dialog's darkest"))
         {
            uint edge = Over(stroke.Value.Edge, surface);

            double onSurface = ChartPalette.ContrastRatio(edge, surface);
            Assert.True(onSurface >= 3.0, $"{theme}: a field's bottom edge is {onSurface:F2}:1 on {name} ({surface:X8}), under 3:1.");

            foreach ((string state, uint fill) in new[] { ("at rest", rest), ("under the pointer", pointer) })
            {
               double onFill = ChartPalette.ContrastRatio(edge, Over(fill, surface));
               Assert.True(onFill >= 3.0, $"{theme}: on {name} a field's bottom edge is {onFill:F2}:1 against its fill {state}, under 3:1.");
            }
         }

         // The outline stays the theme's subtle stroke: translucent and faint.
         Assert.InRange(stroke.Value.Outline >> 24, 1u, 0x20u);
      }

      /// <summary>Under High Contrast the theme's own border stands: there is no value to publish.</summary>
      [Fact]
      public void UnderHighContrastATextFieldKeepsTheThemesBorder()
      {
         Assert.Null(DesignTokens.TextFieldStroke(ChartTheme.HighContrast));
      }

      /// <summary>
      /// ThemeTokens republishes the theme's own key the text box, password box
      /// and number box draw their border in, and takes it away under High
      /// Contrast; App.xaml gives the date picker the same border on all four
      /// sides, where WPF-UI drew it with the buttons' border and no bottom edge.
      /// </summary>
      [Fact]
      public void TheTextFieldsAndTheDatePickerTakeTheBorderWithTheStrongBottomEdge()
      {
         string themeTokens = ReadControlPanelFile(Path.Join("Services", "ThemeTokens.cs"));
         string app = ReadControlPanelFile("App.xaml");
         if (themeTokens == null || app == null)
            return;

         Assert.Contains("PublishValue(\"TextControlElevationBorderBrush\", TextFieldBorder_(fieldStroke))", themeTokens, StringComparison.Ordinal);
         Assert.Contains("Resources.Remove(\"TextControlElevationBorderBrush\")", themeTokens, StringComparison.Ordinal);

         XElement style = XDocument.Parse(app).Root.Descendants(Wpf + "Style")
            .Single(s => (string)s.Attribute("TargetType") == "DatePicker" && s.Attribute(X + "Key") == null);
         Assert.Equal("{StaticResource DefaultDatePickerStyle}", (string)style.Attribute("BasedOn"));
         var setters = style.Elements(Wpf + "Setter").ToDictionary(s => (string)s.Attribute("Property"), s => (string)s.Attribute("Value"));
         Assert.Equal("{DynamicResource TextControlElevationBorderBrush}", setters["BorderBrush"]);
         Assert.Equal("1", setters["BorderThickness"]);
      }

      // ---- Links (F190, F011) --------------------------------------------------

      /// <summary>
      /// A link clears 4.5:1 at rest and under the pointer on every surface its
      /// theme draws text on. WPF's own link colour, the system's #0066CC, is
      /// 2.5:1 on the dark card, and it turned red under the pointer.
      /// </summary>
      [Theory]
      [MemberData(nameof(LightAndDark))]
      public void ALinkClearsFourPointFiveToOneOnEverySurfaceAtRestAndUnderThePointer(ChartTheme theme)
      {
         DesignTokens.LinkArgb link = DesignTokens.Link(theme, ChartSystemColors.Fallback);

         foreach ((string state, uint colour) in new[] { ("at rest", link.Rest), ("under the pointer", link.Hover) })
         {
            Assert.Equal(0xFFu, colour >> 24);

            foreach ((string name, uint surface) in Surfaces(theme))
            {
               double ratio = ChartPalette.ContrastRatio(colour, surface);
               Assert.True(ratio >= 4.5, $"{theme}: a link {state} is {ratio:F2}:1 on {name} ({surface:X8}), under 4.5:1.");
            }
         }

         // Under the pointer it is a different colour, and never the danger colour.
         Assert.NotEqual(link.Rest, link.Hover);
         Assert.NotEqual(DesignTokens.Status(theme, ChartSystemColors.Fallback).Danger, link.Hover);
      }

      /// <summary>Under High Contrast a link is the system's link colour, HotTrack.</summary>
      [Fact]
      public void UnderHighContrastALinkIsHotTrack()
      {
         var system = new ChartSystemColors(0xFF112233, 0xFFEEDDCC, 0xFF445566, 0xFF778899, 0xFFAABBCC);
         DesignTokens.LinkArgb link = DesignTokens.Link(ChartTheme.HighContrast, system);

         Assert.Equal(system.HotTrack, link.Rest);
         Assert.Equal(system.HotTrack, link.Hover);
      }

      /// <summary>
      /// App.xaml's implicit Hyperlink style reaches every link - the About page's,
      /// the help pane's and the help documents', none of which sets a colour of its
      /// own - with the link colour, the hover colour, the underline and the
      /// disabled text colour; ThemeTokens publishes the two brushes and Tokens.xaml
      /// declares them with the dark values.
      /// </summary>
      [Fact]
      public void EveryLinkTakesTheLinkColourFromTheImplicitHyperlinkStyle()
      {
         string app = ReadControlPanelFile("App.xaml");
         string themeTokens = ReadControlPanelFile(Path.Join("Services", "ThemeTokens.cs"));
         string tokensXaml = ReadControlPanelFile(Path.Join("Views", "Scaffold", "Tokens.xaml"));
         if (app == null || themeTokens == null || tokensXaml == null)
            return;

         XElement style = XDocument.Parse(app).Root.Descendants(Wpf + "Style")
            .Single(s => (string)s.Attribute("TargetType") == "Hyperlink" && s.Attribute(X + "Key") == null);
         var setters = style.Elements(Wpf + "Setter").ToDictionary(s => (string)s.Attribute("Property"), s => (string)s.Attribute("Value"));
         Assert.Equal("{DynamicResource AppLinkBrush}", setters["Foreground"]);
         Assert.Equal("Underline", setters["TextDecorations"]);

         List<XElement> triggers = style.Element(Wpf + "Style.Triggers").Elements(Wpf + "Trigger").ToList();
         string Foreground(string property, string value) => (string)triggers
            .Single(t => (string)t.Attribute("Property") == property && (string)t.Attribute("Value") == value)
            .Elements(Wpf + "Setter").Single(s => (string)s.Attribute("Property") == "Foreground").Attribute("Value");
         Assert.Equal("{DynamicResource AppLinkHoverBrush}", Foreground("IsMouseOver", "True"));
         Assert.Equal("{DynamicResource TextFillColorDisabledBrush}", Foreground("IsEnabled", "False"));

         DesignTokens.LinkArgb dark = DesignTokens.Link(ChartTheme.Dark, ChartSystemColors.Fallback);
         Assert.Contains("Publish(\"AppLinkBrush\"", themeTokens, StringComparison.Ordinal);
         Assert.Contains("Publish(\"AppLinkHoverBrush\"", themeTokens, StringComparison.Ordinal);
         Assert.Equal(dark.Rest.ToString("X8"), DeclaredColour(tokensXaml, "AppLinkBrush"));
         Assert.Equal(dark.Hover.ToString("X8"), DeclaredColour(tokensXaml, "AppLinkHoverBrush"));

         // The links themselves set no colour that would outrank the style.
         foreach (string file in new[] { Path.Join("Views", "AboutView.cs"), Path.Join("Views", "HelpPane.cs"), Path.Join("Services", "MarkdownFlow.cs") })
         {
            string source = ReadControlPanelFile(file);
            Assert.True(source != null && source.Contains("Hyperlink", StringComparison.Ordinal), file + " no longer builds a link.");
            Assert.DoesNotMatch(new Regex(@"\b(?:link|hl|webLink)\.Foreground\b|Hyperlink\([^;]*Foreground\s*="), source);
         }
      }

      /// <summary>
      /// WPF-UI's link button - the sign-in page's "Set up two-factor
      /// authentication..." - takes the link colours too: its theme draws its words
      /// in HyperlinkButtonForeground, ...PointerOver and ...Pressed, shades of
      /// the Windows accent, which the implicit Hyperlink style does not reach, so
      /// that until 9 October 2026 the first link an administrator sees followed
      /// the user's accent (#005494 on a light card with Windows' default blue)
      /// while every other link was #0B5CB5. ThemeTokens shadows the three keys
      /// with the link colours, and takes them away under High Contrast; no link
      /// button and no style for one sets a colour that would outrank them. Its
      /// fill under the pointer is the theme's subtle fill, the "hovered row" the
      /// link colours are measured on.
      /// </summary>
      [Fact]
      public void TheLinkButtonTakesTheLinkColourInPlaceOfTheWindowsAccent()
      {
         Assert.True(DesignTokens.LinkButtonFor(ChartTheme.Light));
         Assert.True(DesignTokens.LinkButtonFor(ChartTheme.Dark));
         Assert.False(DesignTokens.LinkButtonFor(ChartTheme.HighContrast));

         string themeTokens = ReadControlPanelFile(Path.Join("Services", "ThemeTokens.cs"));
         string controlPanel = FindControlPanelDirectory();
         if (themeTokens == null || controlPanel == null)
            return;

         Assert.Contains("if (DesignTokens.LinkButtonFor(CurrentChartTheme))", themeTokens, StringComparison.Ordinal);
         foreach ((string key, string colour) in new[]
         {
            ("HyperlinkButtonForeground", "link.Rest"),
            ("HyperlinkButtonForegroundPointerOver", "link.Hover"),
            ("HyperlinkButtonForegroundPressed", "link.Hover"),
         })
         {
            Assert.Contains("Publish(\"" + key + "\", FromArgb(" + colour + "));", themeTokens, StringComparison.Ordinal);
            Assert.Contains("Resources.Remove(\"" + key + "\");", themeTokens, StringComparison.Ordinal);
         }

         int buttons = 0;
         var offenders = new List<string>();
         foreach (string path in Directory.EnumerateFiles(controlPanel, "*.xaml", SearchOption.AllDirectories))
         {
            string relative = Path.GetRelativePath(controlPanel, path);
            if (relative.StartsWith("bin" + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase) ||
                relative.StartsWith("obj" + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
               continue;

            XDocument document = XDocument.Parse(File.ReadAllText(path));
            foreach (XElement button in document.Descendants().Where(e => e.Name.LocalName == "HyperlinkButton"))
            {
               buttons++;
               if (button.Attribute("Foreground") != null || button.Elements().Any(e => e.Name.LocalName == "HyperlinkButton.Foreground"))
                  offenders.Add(relative + ": a link button with a Foreground of its own");
            }

            foreach (XElement style in document.Descendants(Wpf + "Style"))
            {
               if (((string)style.Attribute("TargetType") ?? string.Empty).Contains("HyperlinkButton", StringComparison.Ordinal) &&
                   style.Descendants(Wpf + "Setter").Any(s => (string)s.Attribute("Property") == "Foreground"))
                  offenders.Add(relative + ": a link button style that sets Foreground");
            }
         }

         Assert.True(buttons > 0, "No view draws a link button any more; this test has nothing left to hold.");
         Assert.True(offenders.Count == 0, "These link buttons outrank the link colour:\n" + string.Join("\n", offenders));
      }

      // ---- helpers ---------------------------------------------------------------

      private static readonly XNamespace Wpf = "http://schemas.microsoft.com/winfx/2006/xaml/presentation";
      private static readonly XNamespace X = "http://schemas.microsoft.com/winfx/2006/xaml";

      /// <summary>A brush's colour as Tokens.xaml declares it, as AARRGGBB.</summary>
      private static string DeclaredColour(string tokensXaml, string key)
      {
         Match match = Regex.Match(tokensXaml, "x:Key=\"" + key + "\" Color=\"#([0-9A-Fa-f]+)\"");
         Assert.True(match.Success, key + " is not a brush in Tokens.xaml.");
         string hex = match.Groups[1].Value.ToUpperInvariant();
         return hex.Length == 6 ? "FF" + hex : hex;
      }

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
