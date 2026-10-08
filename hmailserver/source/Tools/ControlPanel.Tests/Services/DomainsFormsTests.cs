// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Xml.Linq;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The domains page's four add forms, and the distribution lists card's row
   /// buttons. In a 1280 by 800 window the detail column's floors are taller than
   /// the page, so the page scrolls, the domains card stretches to the column's
   /// height, and its Domain box and Add domain sat at the foot of the card in the
   /// hidden bottom 90 pixels - on an empty server the card said "No domains yet.
   /// Add the first one" with nothing to press on screen - and the alias and list
   /// forms were off screen too. And the lists card's rows read "everyone@e..."
   /// beside Properties and Members as 10-pixel words, the only 10-pixel text in
   /// the Control Panel. Every test here fails against that page: each form is at
   /// the top of its card, above the list, the empty domains state offers the next
   /// thing to do, the row's buttons are glyphs in 24-pixel boxes named for the
   /// row, and no text on the page is set below the 12-pixel caption size.
   /// </summary>
   public class DomainsFormsTests
   {
      private static readonly XNamespace Wpf = "http://schemas.microsoft.com/winfx/2006/xaml/presentation";
      private static readonly XNamespace X = "http://schemas.microsoft.com/winfx/2006/xaml";
      private static readonly XNamespace Scaffold = "clr-namespace:hMailServer.ControlPanel.Views.Scaffold";

      // Each card's add box and its list.
      public static TheoryData<string, string> Forms => new()
      {
         { "NewDomainBox", "DomainList" },
         { "NewAccountBox", "AccountList" },
         { "NewAliasName", "AliasList" },
         { "NewDistAddress", "DistList" },
      };

      [Theory]
      [MemberData(nameof(Forms))]
      public void EveryAddForm_IsAboveItsList_InTheSameFillPanel(string box, string list)
      {
         XElement page = Page();
         XElement form = Named(page, box);
         XElement view = Named(page, list);

         XElement panel = view.Ancestors(Scaffold + "FillPanel").First(p => p.Descendants().Contains(form));
         XElement formPart = PartOf(panel, form);
         XElement listPart = PartOf(panel, view);
         Assert.True(panel.Elements().ToList().IndexOf(formPart) < panel.Elements().ToList().IndexOf(listPart),
            box + " is under " + list + " in its card, where a short window hides it.");

         // Not the filling part: the form keeps its own height, the list takes the rest.
         Assert.Null(formPart.Attribute(Scaffold + "FillPanel.Fill"));
      }

      [Fact]
      public void TheEmptyDomainsState_OffersToAddADomain_WhichPutsTheKeyboardInTheBox()
      {
         XElement page = Page();
         XElement status = Named(page, "DomainStatus");
         XElement action = status.Element(Scaffold + "EmptyState.Action")?.Elements().SingleOrDefault();
         Assert.True(action != null, "DomainStatus has no action, so an empty server's first screen says what to do with nothing to press.");
         Assert.Equal("AddDomainAction_Click", (string)action.Attribute("Click"));

         string code = CodeBehind();
         string handler = WithheldRowTests.MemberText(code, "private void AddDomainAction_Click(object sender, RoutedEventArgs e)");
         Assert.True(handler != null && handler.Contains("NewDomainBox.Focus();", StringComparison.Ordinal), "The action does not put the keyboard in the Domain box.");
      }

      [Fact]
      public void TheListRowsButtons_AreGlyphsIn24PixelBoxes_NamedForTheRow()
      {
         XElement template = Named(Page(), "DistList").Descendants(Wpf + "DataTemplate").Single();
         List<XElement> buttons = template.Descendants().Where(e => e.Name.LocalName == "Button").ToList();
         Assert.Equal(3, buttons.Count);

         foreach (XElement button in buttons)
         {
            Assert.Null(button.Attribute("Content"));
            Assert.NotNull(button.Attribute("Icon"));
            Assert.True(Size(button, "MinWidth") >= 24 && Size(button, "MinHeight") >= 24, "A row button is smaller than 24 by 24.");
            Assert.NotNull(button.Attribute("ToolTip"));
            Assert.NotNull(button.Attribute("AutomationProperties.Name"));
         }

         // Properties and Members say which list they act on; the row is otherwise three identical glyphs to a screen reader.
         Assert.Equal("{Binding PropertiesName}", (string)buttons[0].Attribute("AutomationProperties.Name"));
         Assert.Equal("{Binding MembersName}", (string)buttons[1].Attribute("AutomationProperties.Name"));

         string code = CodeBehind();
         Assert.Contains("public string PropertiesName => F(\"Properties of {0}\", Address);", code, StringComparison.Ordinal);
         Assert.Contains("public string MembersName => F(\"Members of {0}\", Address);", code, StringComparison.Ordinal);
      }

      [Fact]
      public void NoText_IsSetBelowTheCaptionSize()
      {
         foreach (XElement e in Page().Descendants())
         {
            string size = (string)e.Attribute("FontSize");
            if (size == null)
               continue;
            Assert.True(double.Parse(size, CultureInfo.InvariantCulture) >= 12, e.Name.LocalName + " is set at " + size + " pixels, below the 12-pixel caption floor.");
         }
      }

      // ---- helpers -------------------------------------------------------------------------

      private static XElement PartOf(XElement panel, XElement inside)
         => panel.Elements().Single(part => part == inside || part.Descendants().Contains(inside));

      private static double Size(XElement e, string attribute)
         => double.Parse((string)e.Attribute(attribute) ?? "0", CultureInfo.InvariantCulture);

      private static XElement Page() => XDocument.Parse(ReadControlPanelFile(Path.Join("Views", "DomainsView.xaml"))).Root;

      private static string CodeBehind() => ReadControlPanelFile(Path.Join("Views", "DomainsView.xaml.cs"));

      private static XElement Named(XElement root, string name)
      {
         XElement found = root.Descendants().FirstOrDefault(e => (string)e.Attribute(X + "Name") == name);
         Assert.True(found != null, "DomainsView.xaml has no element named " + name + ".");
         return found;
      }

      private static string ReadControlPanelFile(string relative)
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", relative);
            if (File.Exists(candidate))
               return File.ReadAllText(candidate);
            directory = directory.Parent;
         }

         Assert.Fail("ControlPanel/" + relative.Replace('\\', '/') + " could not be found from the test binaries.");
         return null;
      }
   }
}
