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
   /// The Control Panel's English is British (the owner's decision of 9 October 2026:
   /// reinitialise, behaviour, honour), and one page said both: the Delivery subtitle's
   /// "behavior" beside "behaviour", "Honor recipient MTA-STS policies" beside "honour",
   /// "Organization name in TLS reports" beside "organisation", and Prioritize, finalization
   /// and authorization endpoint beside synchronisation (review finding F298). Every
   /// English text is a catalogue key, so the neutral catalogue is read for the American
   /// forms. A name that is not prose keeps its spelling: the HTTP header Authorization:
   /// and Azure's Artifact Signing, which are what the reader types or looks for.
   /// </summary>
   public class BritishSpellingTests
   {
      private static readonly Regex American = new Regex(
         @"\b(\w+iz(?:e|es|ed|ing|ation|ations|ational|er|ers)|\w+yz(?:e|es|ed|ing)|behaviors?|honor(?:s|ed|ing|able)?|colors?|colored|favou?rite|favor(?:s|ed|ing|able)?|centers?|centered|organizations?|catalogs?|canceled|canceling|labeled|labeling|modeled|traveled|traveling|gray|defense|artifacts?|enrollments?|fulfillment|skeptic\w*)\b",
         RegexOptions.IgnoreCase);

      // The -ize words that are spelt so in British English as well.
      private static readonly HashSet<string> SameInBoth = new HashSet<string>(StringComparer.OrdinalIgnoreCase)
      {
         "size", "sizes", "sized", "sizing", "oversize", "oversized", "resize", "resized", "resizing", "prize", "prizes", "seize", "seized", "capsize",
         "favourite",
      };

      [Fact]
      public void NoEnglishTextIsSpeltTheAmericanWay()
      {
         List<string> found = new List<string>();
         foreach (string key in ReadKeys())
         {
            string prose = key
               .Replace("Authorization:", "", StringComparison.Ordinal)
               .Replace("Azure Artifact Signing", "", StringComparison.Ordinal);
            foreach (Match match in American.Matches(WithoutAccessKey(prose)))
            {
               if (!SameInBoth.Contains(match.Value))
                  found.Add(match.Value + " in: " + key);
            }
         }

         Assert.True(found.Count == 0, "American spellings in the English texts:" + Environment.NewLine + string.Join(Environment.NewLine, found));
      }

      [Fact]
      public void ThePatternReadsTheWordsTheReviewFound()
      {
         foreach (string american in new[] { "behavior", "Honor", "Organization", "organizational", "Prioritize", "modernizes", "finalization", "finalizing", "authorization", "reinitialize", "canonicalization" })
            Assert.Matches(American, american);
         foreach (string british in new[] { "behaviour", "Honour", "Organisation", "organisational", "Prioritise", "modernises", "finalisation", "finalising", "authorisation", "reinitialise", "canonicalisation", "size" })
            Assert.True(!American.IsMatch(british) || SameInBoth.Contains(british), british);
      }

      private static string WithoutAccessKey(string text)
      {
         int at = text.IndexOf('_', StringComparison.Ordinal);
         return at < 0 ? text : text.Remove(at, 1);
      }

      private static IEnumerable<string> ReadKeys()
      {
         string path = ConfirmationPatternTests.ControlPanelFile(Path.Join("Resources", "Strings.resx"));
         return XDocument.Load(path).Root.Elements("data").Select(data => (string)data.Attribute("name")).ToList();
      }
   }
}
