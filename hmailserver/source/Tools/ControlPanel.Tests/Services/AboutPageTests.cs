// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using System.Xml.Linq;
using hMailServer.ControlPanel.Services;
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
         Assert.Contains("ClipboardWriter.TrySetText(text, out string error)", page, StringComparison.Ordinal);
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

      /// <summary>
      /// The host is the session's own and is read outside the server's version, so
      /// that a version the server does not give leaves Connected to and the report's
      /// server line with the host: read in one try with the version, a failed read
      /// of Application.Version said "Not read" of the host beside the command bar's
      /// Connected, and Copy version details left the server line out, host and all.
      /// </summary>
      [Fact]
      public void TheHost_IsShownAndCopied_WhenTheServersVersionIsNotRead()
      {
         string enter = WithheldRowTests.MemberText(Page(), "public void OnEnter()");
         Assert.NotNull(enter);
         int host = enter.IndexOf("host_ = ServerSession.Current?.Host ?? \"\";", StringComparison.Ordinal);
         Match block = Regex.Match(enter, @"^\s*try\s*$", RegexOptions.Multiline);
         Assert.True(block.Success, "OnEnter reads nothing in a try.");
         int attempt = block.Index;
         int version = enter.IndexOf("serverVersion_ = (string)ServerSession.Current.Application.Version ?? \"\";", StringComparison.Ordinal);
         Assert.True(host >= 0 && host < attempt && attempt < version, "The host is read inside the try that reads the server's version.");
         Assert.Equal(1, enter.Split("host_ =").Length - 1);

         string copy = WithheldRowTests.MemberText(Page(), "private void CopyVersionDetails_()");
         Assert.NotNull(copy);
         Assert.Contains("VersionDetailsReport.Text(ControlPanelVersion_(), serverVersion_, host_, database_,", copy, StringComparison.Ordinal);

         // Connected to shows the host whatever the version read did.
         Assert.Contains("hostValue_.Text = host_.Length > 0 ? host_ : L(\"Not read\");", enter, StringComparison.Ordinal);
      }

      [Fact]
      public void TheReportsServerLine_KeepsTheHost_WhenTheVersionIsNotRead()
      {
         Assert.Equal("Server: hMailServer 6.3.6 on mail.example.com", VersionDetailsReport.ServerLine("6.3.6", "mail.example.com"));
         Assert.Equal("Server: hMailServer (version not read) on mail.example.com", VersionDetailsReport.ServerLine("", "mail.example.com"));
         Assert.Equal("Server: hMailServer 6.3.6", VersionDetailsReport.ServerLine("6.3.6", ""));
         Assert.Null(VersionDetailsReport.ServerLine("", null));

         string[] report = VersionDetailsReport.Text("6.3.6 (abc1234)", "", "mail.example.com", "", "10.0.1", "10.0.26200.0")
            .Split(Environment.NewLine, StringSplitOptions.RemoveEmptyEntries);
         Assert.Equal(new[]
         {
            "hMailServer Control Panel 6.3.6 (abc1234)",
            "Server: hMailServer (version not read) on mail.example.com",
            ".NET 10.0.1",
            "Windows 10.0.26200.0",
         }, report);
         Assert.Contains("Database: MySQL", VersionDetailsReport.Text("6.3.6", "6.3.6", "localhost", "MySQL", "10.0.1", "10.0"), StringComparison.Ordinal);
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
