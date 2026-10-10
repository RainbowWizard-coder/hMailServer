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
   /// Server status, the first page issue #48 named: its version, state, start time,
   /// uptime and update line, the database's type, host, name and schema, the six
   /// statistics and the warnings can be selected and copied, with their captions.
   ///
   /// SelectableText makes every TextBlock selectable except one inside a control that
   /// keeps its own words (a button, a text box, a list's row and the rest: its
   /// IsInteractive_), one under a menu of somebody else's, and one switched off with
   /// SelectableText.IsEnabled. So what is held here is the page: every caption and
   /// value is a TextBlock outside all three, the values the code fills are those
   /// TextBlocks, and the cards and the warnings around them are not controls of that
   /// kind. The rendered behaviour - pressing a value and copying it - was measured in
   /// the real Control Panel (dacf32c41); this assembly has no WPF, so the page is read.
   /// </summary>
   public class StatusPageTests
   {
      private static readonly XNamespace Xaml = "http://schemas.microsoft.com/winfx/2006/xaml";

      // The XAML elements whose text SelectableText leaves to them, by the kinds its
      // IsInteractive_ names (held in TheKindsLeftAlone_AreStillLeftAlone).
      private static readonly HashSet<string> KeepTheirText = new(StringComparer.Ordinal)
      {
         "Button", "RepeatButton", "ToggleButton", "CheckBox", "RadioButton", "HyperlinkButton", "ToggleSwitch",
         "TextBox", "RichTextBox", "PasswordBox", "NumberBox", "AutoSuggestBox", "ComboBox",
         "ListBox", "ListView", "ListBoxItem", "ListViewItem", "TreeView", "TreeViewItem", "TabControl", "TabItem",
         "Menu", "MenuItem", "ContextMenu", "DataGrid", "ToolTip", "Slider", "ScrollBar", "Hyperlink", "TitleBar",
      };

      [Fact]
      public void EveryCaptionAndValue_IsSelectableText()
      {
         XDocument page = XDocument.Parse(ReadControlPanelFile(Path.Join("Views", "StatusView.xaml")), LoadOptions.SetLineInfo);
         List<XElement> texts = page.Descendants().Where(e => e.Name.LocalName == "TextBlock").ToList();

         // Version, State, Started, Uptime and Update with their values; Type, Host,
         // Name and Schema with theirs; six statistics with theirs.
         Assert.True(texts.Count >= 30, "The page has " + texts.Count + " TextBlocks where its three cards have thirty.");

         foreach (XElement text in texts)
         {
            string which = Describe_(text);
            foreach (XElement around in text.AncestorsAndSelf())
            {
               string kind = around.Name.LocalName.Split('.')[0];   // Button.Icon is inside a Button
               Assert.False(KeepTheirText.Contains(kind) && around != text,
                  which + " is inside a " + kind + ", which keeps its words: it cannot be selected or copied.");
               Assert.False(around.Attributes().Any(a => a.Name.LocalName.EndsWith("ContextMenu", StringComparison.Ordinal))
                            || around.Elements().Any(e => e.Name.LocalName.EndsWith(".ContextMenu", StringComparison.Ordinal)),
                  which + " is under a menu of its own, which SelectableText leaves alone.");
               Assert.False(around.Attributes().Any(a => a.Name.LocalName.Contains("SelectableText.IsEnabled", StringComparison.Ordinal)),
                  which + " has selecting switched off.");
               Assert.False(around.Attributes().Any(a => a.Name.LocalName == "IsHitTestVisible" && a.Value == "False"),
                  which + " cannot take the pointer, so it cannot be pressed to select.");
            }
         }

         // Nor anywhere else on the page: the warnings are added from the code into
         // WarningsPanel, which the XAML gives no text of its own.
         foreach (XElement element in page.Descendants())
         {
            Assert.False(element.Attributes().Any(a => a.Name.LocalName.Contains("SelectableText.IsEnabled", StringComparison.Ordinal))
                         || element.Elements().Any(e => e.Name.LocalName.EndsWith("SelectableText.IsEnabled", StringComparison.Ordinal)),
               "The page's " + element.Name.LocalName + " at line " + ((System.Xml.IXmlLineInfo)element).LineNumber + " switches selecting off.");
         }
      }

      [Fact]
      public void TheValuesTheCodeFills_AreThePagesTextBlocks()
      {
         XDocument page = XDocument.Parse(ReadControlPanelFile(Path.Join("Views", "StatusView.xaml")));
         var named = page.Descendants()
            .Where(e => e.Attribute(Xaml + "Name") != null)
            .ToDictionary(e => e.Attribute(Xaml + "Name").Value, e => e.Name.LocalName);

         string code = ReadControlPanelFile(Path.Join("Views", "StatusView.xaml.cs"));
         string setValue = WithheldRowTests.MemberText(code, "private static void SetValue_(TextBlock value, string label, string text)");
         Assert.True(setValue != null, "StatusView.SetValue_ no longer writes a value into a TextBlock.");
         Assert.Contains("value.Text = text;", setValue);

         var filled = Regex.Matches(code, @"\bSetValue_\(\s*(\w+)\s*,").Select(m => m.Groups[1].Value).Distinct().ToList();
         // The state is a pill since review finding F246 (ServerStatusFlowTests holds it),
         // named "State, word" for a screen reader as these are.
         var expected = new[] { "VersionValue", "DbTypeValue", "DbHostValue", "DbNameValue", "DbVersionValue",
                                "ProcessedValue", "SpamValue", "VirusValue", "SmtpValue", "ImapValue", "Pop3Value" };
         foreach (string value in expected)
            Assert.True(filled.Contains(value), "The page no longer fills " + value + ".");

         foreach (string value in filled.Concat(new[] { "StartedValue", "UptimeValue" }))
         {
            Assert.True(named.TryGetValue(value, out string kind), value + " is not on the page.");
            Assert.True(kind == "TextBlock", value + " is a " + kind + ", not a TextBlock, so SelectableText does not make it selectable.");
         }
      }

      /// <summary>
      /// The cards and the warnings around the text are plain content: a Card or an
      /// InlineNotice that acted as a button would keep its words to itself. A warning
      /// shows its sentence in a TextBlock of the notice's template.
      /// </summary>
      [Fact]
      public void TheCardsAndTheWarnings_AreNotControlsThatKeepTheirText()
      {
         string scaffold = ReadControlPanelFile(Path.Join("Views", "Scaffold", "ScaffoldControl.cs"));
         Assert.Matches(@"public abstract class ScaffoldContentControl : ContentControl\b", scaffold);
         Assert.Matches(@"public class Card : ScaffoldContentControl\b", ReadControlPanelFile(Path.Join("Views", "Scaffold", "Card.cs")));
         Assert.Matches(@"public class InlineNotice : ScaffoldContentControl\b", ReadControlPanelFile(Path.Join("Views", "Scaffold", "InlineNotice.cs")));

         XDocument styles = XDocument.Parse(ReadControlPanelFile(Path.Join("Views", "Scaffold", "Scaffold.xaml")));
         XElement notice = styles.Descendants().Single(e => e.Name.LocalName == "ControlTemplate"
            && (string)e.Attribute("TargetType") == "{x:Type scaffold:InlineNotice}");
         Assert.Contains(notice.Descendants(), e => e.Name.LocalName == "TextBlock" && (string)e.Attribute("Text") == "{TemplateBinding Text}");
         Assert.DoesNotContain(notice.Descendants(), e => KeepTheirText.Contains(e.Name.LocalName));

         string code = ReadControlPanelFile(Path.Join("Views", "StatusView.xaml.cs"));
         Assert.Matches(@"WarningsPanel\.Children\.(Add|Insert)\((0,\s*)?new InlineNotice\b", code);
         Assert.DoesNotMatch(@"WarningsPanel\.Children\.(Add|Insert)\((0,\s*)?new (?!InlineNotice\b)", code);
         Assert.DoesNotContain("SelectableText.SetIsEnabled", code);
         Assert.DoesNotContain("SelectableText.IsEnabledProperty", code);
      }

      /// <summary>The kinds the list above stands for are still the ones SelectableText leaves to their control.</summary>
      [Fact]
      public void TheKindsLeftAlone_AreStillLeftAlone()
      {
         string source = ReadControlPanelFile(Path.Join("Services", "SelectableText.cs"));
         string interactive = WithheldRowTests.MemberText(source, "private static bool IsInteractive_(DependencyObject node)");
         Assert.True(interactive != null, "SelectableText.IsInteractive_ is gone.");
         var kinds = Regex.Matches(interactive, @"node is (?:[\w.]+\.)?(\w+)").Select(m => m.Groups[1].Value).ToHashSet();
         foreach (string kind in new[] { "ButtonBase", "TextBoxBase", "PasswordBox", "ComboBox", "ListBoxItem", "TreeViewItem",
                                         "TabItem", "MenuItem", "ToolTip", "ContextMenu", "Slider", "ScrollBar", "Hyperlink", "TitleBar" })
         {
            Assert.True(kinds.Contains(kind), "SelectableText no longer leaves a " + kind + "'s text to it; StatusPageTests' list says it does.");
         }
      }

      private static string Describe_(XElement text)
      {
         string name = (string)text.Attribute(Xaml + "Name");
         string words = (string)text.Attribute("Text");
         return "The TextBlock " + (name ?? words ?? "at line " + ((System.Xml.IXmlLineInfo)text).LineNumber);
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
