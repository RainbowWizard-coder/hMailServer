// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// No member of the Control Panel's source carries two summaries. A member put
   /// between a summary and the member it was written for takes that summary as
   /// well as its own, and leaves the other with none: ComNoticeRow went in under
   /// ComInert's account of a setting the server never reads, so that IntelliSense
   /// and a reader showed each class the other's documentation;
   /// VirusPipeline.WhenNoScannerAnswers had taken Notes' summary in the same way,
   /// and a SpamPipelineTests theory the summary of the test after it.
   ///
   /// Scanned rather than compiled: the C# compiler takes the two summaries of a
   /// member without a word.
   /// </summary>
   public class DocCommentTests
   {
      /// <summary>
      /// The members that carried two summaries when this test was written, in
      /// files other pages' work was changing at the time, each once. A file
      /// leaves the list when its member is put right; no file joins it.
      /// </summary>
      private static readonly Dictionary<string, int> KnownBefore = new(StringComparer.OrdinalIgnoreCase)
      {
         ["ControlPanel/Services/NavigationMap.cs"] = 1,
         ["ControlPanel/Views/AccessibleChartCard.cs"] = 1,
         ["ControlPanel/Views/AccountDialog.cs"] = 1,
         ["ControlPanel/Views/LdapSettingsView.cs"] = 1,
         ["ControlPanel.Tests/Services/ApiKeyStoreTests.cs"] = 1
      };

      [Fact]
      public void NoMember_CarriesTwoSummaries()
      {
         string tools = ToolsFolder();
         var found = new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase);
         var where = new List<string>();

         foreach (string project in new[] { "ControlPanel", "ControlPanel.Core", "ControlPanel.Tests" })
         {
            string root = Path.Join(tools, project);
            if (!Directory.Exists(root))
               continue;

            foreach (string file in Directory.EnumerateFiles(root, "*.cs", SearchOption.AllDirectories))
            {
               string relative = Path.GetRelativePath(tools, file).Replace('\\', '/');
               if (relative.Contains("/bin/", StringComparison.Ordinal) || relative.Contains("/obj/", StringComparison.Ordinal))
                  continue;

               string[] lines = File.ReadAllLines(file);
               for (int i = 0; i + 1 < lines.Length; i++)
               {
                  if (lines[i].Trim() == "/// </summary>" && lines[i + 1].TrimStart().StartsWith("/// <summary>", StringComparison.Ordinal))
                  {
                     found[relative] = found.GetValueOrDefault(relative) + 1;
                     where.Add(relative + ":" + (i + 2));
                  }
               }
            }
         }

         var unexpected = found.Where(f => f.Value > KnownBefore.GetValueOrDefault(f.Key)).Select(f => f.Key).ToList();
         Assert.True(unexpected.Count == 0, "A member carries two summaries: " + string.Join(", ", where.Where(w => unexpected.Any(u => w.StartsWith(u + ":", StringComparison.OrdinalIgnoreCase)))));

         var putRight = KnownBefore.Where(k => found.GetValueOrDefault(k.Key) < k.Value).Select(k => k.Key).ToList();
         Assert.True(putRight.Count == 0, "Put right, so leave DocCommentTests.KnownBefore: " + string.Join(", ", putRight));
      }

      private static string ToolsFolder()
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            if (File.Exists(Path.Join(directory.FullName, "ControlPanel", "Views", "ServerSettingsView.xaml.cs")))
               return directory.FullName;
            directory = directory.Parent;
         }

         Assert.Fail("The Control Panel's source could not be found from the test binaries.");
         return null;
      }
   }
}
