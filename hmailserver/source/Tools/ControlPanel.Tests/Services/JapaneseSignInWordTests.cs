// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using System.Xml.Linq;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// Where the English says sign in, the Japanese says サインイン, as the catalogue
   /// does for the account's Last sign-in row (最終サインイン) and the request to sign
   /// in as the server administrator (the owner's decision on the word: Sign in,
   /// where the Control Panel said log in, logged in and log on). The English of
   /// three texts was changed to sign in and their Japanese kept ログイン and ログオン:
   /// the distribution list's "people who have signed in", the account dialog's
   /// Windows directory tab's "can never sign in" - on the dialog whose General tab
   /// says 最終サインイン - and the setup checklist's "no client can sign in with a
   /// token" (found re-checking 7b204b840).
   /// </summary>
   public class JapaneseSignInWordTests
   {
      private static readonly Regex SignIn = new Regex(@"\bsign(?:s|ed|ing)?[ -]in\b", RegexOptions.IgnoreCase);

      [Fact]
      public void EveryTextThatSaysSignIn_SaysItWithTheCataloguesWord()
      {
         Dictionary<string, string> japanese = ReadResx("Strings.ja.resx");
         List<string> signIn = japanese.Keys.Where(key => SignIn.IsMatch(key)).ToList();
         Assert.True(signIn.Count > 50, "Only " + signIn.Count + " texts say sign in; the pattern no longer reads them.");

         List<string> found = signIn
            .Where(key => japanese[key].Contains("ログイン", StringComparison.Ordinal) || japanese[key].Contains("ログオン", StringComparison.Ordinal))
            .Select(key => key + " = " + japanese[key])
            .ToList();
         Assert.True(found.Count == 0, "Sign in is サインイン in Japanese:" + Environment.NewLine + string.Join(Environment.NewLine, found));

         Assert.Equal("最終サインイン", japanese["Last sign-in"]);
      }

      private static Dictionary<string, string> ReadResx(string file)
      {
         var result = new Dictionary<string, string>(StringComparer.Ordinal);
         string path = ConfirmationPatternTests.ControlPanelFile(Path.Join("Resources", file));
         foreach (XElement data in XDocument.Load(path).Root.Elements("data"))
            result[(string)data.Attribute("name")] = data.Element("value")?.Value ?? "";
         return result;
      }
   }
}
