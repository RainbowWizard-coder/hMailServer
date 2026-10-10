// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Text.RegularExpressions;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The LDAP search filters of the Directory synchronisation and Directory
   /// authentication (LDAP) pages shown whole (review finding F131): the width of
   /// the card, wrapping, monospaced, with the server's default in the hint, and
   /// the synchronisation page's cards at the form width the LDAP page's have. The
   /// synchronisation filter's placeholder was cut at
   /// "...(!(userAccountControl:1.2.840.113556.1.4.80", the start of the clause that
   /// keeps disabled accounts out.
   /// </summary>
   public class LdapFilterFieldTests
   {
      [Fact]
      public void AFilterBox_IsShownWhole()
      {
         string field = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "LdapFilterField.cs"));
         Assert.Contains("box.HorizontalAlignment = HorizontalAlignment.Stretch;", field, StringComparison.Ordinal);
         Assert.Contains("box.MaxWidth = double.PositiveInfinity;", field, StringComparison.Ordinal);
         Assert.Contains("box.TextWrapping = TextWrapping.Wrap;", field, StringComparison.Ordinal);
         Assert.Contains("box.MinLines = 2;", field, StringComparison.Ordinal);
         Assert.Contains("box.FontFamily = new FontFamily(Typography.MonoFontFamily);", field, StringComparison.Ordinal);
         Assert.Contains("box.Loaded += (_, _) => WrapPlaceholder_(box);", field, StringComparison.Ordinal);
         Assert.Contains("words.TextWrapping = TextWrapping.Wrap;", field, StringComparison.Ordinal);

         // The hint is the page's: a sentence appended here, "The default filter:
         // ...", made the synchronisation filter's hint three sentences.
         Assert.DoesNotContain("row.Hint", field, StringComparison.Ordinal);
         Assert.DoesNotContain("The default filter", field, StringComparison.Ordinal);
      }

      [Theory]
      [InlineData("DirectorySyncView.cs", "LdapFilterField.ShowWhole(filter_);")]
      [InlineData("LdapSettingsView.cs", "LdapFilterField.ShowWhole(userSearchFilter_);")]
      public void BothPages_ShowTheirFilterWhole(string view, string line)
      {
         Assert.Contains(line, View(view), StringComparison.Ordinal);
      }

      /// <summary>
      /// Each hint names the server's default inside a sentence it already had -
      /// Directory synchronisation's second, the LDAP page's first - so that the
      /// synchronisation filter's hint stays at the two sentences the owner's
      /// decision 11 keeps on a page, where a sentence of its own for the default
      /// made it three, and the LDAP page's hint gains none.
      /// </summary>
      [Theory]
      [InlineData("DirectorySyncView.cs", "dirsync-filter", "DefaultSyncFilter", "The LDAP filter that selects the people who should have a mailbox. The default, {0}, takes enabled person objects", 2)]
      [InlineData("LdapSettingsView.cs", "ldap-user-search-filter", "DefaultUserSearchFilter", "Left empty, the server uses the shipped filter, {0} - objectCategory first", 3)]
      public void EachHint_NamesTheDefault_InASentenceItHad(string view, string id, string constant, string start, int sentences)
      {
         string page = View(view);
         int at = page.IndexOf("\"" + id + "\"", StringComparison.Ordinal);
         Assert.True(at >= 0, view + " has no " + id + " box.");
         Match hint = new Regex(@"F\(""((?:[^""\\]|\\.)*)"", " + constant + @"\)").Match(page, at);
         Assert.True(hint.Success && hint.Index - at < 1200, view + "'s " + id + " hint does not name " + constant + " through F().");
         Assert.StartsWith(start, hint.Groups[1].Value, StringComparison.Ordinal);
         Assert.Equal(sentences, Regex.Matches(hint.Groups[1].Value, @"[.!?]\s+[A-Z%(]").Count + 1);
         Assert.DoesNotContain("shown in grey", hint.Groups[1].Value, StringComparison.Ordinal);
      }

      /// <summary>
      /// What the pages say is the default is the server's own, character for
      /// character: a default stated without the userAccountControl clause is a
      /// filter that provisions a mailbox for every disabled leaver.
      /// </summary>
      [Theory]
      [InlineData("DirectorySyncView.cs", "DefaultSyncFilter", "kDefaultSyncFilter")]
      [InlineData("LdapSettingsView.cs", "DefaultUserSearchFilter", "kDefaultUserSearchFilter")]
      public void TheDefaultSaid_IsTheServersOwn(string view, string constant, string serverConstant)
      {
         Match page = Regex.Match(View(view), "private const string " + constant + @"\s*=\s*""([^""]+)""");
         Assert.True(page.Success, view + " has no " + constant + " held as one literal.");

         string settings = File.ReadAllText(ServerFile(Path.Join("Common", "LDAP", "LdapSettings.cpp")));
         Match declared = Regex.Match(settings, @"\*" + serverConstant + @"\s*=\s*((?:\s*(?:L|_T\()?""[^""]*""\)?)+);");
         Assert.True(declared.Success, "LdapSettings.cpp has no " + serverConstant + ".");
         string server = string.Concat(Regex.Matches(declared.Groups[1].Value, @"""([^""]*)""").Select(m => m.Groups[1].Value));

         Assert.Equal(server, page.Groups[1].Value);
         Assert.Contains("userAccountControl:1.2.840.113556.1.4.803:=2", Regex.Match(View("DirectorySyncView.cs"), "private const string DefaultSyncFilter[^;]*;").Value, StringComparison.Ordinal);
      }

      [Fact]
      public void TheSynchronisationCards_AreTheFormsWidth()
      {
         string page = View("DirectorySyncView.cs");
         Assert.Contains("cards.SetResourceReference(MaxWidthProperty, \"AppFormWidth\");", page, StringComparison.Ordinal);
         Assert.DoesNotContain("MaxWidth = 860", page, StringComparison.Ordinal);
      }

      private static string View(string file) => ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", file));

      private static string ServerFile(string relative)
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "Server", relative);
            if (File.Exists(candidate))
               return candidate;
            candidate = Path.Join(directory.FullName, "hmailserver", "source", "Server", relative);
            if (File.Exists(candidate))
               return candidate;
            directory = directory.Parent;
         }

         Assert.Fail("Server/" + relative.Replace('\\', '/') + " could not be found from the test binaries.");
         return null;
      }
   }
}
