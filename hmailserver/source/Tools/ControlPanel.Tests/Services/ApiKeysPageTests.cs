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
   /// The REST API keys page says what each key is. Each row's pill read a
   /// severity word - "Warning" for a key with full authority, "OK" for a
   /// read-only one, "Information" for an expired one - which said how much
   /// attention the key wanted and not what it was; the new key's line put the
   /// English "read-only" into every language; the source field's example carried
   /// the English "or" into German and Japanese under a plural caption for a
   /// field that takes one value; and an empty store was said twice.
   ///
   /// Scanned rather than rendered: this assembly has no WPF.
   /// </summary>
   public class ApiKeysPageTests
   {
      [Theory]
      [InlineData("Ignored", "StatusLevel.Warning")]
      [InlineData("Full authority", "StatusLevel.Warning")]
      [InlineData("Read-only", "StatusLevel.Good")]
      public void EachKeysPill_NamesItsState_AtItsLevel(string word, string level)
      {
         string row = KeyRow();
         Match pill = Regex.Match(row, @"level = (StatusLevel\.\w+);\s*pill = L\(""" + Regex.Escape(word) + @"""\);");
         Assert.True(pill.Success, "No key's pill reads \"" + word + "\".");
         Assert.Equal(level, pill.Groups[1].Value);
      }

      /// <summary>
      /// The sentence beside a pill says what the state means for the key and
      /// does not name the state again: with the state in the pill, "Read-only"
      /// stood beside "Read-only. Expires ...", "Full authority" beside "Full
      /// authority - this key can change...", "Expired" beside "Expired on ..."
      /// and "Ignored" beside "Ignored by the server - ...", each said twice to
      /// the eye and to a screen reader, which reads the pill and then the
      /// sentence.
      /// </summary>
      [Fact]
      public void TheSentenceBesideAPill_DoesNotNameAnyPillsStateAgain()
      {
         string row = KeyRow();
         var words = Regex.Matches(row, @"pill = [^;]*;").Cast<Match>()
            .SelectMany(pill => Regex.Matches(pill.Value, @"L\(""([^""]+)""\)").Cast<Match>().Select(word => word.Groups[1].Value))
            .ToList();
         Assert.Equal(new[] { "Ignored", "Expiry unreadable", "Expired", "Full authority", "Read-only" }, words);

         var sentences = Regex.Matches(row, @"state = (.*?);\s*(?://[^\n]*)?\r?\n", RegexOptions.Singleline).Cast<Match>()
            .SelectMany(state => Regex.Matches(state.Groups[1].Value, @"[LF]\(""((?:[^""\\]|\\.)*)""").Cast<Match>().Select(text => text.Groups[1].Value))
            .ToList();
         Assert.Equal(5, sentences.Count);
         foreach (string sentence in sentences)
         {
            foreach (string word in words)
               Assert.False(sentence.Contains(word, StringComparison.OrdinalIgnoreCase), "\"" + sentence + "\" names the state \"" + word + "\" its pill says.");
         }

         Assert.Contains("state = F(\"Expires {0}.\"", row, StringComparison.Ordinal);
         Assert.Contains("state = F(\"This key can change and delete things. Expires {0}.\"", row, StringComparison.Ordinal);
      }

      /// <summary>
      /// The sentence beside "Expiry unreadable" starts from the reason - the one
      /// form of expiry this page reads - and does not say again that the expiry
      /// could not be read: it read "The stored expiry could not be read in the
      /// form this page expects (YYYY-MM-DD HH:MM:SS), ...", German "Der
      /// gespeicherte Ablauf konnte nicht in der Form gelesen werden, ...", the
      /// pill's state in other words, which a test of the pill's own words beside
      /// it did not see. No catalogue keeps the old sentence.
      /// </summary>
      [Fact]
      public void TheSentenceBesideExpiryUnreadable_StartsFromTheReason_AndDoesNotSayTheExpiryCouldNotBeRead()
      {
         Match unreadable = Regex.Match(KeyRow(), @"state = key\.ExpiresAt == null\s*\?\s*L\(""((?:[^""\\]|\\.)*)""\)");
         Assert.True(unreadable.Success, "The sentence beside Expiry unreadable is gone.");
         string sentence = unreadable.Groups[1].Value;
         Assert.StartsWith("This page reads an expiry only in the form YYYY-MM-DD HH:MM:SS, so ", sentence, StringComparison.Ordinal);
         foreach (string again in new[] { "could not be read", "cannot be read", "unreadable" })
            Assert.False(sentence.Contains(again, StringComparison.OrdinalIgnoreCase), "\"" + sentence + "\" says again that the expiry could not be read (\"" + again + "\").");

         string resources = ResourcesDirectory();
         foreach (string path in Directory.GetFiles(resources, "Strings*.resx"))
         {
            Dictionary<string, string> catalogue = ReadResx(path);
            Assert.True(catalogue.TryGetValue(sentence, out string value) && value.Length > 0, Path.GetFileName(path) + " has no translation of the sentence beside Expiry unreadable.");
            Assert.False(catalogue.Keys.Any(k => k.StartsWith("The stored expiry could not be read", StringComparison.Ordinal)), Path.GetFileName(path) + " still holds the old sentence.");
         }

         string german = ReadResx(Path.Join(resources, "Strings.de.resx"))[sentence];
         Assert.False(german.StartsWith("Der gespeicherte Ablauf", StringComparison.Ordinal), "The German sentence says again that the expiry could not be read: " + german);
         Assert.DoesNotContain("nicht gelesen", german, StringComparison.Ordinal);
      }

      [Fact]
      public void AnExpiredKey_IsNormal_AndOneWhoseExpiryCannotBeRead_IsAWarning()
      {
         string row = KeyRow();
         Assert.Contains("level = key.ExpiresAt == null ? StatusLevel.Warning : StatusLevel.Normal;", row, StringComparison.Ordinal);
         Assert.Contains("pill = key.ExpiresAt == null ? L(\"Expiry unreadable\") : L(\"Expired\");", row, StringComparison.Ordinal);
         Assert.Contains("Text = pill,", row, StringComparison.Ordinal);
         Assert.DoesNotContain("SeverityWord", row, StringComparison.Ordinal);
      }

      [Fact]
      public void TheDialogsWords_AreTranslated_AndTheSourceIsOneValue()
      {
         string code = Source();
         Assert.Contains("L(\"Full authority\") : L(\"Read-only\")", code, StringComparison.Ordinal);
         Assert.DoesNotContain(": \"read-only\"", code, StringComparison.Ordinal);
         Assert.Contains("PlaceholderText = L(\"For example 10.0.0.5 or 10.0.0.0/24\")", code, StringComparison.Ordinal);
         Assert.Contains("L(\"_Source address or range it may be used from (optional)\")", code, StringComparison.Ordinal);
         Assert.DoesNotContain("10.0.0.5, or 10.0.0.0/24", code, StringComparison.Ordinal);
      }

      [Fact]
      public void AnEmptyStore_IsSaidOnce_ByTheListsEmptyState_WithItsAction()
      {
         string code = Source();
         Assert.DoesNotContain("No API keys exist.", code, StringComparison.Ordinal);
         Assert.Contains("empty_.Action = first;", code, StringComparison.Ordinal);
         Assert.Contains("summary_.Visibility = keys.Count == 0 ? Visibility.Collapsed : Visibility.Visible;", code, StringComparison.Ordinal);
      }

      private static string KeyRow()
      {
         string row = WithheldRowTests.MemberText(Source(), "private FrameworkElement BuildKeyRow_(ApiKeyRecord key)");
         Assert.True(row != null, "ApiKeysView.BuildKeyRow_ is gone.");
         return row;
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
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", "Resources");
            if (File.Exists(Path.Join(candidate, "Strings.resx")))
               return candidate;
            directory = directory.Parent;
         }

         Assert.Fail("ControlPanel/Resources could not be found from the test binaries.");
         return null;
      }

      private static string Source()
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", "Views", "ApiKeysView.cs");
            if (File.Exists(candidate))
               return File.ReadAllText(candidate);
            directory = directory.Parent;
         }

         Assert.Fail("ControlPanel/Views/ApiKeysView.cs could not be found from the test binaries.");
         return null;
      }
   }
}
