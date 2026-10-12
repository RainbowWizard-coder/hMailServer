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
   /// The Administrator roles page in the reader's language and in the words of the
   /// rest of the Control Panel (review findings F084 to F087): the built-in roles'
   /// purposes were the English hm_roles holds among translated captions; the
   /// subtitle ran three lines and the empty state said its last sentence again;
   /// Built in was Yes or nothing; an assignment's domains read
   /// "example.com,example.net"; and the form's placeholders said the captions above
   /// them again while nothing said what an empty Domains box gives.
   /// </summary>
   public class RoleWordsTests : IDisposable
   {
      public void Dispose() => Loc.Reset();

      /// <summary>The page's English is the scripts' own description of each built-in role, word for word.</summary>
      [Theory]
      [InlineData("CreateTablesMSSQL.sql")]
      [InlineData("CreateTablesMySQL.sql")]
      [InlineData("CreateTablesPGSQL.sql")]
      public void EachBuiltInPurpose_IsTheDescriptionTheDatabaseScriptsWrite(string script)
      {
         Loc.Reset();
         string sql = File.ReadAllText(Path.Join(DbScripts(), script));
         foreach (string role in RoleWords.BuiltInNames)
         {
            Match row = Regex.Match(sql, @"values\s*\(\s*'" + role + @"'\s*,\s*'((?:[^']|'')*)'\s*,\s*1\s*,", RegexOptions.IgnoreCase);
            Assert.True(row.Success, script + " has no hm_roles row for " + role + ".");
            Assert.Equal(row.Groups[1].Value.Replace("''", "'"), RoleWords.Purpose(role, true, "ignored"));
         }

         Assert.Equal(new[] { "auditor", "domainadmin", "helpdesk", "serveradmin" }, RoleWords.BuiltInNames.OrderBy(n => n, StringComparer.Ordinal));
      }

      [Fact]
      public void ABuiltInRolesPurpose_IsInTheReadersLanguage_AndACustomRolesAsTyped()
      {
         Dictionary<string, string> german = ReadResx("Strings.de.resx");
         Loc.Use((text, culture) => german.TryGetValue(text, out string value) && value.Length > 0 ? value : null, CultureInfo.GetCultureInfo("de"));

         Assert.Equal("Jede Berechtigung, in jeder Domain.", RoleWords.Purpose("serveradmin", true, "Every permission, in every domain."));
         Assert.Equal("Jede Berechtigung, in jeder Domain.", RoleWords.Purpose("ServerAdmin", true, "Every permission, in every domain."));
         Assert.Equal("Open and export discovery cases.", RoleWords.Purpose("compliance", false, "Open and export discovery cases."));
         // A role the server does not call built in keeps its own words, whatever its name.
         Assert.Equal("Our own help desk.", RoleWords.Purpose("helpdesk", false, "Our own help desk."));
         Assert.Equal("", RoleWords.Purpose("compliance", false, null));
      }

      /// <summary>Every language translates the four purposes, so that none reads English among its own captions.</summary>
      [Fact]
      public void EveryCatalogue_TranslatesTheFourPurposes()
      {
         Loc.Reset();
         var failures = new List<string>();
         foreach (string path in Directory.GetFiles(ResourcesDirectory(), "Strings.*.resx"))
         {
            Dictionary<string, string> catalogue = ReadResx(Path.GetFileName(path));
            foreach (string role in RoleWords.BuiltInNames)
            {
               string english = RoleWords.Purpose(role, true, "");
               if (!catalogue.TryGetValue(english, out string value) || value.Length == 0 || value == english)
                  failures.Add(Path.GetFileName(path) + ": " + english);
            }
         }

         Assert.True(failures.Count == 0, string.Join(Environment.NewLine, failures));
      }

      [Theory]
      [InlineData("example.com,example.net", "example.com, example.net")]
      [InlineData("example.com", "example.com")]
      [InlineData(" example.com ,example.net,, ", "example.com, example.net")]
      [InlineData("", "")]
      [InlineData(null, "")]
      public void AnAssignmentsDomains_ReadAsAList(string listed, string shown)
         => Assert.Equal(shown, RoleWords.DomainsText(listed));

      [Theory]
      [InlineData(" example.com , example.net, ", "example.com,example.net")]
      [InlineData("example.com, example.net", "example.com,example.net")]
      [InlineData("*", "*")]
      [InlineData("  ", "")]
      [InlineData(null, "")]
      public void TheTypedDomains_AreSentTrimmed(string typed, string sent)
         => Assert.Equal(sent, RoleWords.DomainsToSend(typed));

      [Fact]
      public void ThePage_SaysItsRolesAndForm_AsTheReviewAsked()
      {
         string page = ReadView("RolesView.cs");

         // F085: one sentence under the title; the built-in roles and the admin level on the roles card; a short empty state.
         Assert.Contains("Subtitle = L(\"Who may administer what: the roles this server has, and the accounts that hold them.\"),", page, StringComparison.Ordinal);
         int card = page.IndexOf("Title = L(\"Roles\"),", StringComparison.Ordinal);
         Assert.True(card >= 0, "The roles card has no title of its own.");
         string roles = page.Substring(card, page.IndexOf("Content = rolesContent", card, StringComparison.Ordinal) - card);
         Assert.Contains("L(\"The four built-in roles cannot be edited:", roles, StringComparison.Ordinal);
         Assert.Contains("L(\"An account with no role here holds whatever its admin level has always given it.\")", roles, StringComparison.Ordinal);
         Assert.Contains("empty_.Text = L(\"Nobody has been given a role yet.\");", page, StringComparison.Ordinal);
         Assert.DoesNotContain("exactly as before roles existed", page, StringComparison.Ordinal);

         // F084, F086: the purpose, Yes or No, the domains.
         Assert.Contains("BuiltIn = parts[1] == \"1\" ? L(\"Yes\") : L(\"No\"),", page, StringComparison.Ordinal);
         Assert.Contains("Purpose = RoleWords.Purpose(parts[0], parts[1] == \"1\", parts[2]),", page, StringComparison.Ordinal);
         Assert.Contains("RoleWords.DomainsText(parts[2])", page, StringComparison.Ordinal);

         // F087: examples, what empty means, and the typed domains trimmed.
         Assert.Contains("address_.PlaceholderText = ExamplePlaceholder.Of(\"helpdesk@example.com\");", page, StringComparison.Ordinal);
         Assert.Contains("domains_.PlaceholderText = ExamplePlaceholder.Of(\"example.com, example.net\");", page, StringComparison.Ordinal);
         Assert.Contains("domainsRow.Hint = L(\"Empty: the account's own domain, or every domain for serveradmin and auditor. * is every domain for any role.\");", page, StringComparison.Ordinal);
         Assert.Contains("Write_(address, role, RoleWords.DomainsToSend(domains_.Text), ", page, StringComparison.Ordinal);

         // The buttons stand on the boxes' line whatever a hint or a message adds under a box:
         // under a row as tall as the captions (t34a's LabelGroup row, F088).
         Assert.Contains("form.Children.Add(BesideTheBoxes_(give_, new Thickness(0, 0, 8, 12)));", page, StringComparison.Ordinal);
         Assert.Contains("form.Children.Add(BesideTheBoxes_(take_, new Thickness(0, 0, 0, 12)));", page, StringComparison.Ordinal);
         string field = WithheldRowTests.MemberText(page, "private static Scaffold.FieldRow FormField_(string caption, FrameworkElement editor)");
         Assert.True(field != null && field.Contains("VerticalAlignment = VerticalAlignment.Top", StringComparison.Ordinal), "The form's boxes are not at the top of its line.");
      }

      private static Dictionary<string, string> ReadResx(string file)
      {
         var result = new Dictionary<string, string>(StringComparer.Ordinal);
         foreach (XElement data in XDocument.Load(Path.Join(ResourcesDirectory(), file)).Root.Elements("data"))
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

      private static string DbScripts()
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "DBScripts");
            if (File.Exists(Path.Join(candidate, "CreateTablesMSSQL.sql")))
               return candidate;
            directory = directory.Parent;
         }

         Assert.Fail("hmailserver/source/DBScripts could not be found from the test binaries.");
         return null;
      }

      private static string ReadView(string file)
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", "Views", file);
            if (File.Exists(candidate))
               return File.ReadAllText(candidate);
            directory = directory.Parent;
         }

         Assert.Fail("ControlPanel/Views/" + file + " could not be found from the test binaries.");
         return null;
      }
   }
}
