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
   /// Text the code joins writes the spaced hyphen with one space each side, as the
   /// catalogues do (the owner's choice of 9 October 2026; review findings F037 and
   /// F299): build/check-catalogues.py refuses a translated text padded for an
   /// alignment the layout never keeps, and this holds the joins the catalogues never
   /// see. Directory synchronisation's preview wrote "jane.doe@example.com  -  create"
   /// and "(no address)  -  skip" under a warning that wrote " - ", and its skipped
   /// entries "1  -  the entry has no mail attribute", as the About page's
   /// "Version {0}  -  .NET {1}" and REST API keys' "{0}  -  {1}, expires {2}" did.
   /// </summary>
   public class PaddedJoinTests
   {
      private static readonly Regex Literal = new(@"(?<!@)""((?:[^""\\\r\n]|\\.)*)""", RegexOptions.Compiled);
      private static readonly Regex Padded = new(@" {2,}[-\u2013\u2014]|[-\u2013\u2014] {2,}", RegexOptions.Compiled);

      [Fact]
      public void NoStringInTheCodePadsADashWithSpaces()
      {
         string controlPanel = ContrastTokensTests.FindControlPanelDirectory();
         Assert.True(controlPanel != null, "The Control Panel's sources were not found above the test binaries.");

         var padded = new List<string>();
         int files = 0;
         IEnumerable<string> sources = new[] { "Views", "Services" }
            .SelectMany(folder => Directory.GetFiles(Path.Join(controlPanel, folder), "*.cs", SearchOption.AllDirectories));
         foreach (string file in sources)
         {
            files++;
            string[] lines = File.ReadAllLines(file);
            for (int i = 0; i < lines.Length; i++)
            {
               string line = lines[i];
               if (line.TrimStart().StartsWith("//", StringComparison.Ordinal))
                  continue;
               foreach (Match literal in Literal.Matches(line))
               {
                  if (Padded.IsMatch(literal.Groups[1].Value))
                     padded.Add(Path.GetFileName(file) + ":" + (i + 1) + ": \"" + literal.Groups[1].Value + "\"");
               }
            }
         }

         Assert.True(files > 100, "Fewer source files than the Control Panel has: " + files);
         Assert.True(padded.Count == 0, "A join pads its dash with spaces; write \" - \":\n" + string.Join("\n", padded));
      }

      /// <summary>The test finds the shape it refuses, and leaves the spaced hyphen and a minus sign alone.</summary>
      [Theory]
      [InlineData("  -  ", true)]
      [InlineData("  \u2014  ", true)]
      [InlineData(" -  ", true)]
      [InlineData(" - ", false)]
      [InlineData("-5", false)]
      [InlineData("a-b", false)]
      public void ThePaddingIsFound(string text, bool padded)
      {
         Assert.Equal(padded, Padded.IsMatch(text));
      }
   }
}
