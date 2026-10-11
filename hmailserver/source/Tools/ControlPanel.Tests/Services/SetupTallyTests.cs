// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
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

      /// <summary>
      /// The sentences say the pills' words in German too: the Dashboard's card showed
      /// the pill Handlungsbedarf beside "10 Punkte erfordern eine Maßnahme." and the
      /// checklist its Fertig pills beside "9 Punkte sind erledigt.", the finding's own
      /// two names for one state in the reader's language.
      /// </summary>
      [Fact]
      public void InGerman_TheSentencesSayThePillsWords()
      {
         Dictionary<string, string> german = ReadResx("Strings.de.resx");
         Assert.Equal("Handlungsbedarf", german["Action needed"]);
         Assert.Equal("Fertig", german["Done"]);
         Assert.Equal("Nicht nötig", german["Not needed"]);

         Loc.Use((text, culture) => german.TryGetValue(text, out string value) ? value : null, CultureInfo.GetCultureInfo("de"));
         Assert.Equal("Bei 10 Punkten besteht Handlungsbedarf. 1 Punkt kann von hier aus nicht geprüft werden.", SetupTally.Summary(10, 1, 25));
         Assert.Equal("Bei 1 Punkt besteht Handlungsbedarf.", SetupTally.Summary(1, 0, 25));
         Assert.Equal("Kein Handlungsbedarf, aber 1 Punkt kann von hier aus nicht geprüft werden.", SetupTally.Summary(0, 1, 25));
         Assert.Equal("Kein Handlungsbedarf, aber 2 Punkte können von hier aus nicht geprüft werden.", SetupTally.Summary(0, 2, 25));
         Assert.Equal("Kein Handlungsbedarf - alle 25 Prüfungen der externen Einrichtung sind fertig oder nicht nötig.", SetupTally.Summary(0, 0, 25));
         Assert.Equal("Kein Handlungsbedarf - 1 Prüfung der externen Einrichtung ist fertig oder nicht nötig.", SetupTally.Summary(0, 0, 1));
         Assert.Equal("Bei 4 Punkten besteht Handlungsbedarf. 1 Punkt ist fertig. Nächster Schritt: SPF.", SetupTally.Tally(4, 0, 1, 0, "SPF"));
         Assert.Equal("Kein Handlungsbedarf. 9 Punkte sind fertig. 4 Punkte sind nicht nötig.", SetupTally.Tally(0, 0, 9, 4, null));
      }

      /// <summary>
      /// Every language says that nothing needs action in one phrase, the checklist's
      /// "Nothing needs action." and the card's sentences that go on from it alike:
      /// the card's "Nothing needs action, but ..." was a text from before the tally,
      /// which German, Japanese, Chinese, Russian, Ukrainian and Turkish said in other
      /// words than the tally's own sentence beside it.
      /// </summary>
      [Fact]
      public void InEveryLanguage_NothingNeedsAction_IsOnePhrase()
      {
         string[] goOn =
         {
            "Nothing needs action - the {0} external setup check is done or not needed.",
            "Nothing needs action - all {0} external setup checks are done or not needed.",
            "Nothing needs action, but {0} item cannot be checked from here.",
            "Nothing needs action, but {0} items cannot be checked from here.",
         };
         var failures = new List<string>();
         foreach (string path in Directory.GetFiles(ResourcesDirectory(), "Strings.*.resx"))
         {
            Dictionary<string, string> catalogue = ReadResx(Path.GetFileName(path));
            string phrase = catalogue["Nothing needs action."].TrimEnd('.', '\u3002');
            foreach (string key in goOn.SelectMany(k => new[] { k, k + "|few" }).Where(catalogue.ContainsKey))
            {
               if (!catalogue[key].StartsWith(phrase, StringComparison.Ordinal))
                  failures.Add(Path.GetFileName(path) + ": " + key + " = " + catalogue[key] + ", beside " + phrase);
            }
         }

         Assert.True(failures.Count == 0, string.Join(Environment.NewLine, failures));
      }

      [Fact]
      public void BothPages_ReadTheirSentencesFromTheTally()
      {
         string dashboard = ReadView("DashboardView.xaml.cs");
         string summary = WithheldRowTests.MemberText(dashboard, "private static string SummaryText_(int action, int unknown, int total)");
         Assert.True(summary != null, "DashboardView.SummaryText_ is gone.");
         Assert.Contains("SetupTally.Summary(action, unknown, total)", summary, StringComparison.Ordinal);

         string checklist = ReadView("ExternalSetupView.cs");
         Assert.Contains("SetupTally.Tally(action, unknown, done, unused, next?.Title));", checklist, StringComparison.Ordinal);

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
