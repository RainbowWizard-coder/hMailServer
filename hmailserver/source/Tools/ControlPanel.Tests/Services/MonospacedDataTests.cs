// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Text.RegularExpressions;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// Data - a cipher list, the key exchange groups, an LDAP search base and DN
   /// template, a log line, a script, a token, a header, a path - is set in the
   /// monospaced face, in a box or a block of its own at one size, AppFontSizeMono
   /// (Typography.Mono, 13), and inside a sentence as a run of its own (review
   /// findings F135, F304). The cipher list editors and the LDAP boxes were in the
   /// UI face, the transport encryption overview's cipher list broke inside a suite
   /// name, REST API keys' header was marked off by two spaces in its sentence, the
   /// stalled-mail guide's log lines were body text in SemiBold, and monospaced text
   /// was 12 pixels in the live log, the Label size in the diagnostics and 14 in the
   /// event script.
   /// </summary>
   public class MonospacedDataTests
   {
      [Fact]
      public void AColonSeparatedListBreaksOnlyAfterItsColons()
      {
         string shown = DataText.BreakAfterColons("ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES256-GCM-SHA384");

         Assert.Equal("ECDHE-ECDSA-AES128-GCM-SHA256:\u200BECDHE-RSA-AES256-GCM-SHA384", shown);
         Assert.Equal("", DataText.BreakAfterColons(null));
         Assert.Equal("AEAD-ONLY", DataText.BreakAfterColons("AEAD-ONLY"));
      }

      [Fact]
      public void ALongPathIsCutInItsMiddleAndAShortOneIsWhole()
      {
         const string Long = @"C:\Program Files\hMailServer\Bin\Configuration\Keys\Current\hMailServerApiKeys.ini";
         Assert.Equal(@"C:\Program Files\" + "\u2026" + @"\Current\hMailServerApiKeys.ini", DataText.MiddleOfPath(Long));
         Assert.Equal(@"C:\hMailServer\Bin\ApiKeys.ini", DataText.MiddleOfPath(@"C:\hMailServer\Bin\ApiKeys.ini"));
         Assert.Equal("", DataText.MiddleOfPath(null));
         string fourParts = @"C:\" + new string('a', 70) + @"\b\c.ini";
         Assert.Equal(fourParts, DataText.MiddleOfPath(fourParts));
      }

      [Fact]
      public void TheMonoSizeIsOneTokenSaidInCodeAndInXaml()
      {
         Assert.Equal(13, Typography.Mono);
         string tokens = Read("Views", "Scaffold", "Tokens.xaml");
         Assert.Contains("<sys:Double x:Key=\"AppFontSizeMono\">13</sys:Double>", tokens, StringComparison.Ordinal);
      }

      [Fact]
      public void TheCipherListAndGroupEditorsAreMonospaced()
      {
         string settings = Read("Views", "ServerSettingsView.xaml.cs");
         foreach (string path in new[] { "TlsKeyExchangeGroups", "SslCipherList", "TlsCipherSuites13" })
            // After the Label, which build/generate-settings-index.ps1 reads beside the Path.
            Assert.Matches(new Regex(@"Path = """ + path + @""",\s*Label = L\(""[^""]*""\),\s*Monospaced = true,"), settings);

         string editor = WithheldRowTests.MemberText(settings, "protected TextBox TextEditor(string text, string placeholder = null)");
         Assert.True(editor != null, "ServerSettingsView has no TextEditor.");
         Assert.Contains("FontSize = Monospaced ? Typography.Mono : Typography.Body", editor, StringComparison.Ordinal);
         Assert.Contains("box.FontFamily = new System.Windows.Media.FontFamily(Typography.MonoFontFamily);", editor, StringComparison.Ordinal);
      }

      [Fact]
      public void TheOverviewsCipherListIsARunOfItsOwn()
      {
         string overview = Read("Views", "TlsOverviewView.cs");
         Assert.Contains("DataTextBlock.Fill(cipherLine, L(\"Cipher list for TLS 1.2 and below: {0}.", overview, StringComparison.Ordinal);
         Assert.Contains("config.CipherList, breakAfterColons: true);", overview, StringComparison.Ordinal);

         string block = Read("Services", "DataTextBlock.cs");
         Assert.Contains("new Run(shown) { FontFamily = new FontFamily(Typography.MonoFontFamily) }", block, StringComparison.Ordinal);
      }

      [Fact]
      public void TheDirectoryPagesNamesAndFiltersAreMonospaced()
      {
         string ldap = Read("Views", "LdapSettingsView.cs");
         Assert.Contains("foreach (Wpf.Ui.Controls.TextBox box in new[] { searchBase_, userDnTemplate_ })", ldap, StringComparison.Ordinal);
         Assert.Contains("LdapFilterField.ShowWhole(userSearchFilter_);", ldap, StringComparison.Ordinal);

         string field = Read("Views", "LdapFilterField.cs");
         Assert.Contains("box.FontSize = Typography.Mono;", field, StringComparison.Ordinal);
      }

      [Fact]
      public void TheBearerHeaderIsALineOfItsOwnAndTheStorePathIsCut()
      {
         string keys = Read("Views", "ApiKeysView.cs");
         Assert.DoesNotContain("Authorization: Bearer hmapi_...  to", keys, StringComparison.Ordinal);
         Assert.Contains("Text = \"Authorization: Bearer hmapi_...\"", keys, StringComparison.Ordinal);
         Assert.Contains("DataText.MiddleOfPath(path)", keys, StringComparison.Ordinal);
         Assert.Contains("storePath_.ToolTip = path;", keys, StringComparison.Ordinal);
      }

      [Fact]
      public void TheStalledMailGuidesLogLinesAreMonospaced()
      {
         string guide = Read("Views", "StalledMailView.cs");
         Assert.Contains("}, logLines: true));", guide, StringComparison.Ordinal);
         string table = WithheldRowTests.MemberText(guide, "private static Grid Table((string what, string cause)[] rows, bool logLines = false)");
         Assert.True(table != null, "StalledMailView has no Table with log lines.");
         Assert.Contains("what.FontSize = Typography.Mono;", table, StringComparison.Ordinal);
      }

      /// <summary>
      /// Every box or block of monospaced text is the one size: none sets
      /// MonoFontFamily beside another of the ramp's sizes. The one-time code boxes,
      /// set large for typing six digits, are the exception.
      /// </summary>
      [Theory]
      [InlineData("Views", "ScriptsView.cs")]
      [InlineData("Views", "AccountDialog.cs")]
      [InlineData("Views", "AppPasswordsPanel.cs")]
      [InlineData("Views", "UtilityViews.cs")]
      [InlineData("Views", "DnsRecordsView.cs")]
      [InlineData("Views", "DomainDialog.cs")]
      [InlineData("Views", "ApiKeysView.cs")]
      public void MonospacedTextIsTheMonoSize(string folder, string file)
      {
         string source = Read(folder, file);
         var initialiser = new Regex(@"\{[^{}]*Typography\.MonoFontFamily[^{}]*\}", RegexOptions.Singleline);
         int seen = 0;
         foreach (Match block in initialiser.Matches(source))
         {
            Match size = Regex.Match(block.Value, @"FontSize = (Typography\.\w+)");
            if (!size.Success)
               continue;
            seen++;
            Assert.True(size.Groups[1].Value == "Typography.Mono", file + " sets monospaced text at " + size.Groups[1].Value + ": " + block.Value.Trim());
         }

         foreach (Match line in Regex.Matches(source, @"(\w+)\.FontFamily = new System\.Windows\.Media\.FontFamily\(Typography\.MonoFontFamily\);"))
         {
            Match size = Regex.Match(source, Regex.Escape(line.Groups[1].Value) + @"\.FontSize = (Typography\.\w+);");
            if (!size.Success)
               continue;
            seen++;
            Assert.Equal("Typography.Mono", size.Groups[1].Value);
         }

         Assert.True(seen > 0, file + " sets no monospaced text at a size.");
      }

      [Fact]
      public void TheLiveLogAndTheMessageViewerAreTheMonoSize()
      {
         Assert.Contains("FontFamily=\"Cascadia Mono, Consolas\" FontSize=\"{StaticResource AppFontSizeMono}\"", Read("Views", "LogsView.xaml"), StringComparison.Ordinal);
         Assert.Contains("content.SetResourceReference(FontSizeProperty, \"AppFontSizeMono\");", Read("Views", "MessageViewerDialog.cs"), StringComparison.Ordinal);
      }

      private static string Read(params string[] parts)
      {
         string controlPanel = ContrastTokensTests.FindControlPanelDirectory();
         Assert.True(controlPanel != null, "The Control Panel's sources were not found above the test binaries.");
         return File.ReadAllText(Path.Join(controlPanel, Path.Join(parts)));
      }
   }
}
