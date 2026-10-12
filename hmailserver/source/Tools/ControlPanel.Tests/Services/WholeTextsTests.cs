// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using System.Xml.Linq;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// Whole texts in place of glued fragments (review finding F283). The
   /// Control Panel built titles and sentences by putting a translated piece
   /// before a value or another piece - L("MX — which server takes the mail
   /// for ") + domain, L("Account - ") + address, F("Add {0}", noun), a
   /// sentence added as " It is safe to enable signing." - so German read
   /// "... die Mail annimmt für example.com", Japanese "SURBL サーバー を追加"
   /// and a space after its 。. Each is one catalogued text now, with the value
   /// as {0}, and sentences are joined by Loc.Sentences.
   /// </summary>
   [Collection("Loc")]
   public class WholeTextsTests : IDisposable
   {
      public void Dispose() => Loc.Reset();

      [Fact]
      public void SentencesAreJoinedWithOneSpaceAndTheMissingOnesLeftOut()
      {
         Assert.Equal("One. Two. Three.", Loc.Sentences("One.", null, " Two. ", "", "   ", "Three."));
         Assert.Equal("Only.", Loc.Sentences(null, "Only."));
         Assert.Equal("", Loc.Sentences());
         Assert.Equal("", Loc.Sentences(null, null));
      }

      [Fact]
      public void ASentenceThatEndsInAFullWidthStopTakesNoSpaceAfterIt()
      {
         Assert.Equal("鍵を読み取れませんでした。署名を有効にしても安全です。",
            Loc.Sentences("鍵を読み取れませんでした。", "署名を有効にしても安全です。"));
         Assert.Equal("现状如下：已索引 5 封。", Loc.Sentences("现状如下：", "已索引 5 封。"));

         // A Latin stop inside Japanese text still takes its space.
         Assert.Equal("Last update: ok. これは最新のリリースです",
            Loc.Sentences("Last update: ok.", "これは最新のリリースです"));
      }

      [Fact]
      public void NamesOfWhichAnyWillDoAreJoinedByTheCataloguesWholeOr()
      {
         Assert.Equal("", Loc.JoinOr(Array.Empty<string>()));
         Assert.Equal("RS384", Loc.JoinOr(new[] { "RS384" }));
         Assert.Equal("RS384 or PS256", Loc.JoinOr(new[] { "RS384", "PS256" }));
         Assert.Equal("RS384 or PS256 or HS512", Loc.JoinOr(new[] { "RS384", "PS256", "HS512" }));

         Dictionary<string, string> japanese = Catalogue("ja");
         Loc.Use((text, culture) => japanese.TryGetValue(text, out string value) ? value : null, CultureInfo.GetCultureInfo("ja-JP"));
         Assert.Equal(japanese["{0} or {1}"].Replace("{0}", "RS384").Replace("{1}", "PS256"), Loc.JoinOr(new[] { "RS384", "PS256" }));
      }

      [Fact]
      public void TheUpdateRowIsOneTemplateForEachStateWithItsDate()
      {
         Dictionary<string, string> japanese = Catalogue("ja");
         Loc.Use((text, culture) => japanese.TryGetValue(text, out string value) ? value : null, CultureInfo.GetCultureInfo("ja-JP"));

         string available = UpdateStatusText.For(2, "6.3.7", "2026-10-19", "", "");
         Assert.Equal(string.Format(CultureInfo.InvariantCulture, japanese["{0} is available (published {1})"], "6.3.7", ServerTimeText.Date(new DateTime(2026, 10, 19))), available);

         // The last outcome is a sentence of its own: no space after its 。.
         string latest = UpdateStatusText.For(1, "", "", "", "", "ok");
         string outcome = string.Format(CultureInfo.InvariantCulture, japanese["Last update: {0}."], "ok");
         Assert.EndsWith("。", outcome);
         Assert.Equal(outcome + japanese["This is the latest release"], latest);
      }

      [Fact]
      public void NoCataloguedTextHasWhiteSpaceAtAnEdgeButTheColonOfALabel()
      {
         // build/check-localisation.py refuses these in the source; this holds
         // the catalogue the Control Panel ships to the same rule.
         var glued = Catalogue(null).Keys
            .Where(key => key != key.TrimStart() || (key != key.TrimEnd() && !key.EndsWith(": ", StringComparison.Ordinal)))
            .ToList();
         Assert.True(glued.Count == 0, "Texts glued at an edge: " + string.Join(" | ", glued.Select(k => "'" + k + "'")));
      }

      [Fact]
      public void NoCataloguedTextIsAPieceOfADialogTitle()
      {
         Dictionary<string, string> english = Catalogue(null);
         foreach (string piece in new[] { "Add {0}", "Edit {0}", "Account - ", "Domain - ", "Route - ", "Members - ", " or " })
            Assert.False(english.ContainsKey(piece), "The catalogue still holds '" + piece + "'.");

         foreach (string whole in new[] { "Account - {0}", "Domain - {0}", "Route - {0}", "MX - which server takes the mail for {0}", "{0} or {1}" })
            Assert.True(english.ContainsKey(whole), "The catalogue lacks '" + whole + "'.");
      }

      [Fact]
      public void EveryCollectionSpecTitlesItsDialogsWithWholeTexts()
      {
         string specs = ReadSource("CollectionSpecs.cs");
         List<string> blocks = Regex.Split(specs, @"new\s+CollectionSpec\b").Skip(1).ToList();
         Assert.True(blocks.Count >= 14, "Fewer specs than expected: " + blocks.Count + ".");

         foreach (string block in blocks)
         {
            string head = block.Substring(0, Math.Min(block.Length, 2500));
            string title = Regex.Match(head, "Title = L\\(\"([^\"]+)\"\\)").Groups[1].Value;
            Assert.Matches("EditTitle = N\\(\"Edit [^\"]+\"\\)", head);
            if (!head.Contains("CanAdd = false", StringComparison.Ordinal))
               Assert.True(Regex.IsMatch(head, "AddTitle = N\\(\"Add [^\"]+\"\\)"), title + " has no AddTitle.");
         }

         string dialog = ReadSource("FieldDialog.cs");
         Assert.Contains("Title = L(existing == null ? spec.AddTitle : spec.EditTitle);", dialog);
         Assert.DoesNotContain("F(\"Add {0}\"", dialog);
      }

      [Fact]
      public void EveryLanguageTranslatesTheDialogTitles()
      {
         string specs = ReadSource("CollectionSpecs.cs");
         List<string> titles = Regex.Matches(specs, "(?:Add|Edit)Title = N\\(\"([^\"]+)\"\\)").Select(m => m.Groups[1].Value).Distinct().ToList();
         Assert.NotEmpty(titles);

         var missing = new List<string>();
         foreach (string path in Directory.GetFiles(ResourcesDirectory(), "Strings.*.resx"))
         {
            Dictionary<string, string> catalogue = ReadResx(path);
            foreach (string title in titles)
            {
               if (!catalogue.TryGetValue(title, out string value) || string.IsNullOrWhiteSpace(value))
                  missing.Add(Path.GetFileName(path) + ": " + title);
            }
         }

         Assert.True(missing.Count == 0, string.Join(Environment.NewLine, missing));
      }

      /// <summary>
      /// The DNS records page's record type and its name's caption are whole
      /// texts - "Record type: MX", then "Name - {0}" with its note - where it
      /// was L("Record type: MX. Host/Name") + " — " + note + ":", whose two
      /// spaces after the stop Japanese and Chinese copied as "MX。  ホスト/名前",
      /// and then one text that said both (review finding F306); and no
      /// Japanese or Chinese text has a space after its 。, as the domain
      /// dialog's restriction hint had inside one paragraph (review finding
      /// F283, second pass).
      /// </summary>
      [Fact]
      public void NoJapaneseOrChineseTextHasASpaceAfterItsFullStop()
      {
         Dictionary<string, string> english = Catalogue(null);
         foreach (string whole in new[] { "Record type: MX", "Record type: TXT", "Name - {0}" })
            Assert.True(english.ContainsKey(whole), "The catalogue lacks '" + whole + "'.");
         Assert.DoesNotContain(english.Keys, key => key.StartsWith("Record type: ", StringComparison.Ordinal) && key.Contains(". ", StringComparison.Ordinal));

         foreach (string tag in new[] { "ja", "zh-Hans" })
         {
            List<string> spaced = Catalogue(tag)
               .Where(pair => pair.Value.Contains("。 ", StringComparison.Ordinal))
               .Select(pair => tag + ": " + pair.Value)
               .ToList();
            Assert.True(spaced.Count == 0, "A space after 。: " + string.Join(" | ", spaced));
         }
      }

      /// <summary>
      /// A sentence the install refusal adds to its first is joined by
      /// Sentences, so that in Japanese it follows the first sentence's 。 with
      /// no space, where text += " " + F(...) put one there.
      /// </summary>
      [Fact]
      public void ASentenceAddedToARefusalTakesNoSpaceAfterAJapaneseStop()
      {
         Dictionary<string, string> japanese = Catalogue("ja");
         Loc.Use((text, culture) => japanese.TryGetValue(text, out string value) ? value : null, CultureInfo.GetCultureInfo("ja-JP"));
         var finding = new InstallationPermissions.Finding
         {
            Failed = true,
            Object = "D:\\hMailServer",
            Principal = "NT AUTHORITY\\Authenticated Users",
            Right = "write data / create files",
            Inherited = true,
            InheritedFrom = "D:\\",
            Fix = "icacls \"D:\\hMailServer\" /inheritance:d"
         };

         string refusal = AdministratorUpdate.PermissionText(finding);

         string inherited = string.Format(CultureInfo.InvariantCulture, japanese["The permission is inherited from {0}; the command stops the folder inheriting it."], "D:\\");
         Assert.EndsWith("。" + inherited, refusal, StringComparison.Ordinal);
         Assert.DoesNotContain("。 ", refusal, StringComparison.Ordinal);
      }

      private static Dictionary<string, string> Catalogue(string tag)
         => ReadResx(Path.Join(ResourcesDirectory(), tag == null ? "Strings.resx" : "Strings." + tag + ".resx"));

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
