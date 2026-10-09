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
   /// The Control Panel's text names a setting the way the reader sees it - its
   /// caption and the page it is on - and not by the key it is stored under in
   /// hMailServer.ini or the server's object model (review finding F020, and the
   /// INI keys of F280; the owner's decision on hints: INI key names only as a
   /// small caption on the expert pages). The setup checklist said "Set
   /// TlsRptFromAddress in the [Settings] section of hMailServer.ini" beside a page
   /// whose field is captioned "TLS report sender address", the never-ban list
   /// "list the address in AutoBanNeverBan", the stalled-mail guide named nine
   /// timeouts by key, and two notes named the server's source:
   /// "(Utilities::ComputerName)" and "(the table lives in WindowsEventLog.h)".
   /// </summary>
   public class SettingNamesInTextTests
   {
      // The settings views give every setting with an editor its key: Key = "..." on
      // the hMailServer.ini pages, Path = "..." on the COM and REST ones.
      private static readonly string[] SettingsViews =
      {
         "FeatureSettingsView.xaml.cs", "ServerSettingsView.xaml.cs", "ServerSettingsView.NeverBanNames.cs",
      };

      private static readonly Regex KeyOrPath = new Regex(@"\b(?:Key|Path)\s*=\s*""([A-Za-z][A-Za-z0-9.]*)""");
      private static readonly Regex Identifier = new Regex(@"^(?:[A-Z][a-z0-9]+){2,}$|^[A-Z]{2,}[A-Za-z0-9]*$");

      [Fact]
      public void NoText_NamesASettingThatHasACaption_ByItsKey()
      {
         var names = new HashSet<string>(StringComparer.Ordinal);
         foreach (string view in SettingsViews)
         {
            string source = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", view));
            foreach (Match match in KeyOrPath.Matches(source))
            {
               string name = match.Groups[1].Value.Split('.').Last();
               if (Identifier.IsMatch(name))
                  names.Add(name);
            }
         }

         Assert.True(names.Count > 300, "The settings views gave only " + names.Count + " keys; the pattern no longer reads them.");
         Assert.Contains("TlsRptFromAddress", names);
         Assert.Contains("AutoBanNeverBan", names);

         var offenders = new List<string>();
         foreach (string text in EnglishTexts())
         {
            var named = names.Where(name => Regex.IsMatch(text, @"(?<![\w.])" + Regex.Escape(name) + @"(?!\w)")).ToList();
            if (named.Count > 0)
               offenders.Add(string.Join(", ", named) + ": " + text);
         }

         Assert.True(offenders.Count == 0, "These name a setting by its key; name its caption and its page:\n" + string.Join("\n", offenders));
      }

      [Fact]
      public void NoText_NamesTheServersSourceCode()
      {
         var source = new Regex(@"\b\w+::\w+|\b\w+\.(?:h|hpp|cpp|cs)\b");
         var offenders = EnglishTexts().Where(text => source.IsMatch(text)).ToList();
         Assert.True(offenders.Count == 0, "These name a source file or a member:\n" + string.Join("\n", offenders));
      }

      private static IEnumerable<string> EnglishTexts()
      {
         string path = ConfirmationPatternTests.ControlPanelFile(Path.Join("Resources", "Strings.resx"));
         return XDocument.Load(path).Root.Elements("data").Select(d => (string)d.Attribute("name"));
      }
   }
}
