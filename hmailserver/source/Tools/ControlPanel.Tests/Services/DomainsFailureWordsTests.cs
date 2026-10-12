// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The Domains & accounts and Public folders pages say a failure in one form
   /// (review finding F093): "Could not ..." and the reason through
   /// ServerSession.DescribeComError, which says a lost link as one and the server's
   /// own words in the reader's language where the catalogue holds them. The page
   /// said "Couldn't load aliases: " and "Couldn't load distribution lists: " beside
   /// "Could not ..." everywhere else, and Create account translated the server's
   /// words but showed a lost link as the runtime's text while the other handlers
   /// did the reverse.
   /// </summary>
   public class DomainsFailureWordsTests
   {
      [Theory]
      [InlineData("Views/DomainsView.xaml.cs")]
      [InlineData("Views/DomainsView.Rows.cs")]
      [InlineData("Views/PublicFoldersView.cs")]
      public void ThePagesFailures_SayCouldNot_AndTheirReasonThroughDescribeComError(string file)
      {
         string source = ReadControlPanelFile(file);
         Assert.DoesNotContain("Couldn't", source, StringComparison.Ordinal);
         Assert.DoesNotContain("Couldn’t", source, StringComparison.Ordinal);

         // No reason shown as the exception's own text or the server's untranslated words.
         foreach (Match m in Regex.Matches(source, @"catch \(Exception (\w+)\)"))
         {
            string name = m.Groups[1].Value;
            Assert.DoesNotContain(name + ".Message", source, StringComparison.Ordinal);
         }

         Assert.DoesNotContain("ServerSentences.MessageOf", source, StringComparison.Ordinal);
      }

      [Fact]
      public void TheAliasesAndListsReadFailures_SayCouldNotRead_AsTheDomainsAndAccountsDo()
      {
         string page = ReadControlPanelFile("Views/DomainsView.xaml.cs");
         foreach (string template in new[] { "Could not read the domains: {0}", "Could not read the accounts: {0}", "Could not read the aliases: {0}", "Could not read the distribution lists: {0}" })
            Assert.Contains("L(\"" + template + "\")", page, StringComparison.Ordinal);

         Assert.Contains("F(\"Could not create the account: {0}\", ServerSession.DescribeComError(ex))", page, StringComparison.Ordinal);
      }

      /// <summary>
      /// DescribeComError says the server's own words through the catalogue, so a
      /// sentence the catalogue holds (ServerSentences) is in the reader's language
      /// wherever a page says a failure, and anything else as the server wrote it.
      /// </summary>
      [Fact]
      public void DescribeComError_SaysTheServersOwnWordsThroughTheCatalogue()
      {
         string session = ReadControlPanelFile("Services/ServerSession.cs");
         string describe = WithheldRowTests.MemberText(session, "public static string DescribeComError(Exception ex)");
         Assert.True(describe != null, "ServerSession.DescribeComError is gone.");
         Assert.Contains("return ComErrorText.NotTheServersWords(ex, msg) ?? L(msg);", describe, StringComparison.Ordinal);
      }

      private static string ReadControlPanelFile(string relative)
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", relative.Replace('/', Path.DirectorySeparatorChar));
            if (File.Exists(candidate))
               return File.ReadAllText(candidate);
            directory = directory.Parent;
         }

         Assert.Fail("ControlPanel/" + relative + " could not be found from the test binaries.");
         return null;
      }
   }
}
