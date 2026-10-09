// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// A colour follows a change of theme only if it is taken by its resource key.
   /// ThemeTokens keeps one shared brush per status colour and recolours it in
   /// place, which cannot reach a brush that something froze - and a brush handed
   /// to a FrameworkElementFactory is frozen when its template is sealed. The
   /// quarantine page handed ThemeTokens.Success and ThemeTokens.Danger to its
   /// row template, so after the page had shown a row a change of theme left its
   /// 28 action glyphs in the old theme's green and red (2.5 to 2.7:1 on the new
   /// card), and FocusGlow, which read ThemeTokens.Danger's colour for a refused
   /// field's ring, drew that ring in the old red in every window.
   ///
   /// Scanned rather than rendered, as UnreadRowTests is: this assembly has no
   /// WPF, so the page cannot be built here and the brush's IsFrozen read. The
   /// render harness switches the theme over a quarantine page with rows instead.
   /// </summary>
   public class ThemeFollowingTests
   {
      /// <summary>
      /// No view and no service but ThemeTokens itself holds one of the shared
      /// status brushes: each takes the colour by its key (SetResourceReference,
      /// DynamicResource, TryFindResource). The live log's brushes are the
      /// exception the design keeps: each line is given one as data, which
      /// freezes nothing, and the log recolours with them.
      /// </summary>
      [Fact]
      public void NothingButThemeTokensHoldsASharedStatusBrush()
      {
         string controlPanel = ContrastTokensTests.FindControlPanelDirectory();
         if (controlPanel == null)
            return;

         var held = new Regex(@"\bThemeTokens\.(?:Brand|Success|Warning|Danger|Info|Neutral)\b");
         var offences = new List<string>();

         foreach (string file in Directory.GetFiles(controlPanel, "*.cs", SearchOption.AllDirectories))
         {
            string name = Path.GetRelativePath(controlPanel, file);
            if (name.StartsWith("bin", StringComparison.Ordinal) || name.StartsWith("obj", StringComparison.Ordinal)
                || name.EndsWith("ThemeTokens.cs", StringComparison.Ordinal))
               continue;

            string[] lines = File.ReadAllLines(file);
            for (int i = 0; i < lines.Length; i++)
            {
               string line = lines[i].Trim();
               if (line.StartsWith("//", StringComparison.Ordinal))
                  continue;
               if (held.IsMatch(line))
                  offences.Add(name + ":" + (i + 1) + ": " + line);
            }
         }

         Assert.True(offences.Count == 0,
            "A shared ThemeTokens brush is held, which a template factory freezes and a theme change then misses; take the colour by its key:\n" + string.Join("\n", offences));
      }

      /// <summary>
      /// The quarantine page's action glyphs - on each row, on each group and in
      /// the column's header - take the success and danger colours by key.
      /// </summary>
      [Fact]
      public void TheQuarantineGlyphsTakeTheirColoursByKey()
      {
         string source = ContrastTokensTests.ReadControlPanelFile(Path.Join("Views", "QuarantineView.cs"));
         if (source == null)
            return;

         string iconButton = WithheldRowTests.MemberText(source, "private static FrameworkElementFactory IconButton_(string action, SymbolRegular symbol, string colourKey, bool filled, string tipPath, string keyTipPath = null, string key = null)");
         Assert.True(iconButton != null, "QuarantineView.IconButton_ no longer takes its colour as a resource key.");
         Assert.Contains("icon.SetResourceReference(IconElement.ForegroundProperty, colourKey)", iconButton, StringComparison.Ordinal);
         Assert.DoesNotContain("ForegroundProperty, colour)", iconButton, StringComparison.Ordinal);

         string headerButton = WithheldRowTests.MemberText(source, "private static Button HeaderButton_(SymbolRegular symbol, string colourKey, bool filled, string tip, Action onClick)");
         Assert.True(headerButton != null, "QuarantineView.HeaderButton_ no longer takes its colour as a resource key.");
         Assert.Contains("icon.SetResourceReference(IconElement.ForegroundProperty, colourKey)", headerButton, StringComparison.Ordinal);

         Assert.Contains("private const string KeepColourKey = \"AppSuccessBrush\";", source, StringComparison.Ordinal);
         Assert.Contains("private const string RemoveColourKey = \"AppDangerBrush\";", source, StringComparison.Ordinal);

         // No brush is handed to the template factory at all.
         Assert.DoesNotMatch(new Regex(@"\.SetValue\([^;]*ForegroundProperty\s*,"), source);
      }

      /// <summary>
      /// FocusGlow's ring round a refused field reads the danger colour by its key,
      /// as the focus ring reads the focus colour; and ThemeTokens says in the log
      /// when it meets a frozen shared brush, which it cannot recolour.
      /// </summary>
      [Fact]
      public void TheRefusedFieldsRingReadsItsKey_AndAFrozenSharedBrushIsReported()
      {
         string glow = ContrastTokensTests.ReadControlPanelFile(Path.Join("Services", "FocusGlow.cs"));
         string tokens = ContrastTokensTests.ReadControlPanelFile(Path.Join("Services", "ThemeTokens.cs"));
         if (glow == null || tokens == null)
            return;

         Assert.Contains("Token_(field, \"AppDangerBrush\"", glow, StringComparison.Ordinal);

         string set = WithheldRowTests.MemberText(tokens, "private static void Set(SolidColorBrush brush, Color color)");
         Assert.True(set != null, "ThemeTokens.Set is gone.");
         Assert.Contains("frozenReported_.Add(brush)", set, StringComparison.Ordinal);
         Assert.Contains("App.LogException(", set, StringComparison.Ordinal);
      }
   }
}
