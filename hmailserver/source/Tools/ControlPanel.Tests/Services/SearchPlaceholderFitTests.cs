// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using System.Xml.Linq;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// Every list's search placeholder fits its box in every language: the box is the
   /// toolbar's, 220 to 360 pixels (the live log's filter 480), and its placeholder is
   /// its accessible name as well, so a cut one is a cut name. The final check of Tier 2
   /// found the live log's filter cut at "oder s" in German at 1280 and, by measure, in
   /// fourteen more languages, and the French greylisting bypass list's and the Russian
   /// allow and block list's past their box (g3 defect 4).
   ///
   /// The width is estimated from the characters in the box's font, Segoe UI at 14 -
   /// calibrated on the German render (53 characters in the 309 pixels the box shows of
   /// its text) and on the Russian list (44 characters, 12 pixels over) - since the
   /// tests draw nothing: a narrow letter 3.6, a wide one 10.5, a capital 8.2, a
   /// Cyrillic letter 7.5 (8.2 a capital), a CJK character 14, any other 6.6.
   /// </summary>
   public class SearchPlaceholderFitTests
   {
      // The text a box shows beside its glyph and its clear button: 360 less 51, and
      // three pixels for the estimate's error, which put the French allow and block
      // list's 309 pixels - whole in the box - at 309.4.
      private const double DefaultText = 312;
      private const double LogText = 480 - 48;

      [Fact]
      public void EverySearchPlaceholder_FitsItsBox_InEveryLanguage()
      {
         string root = ContrastTokensTests.FindControlPanelDirectory();
         Assert.True(root != null, "The ControlPanel folder could not be found from the test binaries.");

         var placeholders = new Dictionary<string, double>(StringComparer.Ordinal);
         foreach (string path in Directory.EnumerateFiles(Path.Join(root, "Views"), "*.*", SearchOption.AllDirectories))
         {
            if (!path.EndsWith(".cs", StringComparison.Ordinal) && !path.EndsWith(".xaml", StringComparison.Ordinal))
               continue;
            string text = File.ReadAllText(path);
            foreach (Match m in Regex.Matches(text, @"SearchPlaceholder\s*=\s*[NL]\(""((?:[^""\\]|\\.)*)""\)"))
               placeholders[m.Groups[1].Value] = DefaultText;
            foreach (Match m in Regex.Matches(text, @"<scaffold:Toolbar\b[^>]*>"))
            {
               Match placeholder = Regex.Match(m.Value, @"SearchPlaceholder=""\{loc:L '((?:[^'\\]|\\.)*)'\}""");
               if (!placeholder.Success)
                  continue;
               Match max = Regex.Match(m.Value, @"SearchMaxWidth=""(\d+)""");
               placeholders[placeholder.Groups[1].Value.Replace("\\'", "'", StringComparison.Ordinal)] =
                  max.Success ? double.Parse(max.Groups[1].Value, System.Globalization.CultureInfo.InvariantCulture) - 48 : DefaultText;
            }
         }

         Assert.True(placeholders.Count > 20, "The search placeholders were not found.");
         Assert.Equal(LogText, placeholders["Filter lines: an address, any words, or session=42"]);

         var cut = new List<string>();
         foreach (string path in Directory.GetFiles(Path.Join(root, "Resources"), "Strings*.resx"))
         {
            Dictionary<string, string> values = XDocument.Load(path).Root.Elements("data")
               .ToDictionary(d => (string)d.Attribute("name"), d => (string)d.Element("value") ?? "", StringComparer.Ordinal);
            foreach ((string key, double room) in placeholders)
            {
               string shown = values.TryGetValue(key, out string value) ? value : key;
               double width = Width(shown);
               if (width > room)
                  cut.Add(Path.GetFileName(path) + ": " + shown + " (about " + Math.Round(width) + " of " + room + ")");
            }
         }

         Assert.True(cut.Count == 0, "Search placeholders wider than their box:" + Environment.NewLine + string.Join(Environment.NewLine, cut));
      }

      [Fact]
      public void TheSearchBoxsWidest_IsTheToolbars()
      {
         string root = ContrastTokensTests.FindControlPanelDirectory();
         string scaffold = File.ReadAllText(Path.Join(root, "Views", "Scaffold", "Scaffold.xaml"));
         Assert.Contains("MinWidth=\"220\" MaxWidth=\"{TemplateBinding SearchMaxWidth}\"", scaffold, StringComparison.Ordinal);
         string toolbar = File.ReadAllText(Path.Join(root, "Views", "Scaffold", "Toolbar.cs"));
         Assert.Contains("public const double DefaultSearchMaxWidth = 360;", toolbar, StringComparison.Ordinal);
      }

      private static double Width(string text)
      {
         double width = 0;
         foreach (char c in text)
         {
            if (c >= '　')
               width += 14;
            else if (c >= 'Ѐ' && c <= 'ӿ')
               width += char.IsUpper(c) ? 8.2 : 7.5;
            else if (char.IsUpper(c))
               width += 8.2;
            else if ("il.,:;'| ".IndexOf(c) >= 0)
               width += 3.6;
            else if ("mw".IndexOf(c) >= 0)
               width += 10.5;
            else
               width += 6.6;
         }

         return width;
      }
   }
}
