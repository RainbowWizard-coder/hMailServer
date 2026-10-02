// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Xml.Linq;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// A chart's X axis title (Services/ChartWindowWords.cs). Until 6.3.6 one of a unit
   /// was "last 1 seconds" - a number put into the plural - in English and in every
   /// language whose catalogue writes the plural out.
   /// </summary>
   [Collection("Loc")]
   public class ChartWindowWordsTests : IDisposable
   {
      public void Dispose() => Loc.Reset();

      [Theory]
      [InlineData(0.4, "last second")]
      [InlineData(1.0, "last second")]
      [InlineData(1.49, "last second")]
      [InlineData(1.5, "last 2 seconds")]
      [InlineData(30.0, "last 30 seconds")]
      [InlineData(89.0, "last 89 seconds")]
      [InlineData(90.0, "last 2 minutes")]
      [InlineData(45 * 60.0, "last 45 minutes")]
      [InlineData(90 * 60.0, "last 2 hours")]
      [InlineData(26 * 3600.0, "last 26 hours")]
      public void OneOfAUnit_IsASentenceOfItsOwn(double seconds, string expected)
      {
         Loc.Use((text, culture) => null, CultureInfo.GetCultureInfo("en-GB"));
         Assert.Equal(expected, ChartWindowWords.Of(TimeSpan.FromSeconds(seconds)));
      }

      /// <summary>No span gives a count of one in the plural, and none a count of none.</summary>
      [Fact]
      public void NoSpan_SaysOneInThePlural()
      {
         Loc.Use((text, culture) => null, CultureInfo.GetCultureInfo("en-GB"));
         for (double seconds = 0; seconds < 30 * 3600; seconds += seconds < 200 ? 0.25 : seconds < 10000 ? 7 : 301)
         {
            string words = ChartWindowWords.Of(TimeSpan.FromSeconds(seconds));
            Assert.DoesNotContain("last 1 ", words);
            Assert.DoesNotContain("last 0 ", words);
         }
      }

      /// <summary>The one-of-a-unit sentences are in every catalogue, each a whole sentence with no number to fill.</summary>
      [Fact]
      public void EveryLanguage_HasOneOfEachUnit()
      {
         string resources = ResourcesDirectory();
         Assert.True(resources != null, "ControlPanel/Resources could not be found from the test binaries.");
         string[] catalogues = Directory.GetFiles(resources, "Strings.*.resx");
         Assert.True(catalogues.Length >= 17, "Expected seventeen languages, found " + catalogues.Length + ".");
         foreach (string path in catalogues)
         {
            Dictionary<string, string> strings = XDocument.Load(path).Root.Elements("data")
               .ToDictionary(d => (string)d.Attribute("name"), d => (string)d.Element("value") ?? "");
            string name = Path.GetFileName(path);
            foreach ((string one, string many) in new[] { ("last second", "last {0} seconds"), ("last minute", "last {0} minutes"), ("last hour", "last {0} hours") })
            {
               Assert.True(strings.TryGetValue(one, out string word) && word.Trim().Length > 0 && !word.Contains("{0}"), name + " lacks \"" + one + "\", or gives it a number to fill.");
               Assert.True(strings.TryGetValue(many, out string plural) && plural.Contains("{0}"), name + " lacks \"" + many + "\".");
            }
         }
      }

      private static string ResourcesDirectory()
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", "Resources");
            if (Directory.Exists(candidate))
               return candidate;
            directory = directory.Parent;
         }

         return null;
      }
   }
}
