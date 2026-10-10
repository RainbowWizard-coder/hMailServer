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
   /// A name with underscores in it - a TLS 1.3 suite, a metric - is written in every
   /// language as the English writes it. The Japanese catalogue read the first
   /// underscore of TLS_AES_256_GCM_SHA384 and of hmailserver_messagestore_missing_files
   /// as an Alt key: it wrote TLSAES_256_GCM_SHA384 and hmailservermessagestore_missing_files,
   /// and ended the two texts, a settings blurb and a notice that no access key reaches,
   /// in a "(_A)" and a "(_M)" drawn as they are (review finding F302's catalogue).
   /// </summary>
   public class CatalogueIdentifierTests
   {
      private static readonly Regex Identifier = new(@"(?<![\w%])[A-Za-z0-9]+(?:_[A-Za-z0-9]+){2,}(?!\w)");

      [Fact]
      public void ANameWithUnderscores_IsWrittenAsTheEnglishWritesIt()
      {
         string resources = Path.GetDirectoryName(ConfirmationPatternTests.ControlPanelFile(Path.Join("Resources", "Strings.resx")));
         List<string> english = Keys(Path.Join(resources, "Strings.resx")).Keys.ToList();
         var found = new List<string>();
         int catalogues = 0;
         foreach (string file in Directory.GetFiles(resources, "Strings*.resx").Where(f => !string.Equals(Path.GetFileName(f), "Strings.resx", StringComparison.OrdinalIgnoreCase)))
         {
            catalogues++;
            Dictionary<string, string> values = Keys(file);
            foreach (string key in english)
            {
               if (!values.TryGetValue(key, out string value))
                  continue;
               foreach (Match name in Identifier.Matches(key))
               {
                  if (!value.Contains(name.Value, StringComparison.Ordinal))
                     found.Add(Path.GetFileName(file) + ": " + name.Value + " in " + value);
               }
            }
         }

         Assert.Equal(17, catalogues);
         Assert.True(found.Count == 0, "A name with underscores written otherwise than the English writes it:" + Environment.NewLine + string.Join(Environment.NewLine, found));
      }

      private static Dictionary<string, string> Keys(string path)
         => XDocument.Load(path).Root.Elements("data")
            .ToDictionary(d => (string)d.Attribute("name"), d => d.Element("value")?.Value ?? "", StringComparer.Ordinal);
   }
}
