// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using System.Xml.Linq;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The Domains & accounts page's words for its acts (review findings F082 and
   /// F083). Domain and distribution list rows said Properties where account rows
   /// and Groups said Edit for the same open-the-editor act, and every row's button
   /// was "Edit" or "Properties" to a screen reader whatever row it was on; the
   /// Members button opened a dialog headed "Recipients of ..." whose box, question
   /// and outcomes said recipient. Every test here fails against that page.
   /// </summary>
   public class DomainsVerbWordsTests
   {
      private static readonly XNamespace Ui = "http://schemas.lepo.co/wpfui/2022/xaml";

      [Fact]
      public void EveryRowThatOpensAnEditor_SaysEdit_AndNamesItsRow()
      {
         string xaml = ReadControlPanelFile(Path.Join("Views", "DomainsView.xaml"));
         Assert.DoesNotContain("{loc:L 'Properties'}", xaml, StringComparison.Ordinal);
         Assert.DoesNotContain("PropertiesName", xaml, StringComparison.Ordinal);

         XElement page = XDocument.Parse(xaml).Root;
         foreach (string handler in new[] { "EditDomain_Click", "EditAccount_Click", "EditDistList_Click" })
         {
            XElement button = page.Descendants(Ui + "Button").Single(b => (string)b.Attribute("Click") == handler);
            Assert.Equal("{Binding EditName}", (string)button.Attribute("AutomationProperties.Name"));
            if (handler == "EditDistList_Click")
            {
               // The list row's glyph, the pencil of Edit where it was Properties' cog.
               Assert.Equal("{ui:SymbolIcon Edit24}", (string)button.Attribute("Icon"));
               Assert.Equal("{Binding EditName}", (string)button.Attribute("ToolTip"));
            }
            else
               Assert.Equal("{loc:L 'Edit'}", (string)button.Attribute("Content"));
         }

         string code = ReadControlPanelFile(Path.Join("Views", "DomainsView.xaml.cs"));
         Assert.Contains("public string EditName => F(\"Edit domain {0}\", Name);", code, StringComparison.Ordinal);
         Assert.Contains("public string EditName => F(\"Edit account {0}\", Address);", code, StringComparison.Ordinal);
         Assert.Contains("public string EditName => F(\"Edit distribution list {0}\", Address);", code, StringComparison.Ordinal);

         string rows = ReadControlPanelFile(Path.Join("Views", "DomainsView.Rows.cs"));
         Assert.DoesNotContain("_Properties…", rows, StringComparison.Ordinal);
         Assert.Equal(3, Regex.Matches(rows, @"WireList_\(\w+, L\(""_Edit…""\)").Count);
      }

      /// <summary>
      /// A text button's accessible name starts with its words in every language
      /// (WCAG 2.5.3, Label in Name): "Edit domain example.com" under the button
      /// Edit, "Bearbeiten: Domain example.com" under Bearbeiten, so that voice
      /// control's "click Bearbeiten" reaches it.
      /// </summary>
      [Fact]
      public void InEveryLanguage_TheRowsName_StartsWithTheButtonsWord()
      {
         var failures = new List<string>();
         foreach (string path in Directory.GetFiles(ResourcesDirectory(), "Strings.*.resx"))
         {
            Dictionary<string, string> catalogue = ReadResx(path);
            string tag = Path.GetFileNameWithoutExtension(path).Substring("Strings.".Length);
            string edit = LabelInName.Shown(catalogue["Edit"]);
            foreach (string key in new[] { "Edit domain {0}", "Edit account {0}" })
            {
               if (!catalogue.TryGetValue(key, out string name) || !name.StartsWith(edit, StringComparison.CurrentCultureIgnoreCase))
                  failures.Add(tag + ": " + key + " = " + (name ?? "<missing>") + ", under " + edit);
            }
         }

         Assert.True(failures.Count == 0, string.Join(Environment.NewLine, failures));
      }

      /// <summary>
      /// Members… keeps its own Alt key beside the verbs it is shown with: on the
      /// distribution list rows' menu beside Edit…, which the menu says now where
      /// it said Properties…, and on the Groups page beside the list's Add, Edit,
      /// Delete and Refresh. French and Italian put Members and Modifier or
      /// Modifica on M, and Russian Участники and Удалить on У.
      /// </summary>
      [Fact]
      public void InEveryLanguage_MembersKeepsAKeyOfItsOwn_BesideTheVerbsShownWithIt()
      {
         var scopes = new[]
         {
            new[] { "_Edit…", "_Members…", "Delete _list" },
            new[] { "_Add", "_Edit", "_Delete", "_Refresh", "Add the _first entry…", "_Members…" }
         };
         var failures = new List<string>();
         foreach (string path in Directory.GetFiles(ResourcesDirectory(), "Strings.*.resx"))
         {
            Dictionary<string, string> catalogue = ReadResx(path);
            string tag = Path.GetFileNameWithoutExtension(path).Substring("Strings.".Length);
            foreach (string[] scope in scopes)
            {
               List<string> keys = scope.Select(caption => MnemonicText.Key(catalogue[caption])).ToList();
               if (keys.Distinct(StringComparer.Ordinal).Count() != keys.Count)
                  failures.Add(tag + ": " + string.Join(", ", scope.Select(caption => catalogue[caption])));
            }
         }

         Assert.True(failures.Count == 0, string.Join(Environment.NewLine, failures));
      }

      /// <summary>The members dialog says member, as the button that opens it does.</summary>
      [Fact]
      public void TheMembersDialog_SaysMember()
      {
         string dialog = ReadControlPanelFile(Path.Join("Views", "RecipientsDialog.cs"));
         List<string> marked = Regex.Matches(dialog, @"(?<![\w.])(?:[LF]|Plural)\(\s*(?:added,\s*)?""((?:[^""\\]|\\.)*)""")
            .Select(m => m.Groups[1].Value)
            .Where(text => text.Contains("ecipient", StringComparison.Ordinal))
            .ToList();
         Assert.True(marked.Count == 0, "The members dialog still says recipient: " + string.Join(" | ", marked));

         Assert.Contains("Title = F(\"Members - {0}\", listAddress);", dialog, StringComparison.Ordinal);
         Assert.Contains("UseFrame(F(\"Members of {0}\", listAddress), body, null, closeButton, width: 520);", dialog, StringComparison.Ordinal);
         Assert.Contains("DialogFields.Caption(L(\"Address to add as a member\"))", dialog, StringComparison.Ordinal);
         Assert.Contains("F(\"Remove the member {0} from this list?\", address)", dialog, StringComparison.Ordinal);
      }

      /// <summary>
      /// The members box's caption names the box in a noun phrase, as the dialogs'
      /// captions do: Russian, Ukrainian and Czech said it in a relative clause on a
      /// bare infinitive - "Адрес, который добавить как участника", "Адреса, яку
      /// додати як учасника", "Adresa, kterou přidat jako člena" - which none of the
      /// three writes, where the caption it replaced read "Адрес электронной почты
      /// для добавления в список".
      /// </summary>
      [Theory]
      [InlineData("ru", "Адрес нового участника")]
      [InlineData("uk", "Адреса нового учасника")]
      [InlineData("cs", "Adresa nového člena")]
      public void TheMembersBoxCaption_IsANounPhrase(string tag, string expected)
      {
         Dictionary<string, string> catalogue = ReadResx(Path.Join(ResourcesDirectory(), "Strings." + tag + ".resx"));
         Assert.Equal(expected, catalogue["Address to add as a member"]);
      }

      /// <summary>
      /// The page's own help says the verbs the page shows: DomainsAndAccounts.md,
      /// which the help pane opens for the page and the server embeds, still told its
      /// reader to press Properties on a domain's row and the gear on a list's once
      /// the rows said Edit and the list's glyph was a pencil.
      /// </summary>
      [Fact]
      public void ThePagesHelp_SaysTheRowsVerbs()
      {
         HelpTarget target = HelpMap.For("domains");
         Assert.Equal("DomainsAndAccounts.md", target.Doc);
         string text = Regex.Replace(File.ReadAllText(Path.Join(DocsDirectory(), target.Doc)), @"\s+", " ");
         Assert.Contains("one that opens its dialog - *Edit* on a domain or an account, the pencil on a list, beside the list's *Members*", text, StringComparison.Ordinal);
         Assert.Contains("The list's dialog (the pencil on its row)", text, StringComparison.Ordinal);
         Assert.Contains("*Edit* on a domain's row.", text, StringComparison.Ordinal);
         Assert.DoesNotContain("*Properties*", text, StringComparison.Ordinal);
         Assert.DoesNotMatch(new Regex(@"\b(?:gear|cog)\b", RegexOptions.IgnoreCase), text);
      }

      private static string DocsDirectory()
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", "App.xaml.cs");
            if (File.Exists(candidate))
               return Path.GetFullPath(Path.Join(directory.FullName, "ControlPanel", "..", "..", "..", "docs"));
            directory = directory.Parent;
         }

         Assert.Fail("ControlPanel/App.xaml.cs could not be found from the test binaries.");
         return null;
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

         Assert.Fail("ControlPanel/Resources could not be found from the test binaries.");
         return null;
      }

      private static string ReadControlPanelFile(string relative)
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", relative);
            if (File.Exists(candidate))
               return File.ReadAllText(candidate);
            directory = directory.Parent;
         }

         Assert.Fail("ControlPanel/" + relative.Replace('\\', '/') + " could not be found from the test binaries.");
         return null;
      }
   }
}
