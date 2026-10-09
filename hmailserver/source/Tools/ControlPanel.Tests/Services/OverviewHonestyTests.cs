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
   /// The overview pages say what they do (review finding F234). The transport
   /// encryption, spam and virus overviews promised "each row opens the page that
   /// owns the setting" where no row can be clicked - the button at each section's
   /// foot is what opens a page - and the transport encryption overview showed
   /// 0.0.0.0 under every listener, as if it were an address.
   /// </summary>
   [Collection("Loc")]
   public class OverviewHonestyTests : IDisposable
   {
      public void Dispose() => Loc.Reset();

      [Theory]
      [InlineData("0.0.0.0", "All addresses")]
      [InlineData("::", "All addresses")]
      [InlineData("[::]", "All addresses")]
      [InlineData("", "All addresses")]
      [InlineData(null, "All addresses")]
      [InlineData(" 0.0.0.0 ", "All addresses")]
      [InlineData("192.0.2.7", "192.0.2.7")]
      [InlineData("2001:db8::25", "2001:db8::25")]
      public void AWildcardBindingReadsAsEveryAddress(string bound, string shown)
      {
         Assert.Equal(shown, TlsPosture.ListenerAddress(bound));
      }

      [Fact]
      public void TheWildcardIsTranslated()
      {
         Dictionary<string, string> german = ReadResx(Path.Join(ResourcesDirectory(), "Strings.de.resx"));
         Loc.Use((text, culture) => german.TryGetValue(text, out string value) ? value : null, CultureInfo.GetCultureInfo("de-DE"));

         Assert.Equal("Alle Adressen", TlsPosture.ListenerAddress("0.0.0.0"));
         Assert.Equal("192.0.2.7", TlsPosture.ListenerAddress("192.0.2.7"));
      }

      [Theory]
      [InlineData("TlsOverviewView.cs")]
      [InlineData("SpamOverviewView.cs")]
      [InlineData("VirusOverviewView.cs")]
      public void NoOverviewPromisesRowsThatOpenAPage(string view)
      {
         string source = ReadSource(view);
         Assert.DoesNotContain("each row opens", source, StringComparison.Ordinal);
         Assert.Contains("each section has a button to the page that owns its settings.", source, StringComparison.Ordinal);
      }

      private static Dictionary<string, string> ReadResx(string path)
      {
         var result = new Dictionary<string, string>(StringComparer.Ordinal);
         foreach (XElement data in XDocument.Load(path).Root.Elements("data"))
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

      private static string ReadSource(string view)
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", "Views", view);
            if (File.Exists(candidate))
               return File.ReadAllText(candidate);
            directory = directory.Parent;
         }

         Assert.Fail("ControlPanel/Views/" + view + " could not be found from the test binaries.");
         return null;
      }
   }
}
