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
   /// The rows of the domains page as the contrast review left them. An inactive
   /// domain or account used to be dimmed whole, at 0.55, with the word
   /// "inactive" in italics inside the dimmed part: the word was the least
   /// legible text on the page (2.4:1 in the light theme) and the name measured
   /// 3.9:1. Now only the glyph dims, the name takes the secondary text, and the
   /// word is a Normal StatusPill at full opacity. And the four row deletes - the
   /// domain's and the alias's and the list's trash glyphs, the account's word -
   /// drew the danger colour as a local Foreground (3.3 to 3.5:1 on the button in
   /// the dark theme, 1.8:1 on the selected row) or no danger colour at all; every
   /// one is a DangerTextButton now, as docs/ControlPanelDesign.md prescribes.
   /// Each test fails against the page as it was.
   /// </summary>
   public class DomainsRowTests
   {
      private static readonly XNamespace Wpf = "http://schemas.microsoft.com/winfx/2006/xaml/presentation";
      private static readonly XNamespace X = "http://schemas.microsoft.com/winfx/2006/xaml";
      private static readonly XNamespace Ui = "http://schemas.lepo.co/wpfui/2022/xaml";
      private static readonly XNamespace Scaffold = "clr-namespace:hMailServer.ControlPanel.Views.Scaffold";

      [Fact]
      public void EveryRowDelete_IsADangerTextButton_WithNoLocalColour()
      {
         XElement page = Page();
         List<XElement> deletes = page.Descendants(Ui + "Button")
            .Where(b => ((string)b.Attribute("Click") ?? "").StartsWith("Delete", StringComparison.Ordinal))
            .ToList();

         Assert.Equal(new[] { "DeleteDomain_Click", "DeleteAccount_Click", "DeleteAlias_Click", "DeleteDistList_Click" },
            deletes.Select(b => (string)b.Attribute("Click")));

         foreach (XElement delete in deletes)
         {
            Assert.Equal("{StaticResource DangerTextButton}", (string)delete.Attribute("Style"));
            Assert.Null(delete.Attribute("Foreground"));
            Assert.Null(delete.Attribute("Appearance"));
         }
      }

      [Fact]
      public void NothingOnThePage_PaintsTheDangerColourItself()
      {
         foreach (XAttribute attribute in Page().Descendants().Attributes())
            Assert.DoesNotContain("AppDangerBrush", attribute.Value, StringComparison.Ordinal);
      }

      [Theory]
      [InlineData("DomainList")]
      [InlineData("AccountList")]
      public void AnInactiveRow_SaysSoInANormalPill_AfterItsName(string list)
      {
         XElement template = ItemTemplate(Page(), list);

         XElement pill = Assert.Single(template.Descendants(Scaffold + "StatusPill"));
         Assert.Equal("Normal", (string)pill.Attribute("Level"));
         Assert.Equal("{loc:L 'Inactive'}", (string)pill.Attribute("Text"));
         Assert.Equal("Right", (string)pill.Attribute("DockPanel.Dock"));
         Assert.Equal("{StaticResource RowInactivePill}", (string)pill.Attribute("Style"));
         Assert.Null(pill.Attribute("Opacity"));

         // The italic word is gone, and no text in the row is set in the secondary colour by hand.
         Assert.DoesNotContain(template.Descendants(Wpf + "TextBlock"), t => ((string)t.Attribute("Text") ?? "").Contains("inactive", StringComparison.Ordinal));
      }

      [Fact]
      public void ThePill_IsShownOnlyWhileTheRowIsInactive()
      {
         XElement style = Resource(Page(), "RowInactivePill");
         Assert.Equal("scaffold:StatusPill", (string)style.Attribute("TargetType"));
         Assert.Equal("{StaticResource {x:Type scaffold:StatusPill}}", (string)style.Attribute("BasedOn"));
         Assert.Equal("Collapsed", SetterValue(style.Elements(Wpf + "Setter"), "Visibility"));

         XElement trigger = Assert.Single(style.Element(Wpf + "Style.Triggers").Elements(Wpf + "DataTrigger"));
         Assert.Equal("{Binding Active}", (string)trigger.Attribute("Binding"));
         Assert.Equal("False", (string)trigger.Attribute("Value"));
         Assert.Equal("Visible", SetterValue(trigger.Elements(Wpf + "Setter"), "Visibility"));
      }

      [Theory]
      [InlineData("DomainList")]
      [InlineData("AccountList")]
      public void OnlyTheGlyph_Dims_WhenTheRowIsInactive(string list)
      {
         XElement page = Page();
         XElement template = ItemTemplate(page, list);

         // The one opacity setter on the page is the glyph's, in a DataTrigger on Active.
         List<XElement> opacity = page.Descendants(Wpf + "Setter").Where(s => (string)s.Attribute("Property") == "Opacity").ToList();
         XElement setter = Assert.Single(opacity);
         XElement trigger = setter.Parent;
         Assert.Equal(Wpf + "DataTrigger", trigger.Name);
         Assert.Equal("{Binding Active}", (string)trigger.Attribute("Binding"));
         Assert.Equal("False", (string)trigger.Attribute("Value"));
         XElement style = trigger.Parent.Parent;
         Assert.Equal("RowGlyphDimsWhenInactive", (string)style.Attribute(X + "Key"));
         Assert.Equal("Border", (string)style.Attribute("TargetType"));

         // And the element that takes it holds the glyph alone: no name, no word.
         XElement glyphHost = Assert.Single(template.Descendants(Wpf + "Border"), b => (string)b.Attribute("Style") == "{StaticResource RowGlyphDimsWhenInactive}");
         Assert.Single(glyphHost.Elements(), e => e.Name == Ui + "SymbolIcon");
         Assert.Empty(glyphHost.Descendants(Wpf + "TextBlock"));
         Assert.Empty(glyphHost.Descendants(Scaffold + "StatusPill"));
         Assert.DoesNotContain(template.Descendants(Wpf + "DockPanel"), d => d.Element(Wpf + "DockPanel.Style") != null);
      }

      [Theory]
      [InlineData("DomainList", "Name")]
      [InlineData("AccountList", "Address")]
      public void TheName_StaysLegible_InTheSecondaryTextWhenInactive(string list, string property)
      {
         XElement page = Page();
         XElement template = ItemTemplate(page, list);

         XElement name = Assert.Single(template.Descendants(Wpf + "TextBlock"), t => (string)t.Attribute("Text") == "{Binding " + property + "}");
         Assert.Equal("{StaticResource RowName}", (string)name.Attribute("Style"));
         Assert.Null(name.Attribute("Foreground"));
         Assert.Null(name.Attribute("Opacity"));

         XElement style = Resource(page, "RowName");
         Assert.Equal("{StaticResource GridTrimmedText}", (string)style.Attribute("BasedOn"));
         Assert.Equal("{DynamicResource TextFillColorPrimaryBrush}", SetterValue(style.Elements(Wpf + "Setter"), "Foreground"));
         XElement trigger = Assert.Single(style.Element(Wpf + "Style.Triggers").Elements(Wpf + "DataTrigger"));
         Assert.Equal("{Binding Active}", (string)trigger.Attribute("Binding"));
         Assert.Equal("{DynamicResource TextFillColorSecondaryBrush}", SetterValue(trigger.Elements(Wpf + "Setter"), "Foreground"));
      }

      // ---- the page ------------------------------------------------------------

      private static XElement Page()
      {
         for (var directory = new DirectoryInfo(AppContext.BaseDirectory); directory != null; directory = directory.Parent)
         {
            string path = Path.Join(directory.FullName, "ControlPanel", "Views", "DomainsView.xaml");
            if (File.Exists(path))
               return XDocument.Load(path).Root;
         }

         throw new FileNotFoundException("DomainsView.xaml was not found above the test binaries.");
      }

      private static XElement ItemTemplate(XElement page, string list)
      {
         XElement view = page.Descendants().Single(e => (string)e.Attribute(X + "Name") == list);
         return view.Element(Wpf + "ListView.ItemTemplate").Element(Wpf + "DataTemplate");
      }

      private static XElement Resource(XElement page, string key)
      {
         XElement style = page.Element(Wpf + "UserControl.Resources").Elements(Wpf + "Style").SingleOrDefault(s => (string)s.Attribute(X + "Key") == key);
         Assert.True(style != null, "the page has no resource " + key + ".");
         return style;
      }

      private static string SetterValue(IEnumerable<XElement> setters, string property)
         => (string)setters.Single(s => (string)s.Attribute("Property") == property).Attribute("Value");
   }
}
