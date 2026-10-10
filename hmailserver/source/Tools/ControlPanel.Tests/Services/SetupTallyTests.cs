// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Xml.Linq;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The Dashboard's setup card and the Setup checklist page count the setup
   /// checks' states in the same words, the words of the states' pills (review
   /// finding F034). The Dashboard said "11 things need attention, 1 item cannot be
   /// checked from here." and the checklist "11 need action, 1 cannot be told from
   /// here, 9 done, 4 not needed." beside pills that said Action needed and Cannot
   /// tell on both pages; the checklist's first clause had no noun. Both now read
   /// their sentences from Services/SetupTally, one sentence a state.
   /// </summary>
   public class SetupTallyTests : IDisposable
   {
      public void Dispose() => Loc.Reset();

      [Theory]
      [InlineData(11, 1, 25, "11 items need action. 1 item cannot be checked from here.")]
      [InlineData(1, 0, 25, "1 item needs action.")]
      [InlineData(2, 3, 25, "2 items need action. 3 items cannot be checked from here.")]
      [InlineData(0, 1, 25, "Nothing needs action, but 1 item cannot be checked from here.")]
      [InlineData(0, 2, 25, "Nothing needs action, but 2 items cannot be checked from here.")]
      [InlineData(0, 0, 25, "Nothing needs action - all 25 external setup checks are done or not needed.")]
      [InlineData(0, 0, 1, "Nothing needs action - the 1 external setup check is done or not needed.")]
      public void TheDashboardsCard_SaysWhatNeedsAction(int action, int unknown, int total, string expected)
      {
         Loc.Reset();
         Assert.Equal(expected, SetupTally.Summary(action, unknown, total));
      }

      [Theory]
      [InlineData(4, 7, 0, 4, "MTA-STS policy hosting",
         "4 items need action. 7 items cannot be checked from here. 4 items are not needed. Next step: MTA-STS policy hosting.")]
      [InlineData(11, 1, 9, 4, "Reverse DNS",
         "11 items need action. 1 item cannot be checked from here. 9 items are done. 4 items are not needed. Next step: Reverse DNS.")]
      [InlineData(1, 0, 1, 1, "SPF", "1 item needs action. 1 item is done. 1 item is not needed. Next step: SPF.")]
      [InlineData(0, 0, 21, 4, null, "Nothing needs action. 21 items are done. 4 items are not needed.")]
      [InlineData(0, 2, 19, 4, "", "Nothing needs action. 2 items cannot be checked from here. 19 items are done. 4 items are not needed.")]
      public void TheChecklistsTally_SaysEveryStateInASentenceOfItsOwn(int action, int unknown, int done, int notNeeded, string next, string expected)
      {
         Loc.Reset();
         Assert.Equal(expected, SetupTally.Tally(action, unknown, done, notNeeded, next));
      }

      /// <summary>
      /// One state, one sentence, in the forms its language has for the count: in
      /// Russian 3 takes the "few" form and 5 the other, which the catalogue holds as
      /// "|few" beside the key.
      /// </summary>
      [Fact]
      public void ACount_TakesItsLanguagesForm()
      {
         Dictionary<string, string> russian = ReadResx("Strings.ru.resx");
         Loc.Use((text, culture) => russian.TryGetValue(text, out string value) ? value : null, CultureInfo.GetCultureInfo("ru"));
         Assert.Equal("1 пункт требует действий.", SetupTally.NeedAction(1));
         Assert.Equal("3 пункта требуют действий.", SetupTally.NeedAction(3));
         Assert.Equal("5 пунктов требуют действий.", SetupTally.NeedAction(5));
         Assert.Equal("21 пункт требует действий.", SetupTally.NeedAction(21));
      }

      [Fact]
      public void BothPages_ReadTheirSentencesFromTheTally()
      {
         string dashboard = ReadView("DashboardView.xaml.cs");
         string summary = WithheldRowTests.MemberText(dashboard, "private static string SummaryText_(int action, int unknown, int total)");
         Assert.True(summary != null, "DashboardView.SummaryText_ is gone.");
         Assert.Contains("SetupTally.Summary(action, unknown, total)", summary, StringComparison.Ordinal);

         string checklist = ReadView("ExternalSetupView.cs");
         Assert.Contains("summary_.Text = SetupTally.Tally(action, unknown, done, unused, next?.Title);", checklist, StringComparison.Ordinal);

         // The old texts as literals, not as the comments that say what they were.
         foreach (string old in new[] { "\"{0} thing needs attention.\"", "\"{0} things need attention.\"", "\"Nothing needs attention - ", "\"{0} need action, {1} cannot be told from here" })
         {
            Assert.DoesNotContain(old, dashboard, StringComparison.Ordinal);
            Assert.DoesNotContain(old, checklist, StringComparison.Ordinal);
         }
      }

      private static Dictionary<string, string> ReadResx(string file)
      {
         var result = new Dictionary<string, string>(StringComparer.Ordinal);
         foreach (XElement data in XDocument.Load(Path.Join(ResourcesDirectory(), file)).Root.Elements("data"))
            result[(string)data.Attribute("name")] = data.Element("value")?.Value ?? "";
         return result;
      }

      private static string ResourcesDirectory()
      {
         string dir = AppContext.BaseDirectory;
         for (int i = 0; i < 8 && dir != null; i++)
         {
            string candidate = Path.Join(dir, "ControlPanel", "Resources");
            if (File.Exists(Path.Join(candidate, "Strings.resx")))
               return candidate;
            dir = Path.GetDirectoryName(dir);
         }

         Assert.Fail("ControlPanel/Resources could not be found from the test binaries.");
         return null;
      }

      private static string ReadView(string file)
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", "Views", file);
            if (File.Exists(candidate))
               return File.ReadAllText(candidate);
            directory = directory.Parent;
         }

         Assert.Fail("ControlPanel/Views/" + file + " could not be found from the test binaries.");
         return null;
      }
   }
}
