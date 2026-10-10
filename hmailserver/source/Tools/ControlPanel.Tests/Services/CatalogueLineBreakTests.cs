// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Text.RegularExpressions;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// A language catalogue writes a line break in a text as the line break itself, never
   /// as an escaped carriage return before one. The build keeps a value's line breaks as
   /// the checkout has them - CR LF on Windows - so that "&amp;#13;" before a CR LF made
   /// CR CR LF, which WPF draws as two breaks: five Swedish texts, the group members'
   /// question among them, stood a blank line further apart than in every other language.
   /// The neutral Strings.resx is left out: build/check-localisation.py writes it, and
   /// English is shown from the source's own text, not from it.
   /// </summary>
   public class CatalogueLineBreakTests
   {
      private static readonly Regex Value_ = new(@"<value>(.*?)</value>", RegexOptions.Singleline);

      [Fact]
      public void NoCatalogue_EscapesACarriageReturnBeforeALineBreak()
      {
         string resources = Path.GetDirectoryName(ConfirmationPatternTests.ControlPanelFile(Path.Join("Resources", "Strings.resx")));
         var offenders = new List<string>();
         int catalogues = 0;
         foreach (string path in Directory.EnumerateFiles(resources, "Strings.*.resx"))
         {
            catalogues++;
            foreach (Match value in Value_.Matches(File.ReadAllText(path)))
            {
               if (Regex.IsMatch(value.Groups[1].Value, @"&#13;\r?\n"))
                  offenders.Add(Path.GetFileName(path) + ": " + value.Groups[1].Value.Split('\n')[0].Trim());
            }
         }

         Assert.Equal(17, catalogues);
         Assert.True(offenders.Count == 0,
            "A catalogue escapes a carriage return before a line break, which the build makes two breaks; write the break alone:\n" + string.Join("\n", offenders));
      }
   }
}
