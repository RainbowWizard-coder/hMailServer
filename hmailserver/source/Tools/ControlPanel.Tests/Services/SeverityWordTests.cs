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
   /// The severity word a notice and a pill put before their sentence is in the
   /// language in force when it is drawn (review finding F006). StatusSemantics
   /// made its five presentations once, in a static initialiser, with the word
   /// translated there: whatever touched the type first fixed the language of
   /// every "Warning" for the life of the process, and the render harness, which
   /// touched it before it put German in force, showed "Warning" before every
   /// German sentence. And Japanese gave "Critical", "Normal" and the rule card's
   /// "THEN" back in English (F114, F177).
   /// </summary>
   [Collection("Loc")]
   public class SeverityWordTests : IDisposable
   {
      public void Dispose() => Loc.Reset();

      [Fact]
      public void TheWordIsTranslatedWhenItIsRead_NotWhenTheTypeWasFirstTouched()
      {
         // Touched under English first, as the harness did.
         Loc.Reset();
         Assert.Equal("Warning", StatusSemantics.For(StatusLevel.Warning).SeverityWord);
         Assert.Equal("Critical", StatusSemantics.For(StatusLevel.Critical).SeverityWord);

         var german = new Dictionary<string, string>
         {
            ["Warning"] = "Warnung",
            ["Critical"] = "Kritisch",
            ["Information"] = "Information",
            ["OK"] = "OK",
         };
         Loc.Use((text, culture) => german.TryGetValue(text, out string value) ? value : null, CultureInfo.GetCultureInfo("de"));

         Assert.Equal("Warnung", StatusSemantics.For(StatusLevel.Warning).SeverityWord);
         Assert.Equal("Kritisch", StatusSemantics.For(StatusLevel.Critical).SeverityWord);

         // And back: nothing of the German stays behind.
         Loc.Reset();
         Assert.Equal("Warning", StatusSemantics.For(StatusLevel.Warning).SeverityWord);
      }

      [Fact]
      public void TheEnglishWordIsTheKey_WhateverTheLanguage()
      {
         Loc.Use((text, culture) => "übersetzt", CultureInfo.GetCultureInfo("de"));

         Assert.Equal("Warning", StatusSemantics.For(StatusLevel.Warning).SeverityWordEnglish);
         Assert.Equal("OK", StatusSemantics.For(StatusLevel.Good).SeverityWordEnglish);
         Assert.Equal("übersetzt", StatusSemantics.For(StatusLevel.Good).SeverityWord);
      }

      /// <summary>
      /// Every catalogue translates the words a notice shows - Critical, Warning,
      /// Information - and a catalogue in another script gives none of them back
      /// in English.
      /// </summary>
      [Fact]
      public void EveryCatalogueTranslatesTheShownSeverityWords()
      {
         string resources = ResourcesDirectory();
         if (resources == null)
            return;   // sources not available (a packaged drop)

         var otherScripts = new HashSet<string> { "ja", "zh-Hans", "ru", "uk" };
         var problems = new List<string>();
         foreach (string path in Directory.GetFiles(resources, "Strings.*.resx"))
         {
            string tag = Path.GetFileNameWithoutExtension(path).Substring("Strings.".Length);
            Dictionary<string, string> catalogue = ReadResx(path);
            foreach (StatusLevel level in new[] { StatusLevel.Critical, StatusLevel.Warning, StatusLevel.Information, StatusLevel.Normal })
            {
               string english = StatusSemantics.For(level).SeverityWordEnglish;
               catalogue.TryGetValue(english, out string value);
               if (string.IsNullOrWhiteSpace(value))
                  problems.Add(tag + ": no translation of " + english);
               else if (otherScripts.Contains(tag) && value == english)
                  problems.Add(tag + ": " + english + " is left in English");
            }
         }

         Assert.True(problems.Count == 0, string.Join(Environment.NewLine, problems));
      }

      private static Dictionary<string, string> ReadResx(string path)
      {
         var result = new Dictionary<string, string>(StringComparer.Ordinal);
         foreach (XElement data in XDocument.Load(path).Root.Elements("data"))
            result[(string)data.Attribute("name")] = data.Element("value")?.Value ?? "";
         return result;
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
         return null;
      }
   }
}
