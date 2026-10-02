// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The two switches of GitLab issue 51 - RefuseHeloNotFqdn and
   /// RefuseMailFromDomainNotFqdn - on the Anti-spam page, under Host checks, where
   /// the item asks for them and where an administrator looking at the HELO check
   /// already is.
   ///
   /// Source scanning, for the reason SettingClaimsTests and PageSplitTests give:
   /// the rows are string literals inside a WPF view the test project cannot
   /// reference, and building the page needs a dispatcher and a live COM session.
   /// Both tests fail against the tree before the card was added: the card is not
   /// in BuildAntiSpam, and neither property is in the settings index.
   /// </summary>
   public class FqdnRefusalPageTests
   {
      private static readonly string[] Paths =
      {
         "AntiSpam.RefuseHeloNotFqdn",
         "AntiSpam.RefuseMailFromDomainNotFqdn"
      };

      /// <summary>
      /// One card on the Host checks tab holds both switches, each a checkbox with
      /// its note: a refusal that is off by default and says so beside the switch,
      /// as the null sender's card beside it does.
      /// </summary>
      [Fact]
      public void BothSwitches_AreOneCardOnTheHostChecksTab_EachWithItsNote()
      {
         string source = ReadServerSettingsView();
         Assert.False(source == null, "ServerSettingsView.xaml.cs could not be found from the test binaries.");

         string antiSpam = BuilderBody(source, "BuildAntiSpam");
         Assert.True(antiSpam != null, "BuildAntiSpam is gone from ServerSettingsView.");

         Match card = Regex.Match(antiSpam, @"var (\w+) = Card\(L\(""HELO names and sender domains that are not host names""\)");
         Assert.True(card.Success, "The card for HELO names and sender domains that are not host names is not built on the Anti-spam page.");

         string name = card.Groups[1].Value;

         foreach (string path in Paths)
         {
            Assert.Matches(new Regex(Regex.Escape(name) + @"\.Settings\.Add\(new ComBool\s*\{\s*Path = """ + Regex.Escape(path) +
                                     @"""[\s\S]{0,400}?Blurb = L\(""Off by default\."), antiSpam);
         }

         Assert.Contains("Tab(L(\"Host checks\")).Cards.Add(" + name + ");", antiSpam, StringComparison.Ordinal);
      }

      /// <summary>
      /// The palette finds both switches and opens the Anti-spam page for them. The
      /// index is generated from the view by build/generate-settings-index.ps1, so
      /// this fails until it is regenerated after the card is added.
      /// </summary>
      [Fact]
      public void BothSwitches_AreIndexedAgainstTheAntiSpamPage()
      {
         foreach (string path in Paths)
         {
            SettingEntry entry = SettingsSearchIndex.Entries
               .FirstOrDefault(e => string.Equals(e.Key, path, StringComparison.OrdinalIgnoreCase));

            Assert.True(entry != null, "'" + path + "' is not in the settings index. Re-run build/generate-settings-index.ps1.");
            Assert.Equal("antispam", entry.Page);
         }
      }

      /// <summary>The body of one Build&lt;Section&gt; method, or null.</summary>
      private static string BuilderBody(string source, string method)
      {
         Match match = Regex.Match(source, "private void " + method + @"\(\)\s*\{(.*?)\n      \}", RegexOptions.Singleline);
         return match.Success ? match.Groups[1].Value : null;
      }

      private static string ReadServerSettingsView()
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);

         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", "Views", "ServerSettingsView.xaml.cs");
            if (File.Exists(candidate))
               return File.ReadAllText(candidate);

            directory = directory.Parent;
         }

         return null;
      }
   }
}
