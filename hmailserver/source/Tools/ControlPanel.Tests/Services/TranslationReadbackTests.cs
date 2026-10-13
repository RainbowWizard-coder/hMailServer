// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Xml.Linq;
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
   public class TranslationReadbackTests
   {
      [Theory]
      [InlineData("cs", "Sendout queued {0}.", "Hromadné odeslání bylo zařazeno do fronty v {0}.")]
      [InlineData("da", "Sendout queued {0}.", "Udsendelsen blev sat i kø kl. {0}.")]
      [InlineData("nb", "Sendout queued {0}.", "Utsendingen ble lagt i kø kl. {0}.")]
      [InlineData("tr", "Sendout queued {0}.", "Toplu gönderim {0} saatinde kuyruğa alındı.")]
      [InlineData("ja", "Sendout queued {0}.", "{0} に一斉送信をキューに入れました。")]
      [InlineData("fi", "The cipher list could not be read.", "Salauslistaa ei voitu lukea.")]
      [InlineData("uk", "No listeners are configured.", "Слухачів не налаштовано.")]
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
