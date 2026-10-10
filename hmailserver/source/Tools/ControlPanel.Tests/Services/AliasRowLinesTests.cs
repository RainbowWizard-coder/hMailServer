// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Linq;
using System.Xml.Linq;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// An alias row on the Domains &amp; accounts page is two lines, each trimmed with
   /// its own tool tip: the alias, and under it in the secondary text the address it
   /// delivers to (review finding F067). As one trimmed line, "name  →  target", the
   /// target was the part the ellipsis took at 1280 pixels - "info@example.com  →
   /// user00@ex…" - and a long alias lost the arrow as well; the target is the half
   /// an administrator looks up.
   /// </summary>
   public class AliasRowLinesTests
   {
      private static readonly XNamespace Wpf = "http://schemas.microsoft.com/winfx/2006/xaml/presentation";
      private static readonly XNamespace X = "http://schemas.microsoft.com/winfx/2006/xaml";

      [Fact]
      public void AnAliasRow_IsTheAliasAndUnderItItsTarget_EachTrimmed()
      {
         XDocument page = XDocument.Parse(Read(Path.Join("Views", "DomainsView.xaml")));
         XElement list = page.Descendants().Single(e => (string)e.Attribute(X + "Name") == "AliasList");
         XElement template = list.Descendants(Wpf + "DataTemplate").Single();

         var lines = template.Descendants(Wpf + "TextBlock").ToList();
         Assert.Equal(2, lines.Count);
         Assert.Equal("{Binding Name}", (string)lines[0].Attribute("Text"));
         Assert.Equal("{Binding Target}", (string)lines[1].Attribute("Text"));
         Assert.Equal(Wpf + "StackPanel", lines[0].Parent.Name);
         Assert.Same(lines[0].Parent, lines[1].Parent);
         foreach (XElement line in lines)
            Assert.Equal("{StaticResource GridTrimmedText}", (string)line.Attribute("Style"));
         Assert.Equal("{DynamicResource TextFillColorPrimaryBrush}", (string)lines[0].Attribute("Foreground"));
         Assert.Equal("{DynamicResource TextFillColorSecondaryBrush}", (string)lines[1].Attribute("Foreground"));

         // The row's Delete stays a 24-pixel box in the middle of the taller row.
         XElement delete = template.Descendants().Single(e => e.Name.LocalName == "Button");
         Assert.Equal("Center", (string)delete.Attribute("VerticalAlignment"));
      }

      [Fact]
      public void AnAliasRow_CarriesItsTarget_AndSaysBothToAScreenReader()
      {
         string code = Read(Path.Join("Views", "DomainsView.xaml.cs"));
         int start = code.IndexOf("public class AliasRow", StringComparison.Ordinal);
         Assert.True(start >= 0, "DomainsView no longer has AliasRow.");
         string row = code.Substring(start, code.IndexOf("private dynamic OpenSelectedDomain", start, StringComparison.Ordinal) - start);
         Assert.Contains("public string Value { get; set; }", row, StringComparison.Ordinal);
         Assert.Contains("public string Target => \"→ \" + Value;", row, StringComparison.Ordinal);
         Assert.Contains("public override string ToString() => Display;", row, StringComparison.Ordinal);
         Assert.Contains("Display = name + \"  →  \" + value", code, StringComparison.Ordinal);
      }

      private static string Read(string relative)
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", relative);
            if (File.Exists(candidate))
               return File.ReadAllText(candidate).Replace("\r\n", "\n");
            directory = directory.Parent;
         }

         Assert.Fail("ControlPanel/" + relative.Replace('\\', '/') + " could not be found from the test binaries.");
         return null;
      }
   }
}
