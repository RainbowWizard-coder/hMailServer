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
   /// A placeholder is an example of what the box takes, marked as one - "e.g.
   /// mail.example.com" - and never the field's name, its default or what an empty box
   /// means, which are its caption's and its hint's (docs/ControlPanelDesign.md; review
   /// findings F138 and F303). The final check of Tier 2 found the rule held on the pages
   /// the findings named and broken on the rest (g4 defect 7, g1 defect 1): the Updates
   /// page's Unattended window showed "Sun 03:00" under "empty = never apply on its own",
   /// which read as a schedule in force, its feed URL box the real feed's address, the
   /// directory authentication's Port "Automatic - 636 for LDAPS, 389 otherwise" cut at
   /// its box's edge, and some forty boxes a bare example or their default. Every
   /// placeholder a view writes is now a marked example, a settings row's bare example
   /// marked where its box is made, or one of the few kinds listed here with its reason.
   /// </summary>
   public class PlaceholderExamplesTests
   {
      // What a placeholder may be other than a marked example, and why.
      private static readonly string[] AllowedPrefixes =
      {
         "ExamplePlaceholder.Of(", "ExamplePlaceholder.Shown(", "F(\"e.g. {0}\"",
         // A variable a helper is handed; its callers are checked where they hand it.
         "placeholder", "label",
         // A secret's state: whether one is stored and blank keeps it.
         "Placeholder_()", "clearServicePassword_.IsChecked is true ? \"\"", "\"\"",
         // Format masks for a code and a time.
         "\"000000\"", "\"00:00\"",
         // Search boxes: what the search finds.
         "L(\"Part of a name (empty = everyone)\")", "L(\"Search, or say what you want to do",
         // The label box's purpose with an example in it.
         "L(\"What this key is for, e.g.",
         // The two directory filters' shipped default, wrapped whole in the box and named
         // as the default in the hint (review finding F131).
         "DefaultSyncFilter", "DefaultUserSearchFilter",
         // A key the server holds empty, said to be where the box would otherwise show an
         // example over an empty value it does not use (release review finding CP-4), with
         // what the server uses for it where its reader puts a value in its place.
         "heldEmpty ? IniFeatureStore.HeldEmptyPlaceholder(EmptyMeans) : ExamplePlaceholder.Shown(",
      };

      [Fact]
      public void EveryPlaceholderAViewWrites_IsAMarkedExample()
      {
         var offending = new List<string>();
         var assignment = new Regex(@"(?:PlaceholderText\s*=\s*|\bplaceholder:\s*)(?<rhs>.+)$");
         foreach (string file in Directory.GetFiles(ViewsDirectory(), "*.cs", SearchOption.AllDirectories))
         {
            string[] lines = File.ReadAllLines(file);
            for (int i = 0; i < lines.Length; i++)
            {
               string line = lines[i].Trim();
               if (line.StartsWith("//", StringComparison.Ordinal))
                  continue;
               Match match = assignment.Match(line);
               if (!match.Success)
                  continue;
               string rhs = match.Groups["rhs"].Value.Trim();
               if (!AllowedPrefixes.Any(prefix => rhs.StartsWith(prefix, StringComparison.Ordinal)))
                  offending.Add(Path.GetFileName(file) + ":" + (i + 1) + ": " + rhs);
            }
         }

         foreach (string file in Directory.GetFiles(ViewsDirectory(), "*.xaml", SearchOption.AllDirectories))
         {
            foreach (Match match in Regex.Matches(File.ReadAllText(file), @"PlaceholderText=""(?<value>[^""]*)"""))
            {
               string value = match.Groups["value"].Value;
               // The toolbar's search box, and the passphrase box, which names what a
               // value typed there does: it replaces the stored one.
               if (value != "{TemplateBinding SearchPlaceholder}" && value != "{loc:L 'New passphrase'}")
                  offending.Add(Path.GetFileName(file) + ": PlaceholderText=\"" + value + "\"");
            }
         }

         Assert.True(offending.Count == 0, "A placeholder that is not a marked example:" + Environment.NewLine + string.Join(Environment.NewLine, offending));
      }

      /// <summary>
      /// The settings pages' rows hold a bare example, which the box marks where it is
      /// made, and never a sentence, a marked example of their own (which would read
      /// "e.g. e.g."), or the row's own default.
      /// </summary>
      [Theory]
      [InlineData("FeatureSettingsView.xaml.cs", 4)]
      [InlineData("ServerSettingsView.xaml.cs", 2)]
      public void ASettingsRowsExample_IsBare_MarkedAtItsBox_AndNeverItsDefault(string file, int sinks)
      {
         string code = File.ReadAllText(Path.Join(ViewsDirectory(), file));
         Assert.Equal(sinks, Regex.Matches(code, @"ExamplePlaceholder\.Shown\(Placeholder\)").Count);
         Assert.DoesNotMatch(new Regex(@"\bPlaceholder = (?:L|F)\("), code);
         Assert.DoesNotMatch(new Regex(@"\bPlaceholder = ExamplePlaceholder\."), code);
         Assert.DoesNotMatch(new Regex(@"\bPlaceholder = Placeholder\b"), code);

         // Default and Placeholder in one row's initialiser, the same text.
         foreach (Match row in Regex.Matches(code, @"new \w+Setting\s*\{[^{}]*\}|new Com\w+\s*\{[^{}]*\}"))
         {
            Match placeholder = Regex.Match(row.Value, @"\bPlaceholder = (""(?:[^""\\]|\\.)*"")");
            Match fallback = Regex.Match(row.Value, @"\bDefault = (""(?:[^""\\]|\\.)*"")");
            Assert.False(placeholder.Success && fallback.Success && placeholder.Groups[1].Value == fallback.Groups[1].Value,
               file + ": a row shows its default as its placeholder: " + placeholder.Value);
         }
      }

      [Fact]
      public void AnExample_IsMarkedOnce()
      {
         string helper = File.ReadAllText(Path.Join(ViewsDirectory(), "ExamplePlaceholder.cs"));
         Assert.Contains("public static string Of(string example) => F(\"e.g. {0}\", example);", helper, StringComparison.Ordinal);
         Assert.Contains("public static string Shown(string example) => string.IsNullOrEmpty(example) ? \"\" : Of(example);", helper, StringComparison.Ordinal);
      }

      private static string ViewsDirectory()
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", "Views");
            if (Directory.Exists(candidate))
               return candidate;
            directory = directory.Parent;
         }

         Assert.Fail("ControlPanel/Views could not be found from the test binaries.");
         return null;
      }
   }
}
