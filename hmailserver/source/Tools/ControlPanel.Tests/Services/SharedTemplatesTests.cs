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
   /// The two shared templates in App.xaml that reached every tabbed page and every
   /// long caption: the tab strip, whose selected weight and 14 px were set on the
   /// TabItem itself and inherited by the tab's whole page, so that every caption,
   /// hint and editor value on an open tab came out SemiBold and a size the pages
   /// without tabs do not get; and the check box and radio button, whose string
   /// caption never wrapped and was cut mid-word at the edge of its card or dialog.
   /// WPF's layout is not rendered here - this assembly is WPF-free - so what is
   /// held is the XAML each fix rests on, read from App.xaml; each assertion names
   /// the regression it stands for.
   /// </summary>
   public class SharedTemplatesTests
   {
      private static readonly XNamespace Wpf = "http://schemas.microsoft.com/winfx/2006/xaml/presentation";
      private static readonly XNamespace X = "http://schemas.microsoft.com/winfx/2006/xaml";

      // ---- the tab strip ---------------------------------------------------------------

      /// <summary>
      /// FontWeight and FontSize inherit, and a tab's page is the TabItem's logical
      /// child: a setter for either on the item, in the style or in a trigger, reaches
      /// everything on the open tab.
      /// </summary>
      [Fact]
      public void TheTabStrip_SetsNoWeightAndNoSizeOnTheTabItemItself()
      {
         XElement style = TabItemStyle();
         foreach (XElement setter in style.Descendants(Wpf + "Setter"))
         {
            string property = (string)setter.Attribute("Property");
            if (property == "FontWeight" || property == "FontSize")
               Assert.Fail("The TabItem style sets " + property + " on the TabItem itself (no TargetName), which every caption, hint and editor value on the open tab inherits.");
         }
      }

      [Fact]
      public void TheTabStrip_WeightsTheSelectedTabsHeaderAlone()
      {
         XElement template = TabItemTemplate();
         XElement selected = template.Descendants(Wpf + "Trigger")
            .SingleOrDefault(t => (string)t.Attribute("Property") == "IsSelected" && (string)t.Attribute("Value") == "True");
         Assert.True(selected != null, "The TabItem template has no IsSelected trigger, so the selected tab is not marked.");

         XElement weight = selected.Elements(Wpf + "Setter")
            .SingleOrDefault(s => (string)s.Attribute("Property") == "TextElement.FontWeight");
         Assert.True(weight != null, "The IsSelected trigger no longer sets TextElement.FontWeight, so the selected tab's header is not SemiBold.");
         Assert.Equal("Content", (string)weight.Attribute("TargetName"));
         Assert.Equal("SemiBold", (string)weight.Attribute("Value"));
      }

      [Fact]
      public void TheTabStrip_SizesTheHeaderAlone()
      {
         XElement presenter = HeaderPresenter(TabItemTemplate());
         Assert.Equal("{DynamicResource AppFontSizeBody}", (string)presenter.Attribute("TextElement.FontSize"));
      }

      // ---- check box and radio button captions ---------------------------------------

      /// <summary>
      /// An implicit data template for strings in the style's resources, not a
      /// ContentTemplate: a ContentTemplate would also take a box whose Content is an
      /// element of its own (FieldDialog's AccessText) and show its ToString().
      /// </summary>
      [Theory]
      [InlineData("CheckBox")]
      [InlineData("RadioButton")]
      public void AStringCaption_WrapsInAnAccessText(string kind)
      {
         XElement style = ImplicitStyle(kind);
         Assert.True(style.Element(Wpf + "Setter") == null || style.Elements(Wpf + "Setter").All(s => (string)s.Attribute("Property") != "ContentTemplate"),
            "The " + kind + " style sets a ContentTemplate, which would present a box whose Content is an element as that element's ToString().");

         XElement resources = style.Element(Wpf + "Style.Resources");
         Assert.True(resources != null, "The " + kind + " style has no resources, so a string caption is presented by WPF's default and never wraps.");

         XElement template = resources.Elements(Wpf + "DataTemplate")
            .SingleOrDefault(t => (string)t.Attribute("DataType") == "{x:Type sys:String}");
         Assert.True(template != null, "The " + kind + " style has no data template for strings, so a string caption never wraps.");

         XElement root = template.Elements().Single();
         Assert.True(root.Name == Wpf + "AccessText", "The " + kind + " caption template's root is " + root.Name.LocalName + ", which prints the caption's Alt key underscore; an AccessText keeps the key.");
         Assert.Equal("Wrap", (string)root.Attribute("TextWrapping"));
         Assert.Equal("{Binding}", (string)root.Attribute("Text"));
      }

      /// <summary>
      /// The style is based on WPF-UI's, named by its own key, so that the template, the
      /// colours and the Normal weight stay WPF-UI's. Not on {StaticResource {x:Type CheckBox}}:
      /// inside the dictionary that defines the implicit style, a StaticResource for the type
      /// key finds the entry being defined and yields null, and a style based on nothing gave
      /// every box the classic theme's 13-pixel square with a black tick.
      /// </summary>
      [Theory]
      [InlineData("CheckBox")]
      [InlineData("RadioButton")]
      public void TheCaptionStyle_IsBasedOnWpfUisStyleByItsKey(string kind)
      {
         XElement style = ImplicitStyle(kind);
         string basedOn = (string)style.Attribute("BasedOn");
         Assert.True("{StaticResource Default" + kind + "Style}" == basedOn,
            "The " + kind + " style is based on " + (basedOn ?? "nothing") + " rather than WPF-UI's Default" + kind + "Style by its key; {StaticResource {x:Type " + kind + "}} inside the dictionary that defines the implicit style finds the entry being defined and yields null.");
      }

      /// <summary>
      /// The same trap for every implicit style App.xaml's own dictionary declares: one
      /// based on {StaticResource {x:Type T}}, T its own TargetType, is based on nothing.
      /// The DataGrid style has been so since the shell was written and is left as it is
      /// here - WPF-UI's DataGrid style would change every grid - so it is the one
      /// exception, until that is done.
      /// </summary>
      [Fact]
      public void NoImplicitStyle_IsBasedOnItsOwnTypeKey()
      {
         foreach (XElement style in App().Descendants(Wpf + "ResourceDictionary").Elements(Wpf + "Style").Where(s => s.Attribute(X + "Key") == null))
         {
            string target = TypeName((string)style.Attribute("TargetType"));
            if (target == "DataGrid")
               continue;
            Assert.True((string)style.Attribute("BasedOn") != "{StaticResource {x:Type " + target + "}}",
               "The implicit " + target + " style is based on {StaticResource {x:Type " + target + "}}, which inside the dictionary that defines it finds the entry being defined and yields null: name WPF-UI's style by its key.");
         }
      }

      [Fact]
      public void TheApplication_DeclaresTheSystemNamespaceTheCaptionTemplateKeysOn()
      {
         XDocument app = App();
         Assert.Equal("clr-namespace:System;assembly=System.Runtime", app.Root.GetNamespaceOfPrefix("sys")?.NamespaceName);
      }

      // ---- helpers ---------------------------------------------------------------------

      private static XDocument App() => XDocument.Parse(ReadControlPanelFile("App.xaml"));

      private static XElement ImplicitStyle(string targetType)
      {
         XElement style = App().Descendants(Wpf + "Style")
            .SingleOrDefault(s => (string)s.Attribute("TargetType") == targetType && s.Attribute(X + "Key") == null);
         Assert.True(style != null, "App.xaml has no implicit " + targetType + " style.");
         return style;
      }

      private static string TypeName(string targetType)
         => targetType.StartsWith("{x:Type ", StringComparison.Ordinal) ? targetType.Substring(8).TrimEnd('}') : targetType;

      private static XElement TabItemStyle() => ImplicitStyle("TabItem");

      private static XElement TabItemTemplate()
      {
         XElement template = TabItemStyle().Descendants(Wpf + "ControlTemplate").SingleOrDefault();
         Assert.True(template != null, "The TabItem style has no ControlTemplate.");
         return template;
      }

      private static XElement HeaderPresenter(XElement template)
      {
         XElement presenter = template.Descendants(Wpf + "ContentPresenter")
            .SingleOrDefault(p => (string)p.Attribute(X + "Name") == "Content" && (string)p.Attribute("ContentSource") == "Header");
         Assert.True(presenter != null, "The TabItem template has no header presenter named Content, which the selected tab's weight and the strip's size are set on.");
         return presenter;
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
