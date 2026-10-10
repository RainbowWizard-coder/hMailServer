// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Linq;
using System.Xml.Linq;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The About page (review finding F314; the owner's choice of 9 October 2026):
   /// where to get help first - the forum, the issue tracker and the version
   /// details to copy into either - then the versions as a label and a value a
   /// line, then who maintains it, the name in ordinary strong text; and its words
   /// say what is so. Its purpose promised support it did not give, the name was
   /// set larger than the product's, and the copy read as a brochure.
   /// </summary>
   public class AboutPageTests
   {
      private static string Page() => ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "AboutView.cs"));

      [Fact]
      public void HelpComesFirst_ThenTheVersions_ThenWhoMaintainsIt()
      {
         string page = Page();
         int help = page.IndexOf("Title = L(\"Get help\")", StringComparison.Ordinal);
         int versions = page.IndexOf("Title = L(\"hMailServer Control Panel\")", StringComparison.Ordinal);
         int maintainer = page.IndexOf("Title = L(\"Developed & maintained by\")", StringComparison.Ordinal);
         Assert.True(help > 0 && versions > help && maintainer > versions, "The About page's cards are not help, versions, maintainer.");

         Assert.Contains("\"https://www.hmailserver.co.uk/\"", page, StringComparison.Ordinal);
         Assert.Contains("\"https://gitlab.com/hmailserver/hmailserver/-/issues\"", page, StringComparison.Ordinal);
         Assert.Contains("L(\"_Copy version details\")", page, StringComparison.Ordinal);
         Assert.Contains("ClipboardWriter.TrySetText(text.ToString(), out string error)", page, StringComparison.Ordinal);
      }

      [Fact]
      public void TheVersionsAreALabelAndAValueALine()
      {
         string page = Page();
         foreach (string row in new[]
         {
            "AddVersion_(versions, L(\"Control Panel\"), Value_(ControlPanelVersion_()));",
            "AddVersion_(versions, L(\"Server\"), serverValue_);",
            "AddVersion_(versions, L(\"Connected to\"), hostValue_);",
            "AddVersion_(versions, L(\"Database\"), databaseValue_);",
            "AddVersion_(versions, \".NET\", Value_(Environment.Version.ToString()));",
            "AddVersion_(versions, \"Windows\", Value_(Environment.OSVersion.Version.ToString()));",
            "AddVersion_(versions, L(\"Licence\"), Link_(\"GNU AGPLv3\", LicenceUrl));",
         })
            Assert.Contains(row, page, StringComparison.Ordinal);

         // The build is the commit the SDK writes after the "+" of the informational version.
         Assert.Contains("GetCustomAttribute<AssemblyInformationalVersionAttribute>()", page, StringComparison.Ordinal);
         Assert.Contains("informational.IndexOf('+')", page, StringComparison.Ordinal);
         Assert.Contains("StatusView.DatabaseTypeName((int)db.DatabaseType)", page, StringComparison.Ordinal);
         Assert.Contains("internal static string DatabaseTypeName(int type)",
            ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "StatusView.xaml.cs")), StringComparison.Ordinal);
      }

      [Fact]
      public void TheNameIsOrdinaryStrongText_AndTheWordsAreFactual()
      {
         string page = Page();
         Assert.Contains("Text(\"Christopher Holloway\", \"TextBodyStrong\"", page, StringComparison.Ordinal);
         Assert.DoesNotContain("\"TextSubtitle\"", page, StringComparison.Ordinal);

         var keys = XDocument.Load(ConfirmationPatternTests.ControlPanelFile(Path.Join("Resources", "Strings.resx"))).Root
            .Elements("data").Select(d => (string)d.Attribute("name")).ToList();
         Assert.DoesNotContain(keys, k => k.StartsWith("A modern administration app", StringComparison.Ordinal));
         Assert.DoesNotContain(keys, k => k.Contains("re-armed with modern cryptography", StringComparison.Ordinal));
         Assert.DoesNotContain(keys, k => k.StartsWith("Progressive Robot Ltd is a software engineering company that builds", StringComparison.Ordinal));

         // What 6.x adds is what the original lacked: the original ran on MySQL,
         // Microsoft SQL Server and PostgreSQL already.
         string adds = keys.Single(k => k.StartsWith("hMailServer 6.x is built with", StringComparison.Ordinal));
         Assert.DoesNotContain("PostgreSQL", adds, StringComparison.Ordinal);
         Assert.Contains("L(\"" + adds + "\")", page, StringComparison.Ordinal);
      }
   }
}
