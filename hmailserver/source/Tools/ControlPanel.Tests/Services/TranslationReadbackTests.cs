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
   /// The read-back of 6.3.7 (batch53, row 53.6) read every explanatory text of the Czech, Danish,
   /// Finnish, Norwegian, Turkish, Ukrainian, Japanese and Chinese catalogues against its English and
   /// mended 1,218: what a text said wrongly (a sendout that queued {0} where {0} is the time it was
   /// queued), a second word for one thing on one page (a cipher list that was a salauslista and a
   /// salausluettelo), grammar (слухачі, an animate noun, as the object of не налаштовано), and a
   /// Chinese confirmation that stated where the English asks. build/check-catalogues.py's
   /// read-back rules find the kinds a rule can see again; these hold one mended text of each
   /// catalogue as it now reads, and the default IP ranges' names.
   /// </summary>
   [Collection("Loc")]
   public class TranslationReadbackTests : IDisposable
   {
      public void Dispose() => Loc.Reset();

      [Theory]
      [InlineData("cs", "Sendout queued {0}.", "Hromadné odeslání bylo zařazeno do fronty v {0}.")]
      [InlineData("da", "Sendout queued {0}.", "Udsendelsen blev sat i kø kl. {0}.")]
      [InlineData("nb", "Sendout queued {0}.", "Utsendingen ble lagt i kø kl. {0}.")]
      [InlineData("tr", "Sendout queued {0}.", "Toplu gönderim {0} saatinde kuyruğa alındı.")]
      [InlineData("ja", "Sendout queued {0}.", "{0} に一斉送信をキューに入れました。")]
      [InlineData("fi", "The cipher list could not be read.", "Salauslistaa ei voitu lukea.")]
      [InlineData("fi", "Only for an archive that was written with a passphrase. Without the right one the archive cannot be opened at all, and the restore is refused before anything on this server is touched.", "Vain arkistolle, joka on kirjoitettu tunnuslauseella. Ilman oikeaa tunnuslausetta arkistoa ei saa auki lainkaan, ja palautus torjutaan, ennen kuin tällä palvelimella kosketaan mihinkään.")]
      [InlineData("uk", "No listeners are configured.", "Слухачів не налаштовано.")]
      [InlineData("uk", "Set the connection security on each listener, then require it for authentication.", "Задайте захист підключення на кожному слухачі, потім вимагайте його для автентифікації.")]
      [InlineData("zh-Hans", "Stop the update? Nothing has been installed yet.", "是否停止更新？尚未安装任何内容。")]
      [InlineData("tr", "Alias addr_ess", "Di_ğer ad adresi")]
      public void AMendedTextReadsAsMended(string tag, string english, string expected)
      {
         Dictionary<string, string> catalogue = ReadCatalogueForReadback(tag);
         Assert.True(catalogue.Count > 5000, "The " + tag + " catalogue was not read.");
         Assert.Equal(expected, catalogue[english]);
      }

      /// <summary>
      /// The server names the two ranges it makes "My computer" and "Internet" (SecurityRanges.cpp),
      /// and the IP ranges page lists them by those names whatever the Control Panel's language, so a
      /// text that names them quotes them as the server wrote them. Six of these catalogues
      /// translated them - Můj počítač, Min computer, Oma tietokone, Min datamaskin, Bilgisayarım,
      /// Мій комп’ютер - and named two ranges the list does not show.
      /// </summary>
      [Fact]
      public void TheDefaultRangesKeepTheNamesTheServerGivesThem()
      {
         string[] tags = { "cs", "da", "fi", "nb", "tr", "uk", "ja", "zh-Hans" };
         var offenders = new List<string>();
         int texts = 0;
         foreach (string tag in tags)
         {
            Dictionary<string, string> catalogue = ReadCatalogueForReadback(tag);
            foreach (KeyValuePair<string, string> pair in catalogue.Where(pair => pair.Key.Contains("'My computer'", StringComparison.Ordinal)))
            {
               texts++;
               if (!pair.Value.Contains("My computer", StringComparison.Ordinal) || !pair.Value.Contains("Internet", StringComparison.Ordinal))
                  offenders.Add(tag + ": " + pair.Value.Split('\n')[0]);
            }
         }

         Assert.Equal(2 * tags.Length, texts);
         Assert.True(offenders.Count == 0, "A text translates the default ranges' names, which the server gives in English:\n" + string.Join("\n", offenders));
      }

      /// <summary>
      /// The quarantine asks about one message with a question of its own (its subject and sender,
      /// or "this message"), and about the checked messages, or a group's, with a plural pair whose
      /// "one" form it never shows for one (QuarantineView's ConfirmDelete_). Ukrainian says 21, 31
      /// and 101 as it says 1, so it shows that form for them: the read-back made its senders one
      /// sender told it was accepted (Відправнику ... його), which said of 21 messages that they had
      /// one sender, and the adversarial check of the read-back put the senders back.
      /// </summary>
      [Theory]
      [InlineData(2)]
      [InlineData(5)]
      [InlineData(21)]
      [InlineData(31)]
      [InlineData(101)]
      public void TheUkrainianDeleteQuestionsSpeakOfTheSendersForEveryCountTheyAreAskedFor(long count)
      {
         Dictionary<string, string> catalogue = ReadCatalogueForReadback("uk");
         Loc.Use((text, culture) => catalogue.TryGetValue(text, out string value) && value.Length > 0 ? value : null, CultureInfo.GetCultureInfo("uk"));

         string checkedMessages = Loc.Plural(count,
            "Delete the {0} checked message permanently?\n\nThe sender was told it was accepted, so nothing will retry and there is no other copy.",
            "Delete the {0} checked messages permanently?\n\nThe senders were told they were accepted, so nothing will retry and there is no other copy.");
         string groupMessages = Loc.PluralF(count,
            "Delete the {0} message from {1} permanently?\n\nThe sender was told it was accepted, so nothing will retry and there is no other copy.",
            "Delete the {0} messages from {1} permanently?\n\nThe senders were told they were accepted, so nothing will retry and there is no other copy.",
            count, "example.com");

         foreach (string question in new[] { checkedMessages, groupMessages })
         {
            Assert.StartsWith("Видалити " + count.ToString(CultureInfo.InvariantCulture) + " ", question);
            Assert.Contains("Відправникам сказали, що їх прийнято", question);
            Assert.DoesNotContain("Відправнику", question);
         }
      }

      private static Dictionary<string, string> ReadCatalogueForReadback(string tag)
      {
         var result = new Dictionary<string, string>(StringComparer.Ordinal);
         string path = ConfirmationPatternTests.ControlPanelFile(Path.Join("Resources", "Strings." + tag + ".resx"));
         foreach (XElement data in XDocument.Load(path).Root.Elements("data"))
            result[(string)data.Attribute("name")] = data.Element("value")?.Value ?? "";
         return result;
      }
   }
}
