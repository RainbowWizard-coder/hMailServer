// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// A notice that names another page carries a button that opens it
   /// (Services/NoticeActions): "Open IP ranges" under "Localhost is currently
   /// banned in the IP ranges", where the warning named the page and offered no
   /// way to it. The button is captioned from the navigation map's own title, so
   /// it and the page agree in every language, and every page a notice opens is a
   /// page of the map - a key that is not opens nothing.
   ///
   /// Scanned rather than rendered: this assembly has no WPF.
   /// </summary>
   public class NoticeActionsTests
   {
      [Fact]
      public void TheButton_IsCaptionedFromTheNavigationMap_AndOpensThePage()
      {
         string source = File.ReadAllText(ControlPanelFile(Path.Join("Services", "NoticeActions.cs")));
         Assert.Contains("F(\"Open {0}\", L(NavigationMap.TitleOf(page)))", source, StringComparison.Ordinal);
         Assert.Contains("NavigateTo(page)", source, StringComparison.Ordinal);
      }

      [Fact]
      public void EveryPageANoticeOpens_IsAPageOfTheNavigationMap()
      {
         List<(string File, string Key)> keys = PageKeys().ToList();
         Assert.True(keys.Count >= 4, "No notice in the views opens a page; the rule checked nothing.");

         foreach ((string file, string key) in keys)
            Assert.True(NavigationMap.Find(key) != null, file + " opens \"" + key + "\" from a notice, which is no page of the navigation map.");
      }

      /// <summary>
      /// Every literal page key a view hands to NoticeActions.OpenPage, and every
      /// one the Server status page's configuration checks name.
      /// </summary>
      internal static IEnumerable<(string File, string Key)> PageKeys()
      {
         string views = Path.GetDirectoryName(ControlPanelFile(Path.Join("Views", "StatusView.xaml.cs")));
         foreach (string path in Directory.EnumerateFiles(views, "*.cs"))
         {
            string source = File.ReadAllText(path);
            string name = Path.GetFileName(path);

            foreach (Match open in Regex.Matches(source, @"NoticeActions\.OpenPage\(""([\w-]+)"""))
               yield return (name, open.Groups[1].Value);

            if (name == "StatusView.xaml.cs")
            {
               foreach (Match check in Regex.Matches(source, @"AddWarning\(StatusLevel\.\w+,[^;]*,\s*""([\w-]+)""\);"))
                  yield return (name, check.Groups[1].Value);
            }
         }
      }

      private static string ControlPanelFile(string relative)
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", relative);
            if (File.Exists(candidate))
               return candidate;
            directory = directory.Parent;
         }

         Assert.Fail("ControlPanel/" + relative.Replace('\\', '/') + " could not be found from the test binaries.");
         return null;
      }
   }
}
