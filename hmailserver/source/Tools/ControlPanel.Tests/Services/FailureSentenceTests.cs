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
   /// The sentences ServerSession.DescribeComError and ComErrorText give for a
   /// failure that blocks every read - a lost link, no database, a session that is
   /// not the server administrator's, an older server, an answer that could not be
   /// read - are whole sentences that begin with a capital and name no button
   /// (review findings F007, F179, F062). They were written to follow a prefix, so
   /// a page that showed one alone said "Critical  the connection to the hMailServer
   /// service was lost ...", and they told the reader to press Reload on pages
   /// whose button is Refresh, or that have none.
   /// </summary>
   public class FailureSentenceTests
   {
      // The keys as the source writes them, after the change.
      private static readonly string[] Sentences =
      {
         "The connection to the hMailServer service was lost - it was most likely restarted. The Control Panel reconnects by itself; try again in a moment.",
         "The server cannot reach its database. Check the database connection and the hMailServer error log, then try again. ({0})",
         "Sign in with the hMailServer server-administrator account to view or change these settings. ({0})",
         "The server has no such setting or action - it may be older than this Control Panel ({0})",
         "The server answered something this Control Panel could not read ({0})",
      };

      private static readonly Regex NamesAButton = new Regex(@"\b(press|click)\b|\bReload\b|\bRefresh\b", RegexOptions.IgnoreCase);

      [Fact]
      public void DescribeComError_SaysEachFailureInWholeSentencesThatNameNoButton()
      {
         string session = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Services", "ServerSession.cs"));
         string describe = WithheldRowTests.MemberText(session, "public static string DescribeComError(Exception ex)");
         var texts = Regex.Matches(describe, @"\b[LF]\(""((?:[^""\\]|\\.)*)""").Select(m => m.Groups[1].Value).ToList();

         // "Unknown error." is the fallback when an exception carries no message.
         Assert.Equal(4, texts.Count);
         foreach (string text in texts)
         {
            Assert.True(char.IsUpper(text[0]), "Starts in lower case: " + text);
            Assert.False(NamesAButton.IsMatch(text), "Names a button: " + text);
         }

         foreach (string sentence in Sentences.Take(3))
            Assert.Contains(sentence, texts);
      }

      [Fact]
      public void TheSentencesForAFailureThatIsNotTheServersWords_BeginWithACapital()
      {
         Assert.StartsWith("The server answered something", ComErrorText.NotTheServersWords(new FormatException("x"), "x"), StringComparison.Ordinal);
         Assert.StartsWith("The server has no such setting", ComErrorText.NotTheServersWords(new MissingMethodException("x"), "x"), StringComparison.Ordinal);
      }

      /// <summary>
      /// Every language says them as whole sentences too: a translation that kept the
      /// old lower-case start would show it on every page that says the reason alone.
      /// Japanese and Chinese have no case; Turkish begins one with the product's own
      /// name, hMailServer, which is written so in every language.
      /// </summary>
      [Fact]
      public void EveryCatalogue_BeginsTheSentencesWithACapital()
      {
         var offenders = new List<string>();
         foreach (string path in Directory.GetFiles(ResourcesDirectory(), "Strings.*.resx"))
         {
            string tag = Path.GetFileNameWithoutExtension(path).Substring("Strings.".Length);
            var values = XDocument.Load(path).Root.Elements("data")
               .ToDictionary(d => (string)d.Attribute("name"), d => (string)d.Element("value") ?? "");
            foreach (string key in Sentences)
            {
               Assert.True(values.ContainsKey(key), "Strings." + tag + ".resx has no " + key);
               string value = values[key];
               Assert.False(string.IsNullOrEmpty(value), "Strings." + tag + ".resx leaves " + key + " untranslated.");
               if (value.StartsWith("hMailServer", StringComparison.Ordinal))
                  continue;
               char first = value[0];
               if (char.IsLetter(first) && char.ToUpper(first, CultureInfo.InvariantCulture) != char.ToLower(first, CultureInfo.InvariantCulture) && !char.IsUpper(first))
                  offenders.Add(tag + ": " + value);
            }
         }

         Assert.True(offenders.Count == 0, "Begins in lower case:\n" + string.Join("\n", offenders));
      }

      /// <summary>
      /// A reason joined to what failed is introduced by a colon, as the server's own
      /// refusals have always been: "The TLS certificate list could not be read -
      /// The connection ..." put a capital after a dash.
      /// </summary>
      [Theory]
      [InlineData("Services/ExternalSetupChecks.cs", "F(\"The TLS certificate list could not be read: {0}\", firstError_)")]
      [InlineData("Services/ExternalSetupChecks.cs", "F(\"The TCP/IP port list could not be read: {0}\", firstError_)")]
      [InlineData("Services/ActiveDirectoryService.cs", "F(\"Active Directory cannot be reached from this computer, so its accounts cannot be browsed here. Windows reported: {0}\", ServerSession.DescribeComError(ex))")]
      [InlineData("Views/DashboardView.xaml.cs", "F(\"The external setup could not be checked: {0}\", ServerSession.DescribeComError(ex))")]
      public void AReasonJoinedToWhatFailed_FollowsAColon(string file, string call)
      {
         Assert.Contains(call, ConfirmationPatternTests.ReadControlPanelFile(file.Replace('/', Path.DirectorySeparatorChar)), StringComparison.Ordinal);
      }

      [Fact]
      public void NoTemplate_JoinsDescribeComErrorWithADash()
      {
         var dashed = new Regex(@"\bF\(""[^""]*(?: - | \u2014 | \u2013 )\{\d+\}""\s*,\s*(?:ServerSession\.)?DescribeComError\(");
         var found = ConfirmationPatternTests.SourceFiles()
            .SelectMany(f => f.Source.Split('\n').Select((line, i) => (f.Relative, Line: i + 1, Text: line)))
            .Where(t => dashed.IsMatch(t.Text))
            .Select(t => t.Relative + ":" + t.Line + ": " + t.Text.Trim())
            .ToList();
         Assert.True(found.Count == 0, "A reason after a dash:\n" + string.Join("\n", found));
      }

      private static string ResourcesDirectory()
         => Path.GetDirectoryName(ConfirmationPatternTests.ControlPanelFile(Path.Join("Resources", "Strings.resx")));
   }
}
