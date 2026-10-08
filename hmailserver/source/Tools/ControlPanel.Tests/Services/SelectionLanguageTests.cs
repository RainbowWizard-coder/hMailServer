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
   /// Selection is one language across the application, the sidebar's: the
   /// selected row of every grid, list box and list view takes the selection
   /// fill (AppSelectionFillBrush) and the brand pill, hover keeps the subtle
   /// grey, and under High Contrast an outline in the highlight draws the row.
   /// Before this a selected grid row was the hover grey (1.08:1 against its
   /// neighbours, exactly as a hovered one), the palette and the bare list
   /// boxes drew WPF-UI's solid accent band - the palette's own text at 1.9 to
   /// 3.5:1 on it - and the domains page's lists drew its grey with an
   /// outline. These read App.xaml's row styles and hold them to the one look,
   /// so that a row style that goes back to the grey for selection, or a list
   /// that is given a band of its own, fails here.
   /// </summary>
   public class SelectionLanguageTests
   {
      private static readonly XNamespace Wpf = "http://schemas.microsoft.com/winfx/2006/xaml/presentation";
      private static readonly XNamespace X = "http://schemas.microsoft.com/winfx/2006/xaml";

      [Fact]
      public void TheGridRow_IsFilledWithTheSelectionFillWhenSelected_AndTheSubtleGreyUnderThePointer()
      {
         XElement style = ImplicitStyle(App(), "DataGridRow");
         List<XElement> triggers = style.Element(Wpf + "Style.Triggers").Elements(Wpf + "Trigger").ToList();

         XElement hover = Assert.Single(triggers, t => (string)t.Attribute("Property") == "IsMouseOver");
         Assert.Equal("{DynamicResource SubtleFillColorSecondaryBrush}", SetterValue(hover, "Background"));

         XElement selected = Assert.Single(triggers, t => (string)t.Attribute("Property") == "IsSelected");
         Assert.Equal("{DynamicResource AppSelectionFillBrush}", SetterValue(selected, "Background"));

         // The selected trigger comes after the hover one, so that a selected row keeps its fill under the pointer.
         Assert.True(triggers.IndexOf(selected) > triggers.IndexOf(hover), "IsSelected must follow IsMouseOver, or hover paints over the selection.");
      }

      [Fact]
      public void TheGridRow_DrawsTheBrandPillAndTheHighContrastOutline_OnTheSelectedRow()
      {
         XElement template = Template(ImplicitStyle(App(), "DataGridRow"));

         XElement pill = Named(template, "Pill");
         Assert.Equal(Wpf + "Rectangle", pill.Name);
         Assert.Equal("{DynamicResource AppBrandBrush}", (string)pill.Attribute("Fill"));
         Assert.Equal("3", (string)pill.Attribute("Width"));
         Assert.Equal("16", (string)pill.Attribute("Height"));
         Assert.Equal("0", (string)pill.Attribute("Opacity"));

         XElement outline = Named(template, "Outline");
         Assert.Equal("{DynamicResource AppSelectionOutlineBrush}", (string)outline.Attribute("Stroke"));
         Assert.Equal("Collapsed", (string)outline.Attribute("Visibility"));

         XElement selected = TemplateTrigger(template, "IsSelected");
         Assert.Equal("1", TargetSetterValue(selected, "Pill", "Opacity"));
         Assert.Equal("Visible", TargetSetterValue(selected, "Outline", "Visibility"));

         // WPF's own row parts are still there: the cells, the details, the hidden row header.
         Assert.NotNull(template.Descendants(Wpf + "DataGridCellsPresenter").SingleOrDefault());
         Assert.NotNull(template.Descendants(Wpf + "DataGridDetailsPresenter").SingleOrDefault());
         Assert.NotNull(template.Descendants(Wpf + "DataGridRowHeader").SingleOrDefault());
      }

      [Fact]
      public void TheListRow_IsImplicit_AndDrawsTheSameSelection()
      {
         XElement style = ImplicitStyle(App(), "ListBoxItem");
         XElement template = Template(style);

         // The stock focus visual gives way to the ring in the focus colour, as the sidebar row's border.
         Assert.Equal("{x:Null}", StyleSetterValue(style, "FocusVisualStyle"));
         Assert.Equal("{DynamicResource AppNavRowHeight}", StyleSetterValue(style, "MinHeight"));

         XElement pill = Named(template, "Pill");
         Assert.Equal("{DynamicResource AppBrandBrush}", (string)pill.Attribute("Fill"));

         XElement hover = TemplateTrigger(template, "IsMouseOver");
         Assert.Equal("{DynamicResource SubtleFillColorSecondaryBrush}", TargetSetterValue(hover, "Bd", "Background"));

         XElement selected = TemplateTrigger(template, "IsSelected");
         Assert.Equal("{DynamicResource AppSelectionFillBrush}", TargetSetterValue(selected, "Bd", "Background"));
         Assert.Equal("1", TargetSetterValue(selected, "Pill", "Opacity"));
         Assert.Equal("{DynamicResource AppSelectionOutlineBrush}", TargetSetterValue(selected, "Ring", "Stroke"));

         XElement focused = TemplateTrigger(template, "IsKeyboardFocused");
         Assert.Equal("{DynamicResource AppFocusBrush}", TargetSetterValue(focused, "Ring", "Stroke"));

         // No solid band anywhere in the row: the fill is the token's, never an accent.
         foreach (XElement setter in template.Descendants(Wpf + "Setter").Where(s => (string)s.Attribute("Property") == "Background"))
            Assert.DoesNotContain("Accent", (string)setter.Attribute("Value"), StringComparison.Ordinal);
      }

      [Fact]
      public void TheListViewRow_IsTheListRow()
      {
         XElement style = ImplicitStyle(App(), "ListViewItem");
         Assert.Equal("{StaticResource {x:Type ListBoxItem}}", (string)style.Attribute("BasedOn"));
         Assert.Null(style.Element(Wpf + "Setter.Template"));
         Assert.DoesNotContain(style.Elements(Wpf + "Setter"), s => (string)s.Attribute("Property") == "Template");
      }

      /// <summary>
      /// The palette's rows take the implicit list row and keep their TextBody
      /// and TextCaption: on the stock accent band those styles' pinned colours
      /// measured 3.55:1 and 2.68:1 in the light theme, 2.30:1 and 1.87:1 in the
      /// dark. A style of the palette's own for its items, or a colour of its
      /// own for a selected row, would be the way back.
      /// </summary>
      [Fact]
      public void ThePalette_TakesTheListRow_AndKeepsItsTextStyles()
      {
         string source = ReadControlPanelFile(Path.Join("Views", "NavigationPalette.cs"));
         if (source == null)
            return;

         Assert.DoesNotContain("ItemContainerStyle", source, StringComparison.Ordinal);
         Assert.DoesNotContain("SelectedBackground", source, StringComparison.Ordinal);
         Assert.DoesNotContain("HighlightBrush", source, StringComparison.Ordinal);
         Assert.Contains("SetResourceReference(StyleProperty, \"TextBody\")", source, StringComparison.Ordinal);
         Assert.Contains("SetResourceReference(StyleProperty, \"TextCaption\")", source, StringComparison.Ordinal);
      }

      /// <summary>
      /// The lists that drew the accent band sit on a card with the compact
      /// inset and no box of their own, as the public folders list does, so the
      /// row's fill and pill are what marks the selection.
      /// </summary>
      [Theory]
      [InlineData("GroupMembersDialog.cs")]
      [InlineData("FolderPermissionsDialog.cs")]
      [InlineData("RecipientsDialog.cs")]
      [InlineData("PublicFoldersView.cs")]
      public void TheBareLists_SitOnACard_WithNoBoxOfTheirOwn(string file)
      {
         string source = ReadControlPanelFile(Path.Join("Views", file));
         if (source == null)
            return;

         Assert.Contains("new Card", source, StringComparison.Ordinal);
         Assert.Contains("BorderThickness = new Thickness(0)", source, StringComparison.Ordinal);
         Assert.Contains("Brushes.Transparent", source, StringComparison.Ordinal);
      }

      // ---- App.xaml readers -------------------------------------------------------

      private static XElement App()
      {
         string xaml = ReadControlPanelFile("App.xaml");
         Assert.NotNull(xaml);
         return XDocument.Parse(xaml).Root;
      }

      private static XElement ImplicitStyle(XElement app, string targetType)
      {
         XElement style = app.Descendants(Wpf + "Style")
            .SingleOrDefault(s => (string)s.Attribute("TargetType") == targetType && s.Attribute(X + "Key") == null);
         Assert.True(style != null, "App.xaml has no implicit " + targetType + " style.");
         return style;
      }

      private static XElement Template(XElement style)
      {
         XElement setter = style.Elements(Wpf + "Setter").SingleOrDefault(s => (string)s.Attribute("Property") == "Template");
         Assert.True(setter != null, (string)style.Attribute("TargetType") + "'s style sets no template.");
         return setter.Element(Wpf + "Setter.Value").Element(Wpf + "ControlTemplate");
      }

      private static XElement Named(XElement template, string name)
      {
         XElement element = template.Descendants().SingleOrDefault(e => (string)e.Attribute(X + "Name") == name);
         Assert.True(element != null, "the template has no part named " + name + ".");
         return element;
      }

      private static XElement TemplateTrigger(XElement template, string property)
      {
         XElement trigger = template.Element(Wpf + "ControlTemplate.Triggers")?.Elements(Wpf + "Trigger")
            .SingleOrDefault(t => (string)t.Attribute("Property") == property && (string)t.Attribute("Value") == "True");
         Assert.True(trigger != null, "the template has no trigger on " + property + ".");
         return trigger;
      }

      private static string SetterValue(XElement trigger, string property)
         => (string)trigger.Elements(Wpf + "Setter").Single(s => (string)s.Attribute("Property") == property).Attribute("Value");

      private static string TargetSetterValue(XElement trigger, string target, string property)
      {
         XElement setter = trigger.Elements(Wpf + "Setter")
            .SingleOrDefault(s => (string)s.Attribute("TargetName") == target && (string)s.Attribute("Property") == property);
         Assert.True(setter != null, "no setter of " + target + "'s " + property + " in the trigger.");
         return (string)setter.Attribute("Value");
      }

      private static string StyleSetterValue(XElement style, string property)
         => (string)style.Elements(Wpf + "Setter").Single(s => (string)s.Attribute("Property") == property).Attribute("Value");

      private static string ReadControlPanelFile(string relativePath)
      {
         for (var directory = new DirectoryInfo(AppContext.BaseDirectory); directory != null; directory = directory.Parent)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", "MainWindow.xaml");
            if (File.Exists(candidate))
            {
               string path = Path.Join(directory.FullName, "ControlPanel", relativePath);
               return File.Exists(path) ? File.ReadAllText(path) : null;
            }
         }

         return null;
      }
   }
}
