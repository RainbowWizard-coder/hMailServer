// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Xml.Linq;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The rule card's THEN footer moves an action with two glyph buttons named for
   /// what they do (review finding F178). The footer's Up was a literal, "U_p",
   /// that no language translated - Japanese read "アクションを削除(M) | Up | 下へ(N)" -
   /// and the five captioned buttons did not fit the half-width card at 1280, so
   /// Down wrapped onto a line of its own.
   /// </summary>
   public class RulesActionOrderTests
   {
      [Fact]
      public void UpAndDownAreGlyphsNamedForTheActionTheyMove()
      {
         string xaml = ReadSource("RulesView.xaml");

         string up = Element(xaml, "ActionUpButton");
         Assert.Contains("Icon=\"{ui:SymbolIcon ChevronUp24}\"", up);
         Assert.Contains("AutomationProperties.Name=\"{loc:L 'Move action up'}\"", up);
         Assert.Contains("ToolTip=\"{loc:L 'Move the selected action up (Alt+Up)'}\"", up);
         Assert.DoesNotContain("Content=", up);

         string down = Element(xaml, "ActionDownButton");
         Assert.Contains("Icon=\"{ui:SymbolIcon ChevronDown24}\"", down);
         Assert.Contains("AutomationProperties.Name=\"{loc:L 'Move action down'}\"", down);
         Assert.Contains("ToolTip=\"{loc:L 'Move the selected action down (Alt+Down)'}\"", down);
         Assert.DoesNotContain("Content=", down);

         Assert.DoesNotContain("Content=\"U_p\"", xaml);

         // Beside the list they reorder, not in the footer, whose line they took from
         // the captioned buttons (the final check of Tier 2).
         int grid = xaml.IndexOf("x:Name=\"ActionsGrid\"", StringComparison.Ordinal);
         int footer = xaml.IndexOf("x:Name=\"ActionsFooter\"", StringComparison.Ordinal);
         int upAt = xaml.IndexOf("x:Name=\"ActionUpButton\"", StringComparison.Ordinal);
         Assert.True(grid >= 0 && upAt > grid && upAt < footer, "Up and Down are not beside the actions' list.");
      }

      [Fact]
      public void AltUpAndAltDownMoveTheSelectedActionAndItStaysSelected()
      {
         string code = ReadSource("RulesView.xaml.cs");

         Assert.Contains("ActionsGrid.PreviewKeyDown += ActionsGrid_PreviewKeyDown;", code);
         Assert.Matches(@"Keyboard\.Modifiers != System\.Windows\.Input\.ModifierKeys\.Alt", code);
         Assert.Contains("e.Key == System.Windows.Input.Key.System ? e.SystemKey : e.Key", code);
         Assert.Contains("MoveAction(key == System.Windows.Input.Key.Up);", code);

         // After the move the list is read again; the moved action is selected again.
         int move = code.IndexOf("private void MoveAction(bool up)", StringComparison.Ordinal);
         Assert.True(move >= 0);
         string body = code.Substring(move);
         Assert.Matches(@"RefreshDetails\(\);\s*(//[^\n]*\n\s*)*foreach \(object item in ActionsGrid\.Items\)", body);
         Assert.Contains("ActionsGrid.SelectedItem = moved;", body);
      }

      [Fact]
      public void EveryLanguageNamesTheTwoButtons()
      {
         string[] keys =
         {
            "Move action up", "Move action down",
            "Move the selected action up (Alt+Up)", "Move the selected action down (Alt+Down)",
         };

         var missing = new List<string>();
         foreach (string path in Directory.GetFiles(ResourcesDirectory(), "Strings.*.resx"))
         {
            var values = new Dictionary<string, string>(StringComparer.Ordinal);
            foreach (XElement data in XDocument.Load(path).Root.Elements("data"))
               values[(string)data.Attribute("name")] = data.Element("value")?.Value ?? "";

            foreach (string key in keys)
            {
               if (!values.TryGetValue(key, out string value) || string.IsNullOrWhiteSpace(value) || value == key)
                  missing.Add(Path.GetFileName(path) + ": " + key);
            }
         }

         Assert.True(missing.Count == 0, string.Join(Environment.NewLine, missing));
      }

      /// <summary>The XAML element that carries x:Name="<paramref name="name"/>", from its tag to its end.</summary>
      private static string Element(string xaml, string name)
      {
         int at = xaml.IndexOf("x:Name=\"" + name + "\"", StringComparison.Ordinal);
         Assert.True(at >= 0, name + " is not in the page.");
         int start = xaml.LastIndexOf('<', at);
         int end = xaml.IndexOf("/>", at, StringComparison.Ordinal);
         return xaml.Substring(start, end - start + 2);
      }

      private static string ResourcesDirectory()
      {
         string dir = AppContext.BaseDirectory;
         for (int i = 0; i < 8 && dir != null; i++)
         {
            string candidate = Path.Join(dir, "ControlPanel", "Resources");
            if (File.Exists(Path.Join(candidate, "Strings.resx")))
               return candidate;
            dir = Path.GetDirectoryName(dir);
         }

         Assert.Fail("ControlPanel/Resources could not be found from the test binaries.");
         return null;
      }

      private static string ReadSource(string view)
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", "Views", view);
            if (File.Exists(candidate))
               return File.ReadAllText(candidate);
            directory = directory.Parent;
         }

         Assert.Fail("ControlPanel/Views/" + view + " could not be found from the test binaries.");
         return null;
      }
   }
}
