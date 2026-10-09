// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using System.Xml.Linq;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The domains page's rows can be opened, deleted and managed from the
   /// keyboard. All four lists put their verbs in buttons inside the row
   /// template: a ListView keeps ListBox's TabNavigation of Once, so Tab enters
   /// the list on the selected row and the next Tab leaves it, the arrow keys
   /// only move the selection, and nothing answered Enter, Delete, a double-click
   /// or the right mouse button - a keyboard user could select a domain or an
   /// account and do nothing with it. The WPF-free test project cannot press a
   /// key, so these tests hold the page's source to the shape that fixes it: every
   /// list is wired for Enter, Delete, a double-click and a context menu, each
   /// verb has one method the row's button and the keyboard share, and the menu's
   /// captions carry an Alt key in every language with none shared inside a menu
   /// - the per-row captions are exempt from build/check-mnemonics.py, and a menu
   /// is where the exemption ends.
   /// </summary>
   public class DomainsRowVerbsTests
   {
      private static readonly XNamespace Wpf = "http://schemas.microsoft.com/winfx/2006/xaml/presentation";
      private static readonly XNamespace X = "http://schemas.microsoft.com/winfx/2006/xaml";

      // The menus, as WireRows_ builds them: the captions that can be on screen
      // together. "_Members…" is the collection editor's caption, shared.
      public static TheoryData<string, string[]> Menus => new()
      {
         { "DomainList", new[] { "_Properties…", "Delete _domain" } },
         { "AccountList", new[] { "_Edit…", "Delete _account" } },
         { "AliasList", new[] { "Delete _alias" } },
         { "DistList", new[] { "_Properties…", "_Members…", "Delete _list" } },
      };

      [Fact]
      public void EveryList_IsWired_ForEnterDeleteADoubleClickAndAMenu()
      {
         string rows = ReadControlPanelFile(Path.Join("Views", "DomainsView.Rows.cs"));
         string wire = WithheldRowTests.MemberText(rows, "private void WireRows_()");
         Assert.True(wire != null, "DomainsView.WireRows_ is gone.");

         foreach (XElement list in Page().Descendants(Wpf + "ListView"))
         {
            string name = (string)list.Attribute(X + "Name");
            Assert.True(wire.Contains("WireList_(" + name + ",", StringComparison.Ordinal), name + " is not wired for the keyboard and the right mouse button.");
         }

         string one = WithheldRowTests.MemberText(rows, "private static void WireList_(ListView list, string openCaption, Action<object> open, Action<object> members, string deleteCaption, Action<object> delete)");
         Assert.True(one != null, "DomainsView.WireList_ is gone or its shape changed; update this test with it.");
         Assert.Contains("e.Key == Key.Enter", one, StringComparison.Ordinal);
         Assert.Contains("e.Key == Key.Delete", one, StringComparison.Ordinal);
         Assert.Contains("list.MouseDoubleClick +=", one, StringComparison.Ordinal);
         Assert.Contains("list.ContextMenu = menu;", one, StringComparison.Ordinal);

         // A double-click on a row's own button is the button's; one on a blank
         // part of the list is nobody's.
         Assert.Contains("RowAt_(list, e.OriginalSource as DependencyObject) == null", one, StringComparison.Ordinal);

         // From the keyboard the menu hangs under the selected row, named on the
         // list's attached placement - what WPF coerces the menu's own from once
         // it has set the owner, so a target set on the menu would not hold -
         // and from the mouse it opens at the pointer; WPF's own placement for a
         // keyboard menu is the centre of the owner, the whole list.
         Assert.Contains("ContextMenuService.SetPlacementTarget(list, list.ItemContainerGenerator.ContainerFromItem(list.SelectedItem) as UIElement ?? list)", one, StringComparison.Ordinal);
         Assert.Contains("ContextMenuService.SetPlacement(list, PlacementMode.Bottom)", one, StringComparison.Ordinal);
         Assert.Contains("list.ClearValue(ContextMenuService.PlacementTargetProperty)", one, StringComparison.Ordinal);
         Assert.Contains("list.ClearValue(ContextMenuService.PlacementProperty)", one, StringComparison.Ordinal);

         // The page builds the wiring as it is made.
         string page = ReadControlPanelFile(Path.Join("Views", "DomainsView.xaml.cs"));
         string ctor = WithheldRowTests.MemberText(page, "public DomainsView()");
         Assert.True(ctor != null && ctor.Contains("WireRows_();", StringComparison.Ordinal), "DomainsView's constructor no longer wires the rows.");
      }

      /// <summary>
      /// Each list's verbs read the selected row as the type the page fills the
      /// list with. The distribution lists' rows became DistRow objects when their
      /// Properties and Members buttons were named for the row, and a verb that
      /// still read the row as a string threw on the first Enter, Delete or menu
      /// on a list - the one road the row's own buttons, which carry the address
      /// in their Tag, never take.
      /// </summary>
      [Theory]
      [InlineData("DomainList", "DomainRow")]
      [InlineData("AccountList", "AccountRow")]
      [InlineData("AliasList", "AliasRow")]
      [InlineData("DistList", "DistRow")]
      public void EveryListsVerbs_ReadTheRow_AsTheTypeThePageFillsItWith(string list, string rowType)
      {
         string page = ReadControlPanelFile(Path.Join("Views", "DomainsView.xaml.cs"));
         Assert.True(page.Contains("new List<" + rowType + ">()", StringComparison.Ordinal) && page.Contains(list + ".ItemsSource = rows;", StringComparison.Ordinal),
            list + " is no longer filled with " + rowType + " rows; update this test with the page.");

         string rows = ReadControlPanelFile(Path.Join("Views", "DomainsView.Rows.cs"));
         string wire = WithheldRowTests.MemberText(rows, "private void WireRows_()");
         Assert.True(wire != null, "DomainsView.WireRows_ is gone.");
         Match call = Regex.Match(wire, @"WireList_\(" + list + @",(.*?)\);", RegexOptions.Singleline);
         Assert.True(call.Success, list + " is not wired.");
         Assert.DoesNotContain("(string)row", call.Groups[1].Value, StringComparison.Ordinal);
         Assert.Contains("((" + rowType + ")row)", call.Groups[1].Value, StringComparison.Ordinal);
      }

      /// <summary>
      /// The row's button and the keyboard lead to one method per verb, so that a
      /// confirmation or a reload added to one road is on the other.
      /// </summary>
      [Theory]
      [InlineData("EditDomain_Click", "EditDomain_")]
      [InlineData("DeleteDomain_Click", "DeleteDomain_")]
      [InlineData("EditAccount_Click", "EditAccount_")]
      [InlineData("DeleteAccount_Click", "DeleteAccount_")]
      [InlineData("DeleteAlias_Click", "DeleteAlias_")]
      [InlineData("EditDistList_Click", "EditDistList_")]
      [InlineData("EditRecipients_Click", "EditRecipients_")]
      [InlineData("DeleteDistList_Click", "DeleteDistList_")]
      public void TheRowsButton_AndTheKeyboard_ShareOneMethodPerVerb(string handler, string verb)
      {
         string page = ReadControlPanelFile(Path.Join("Views", "DomainsView.xaml.cs"));
         string click = WithheldRowTests.MemberText(page, "private void " + handler + "(object sender, RoutedEventArgs e)");
         Assert.True(click != null, "DomainsView." + handler + " is gone.");
         Assert.True(Regex.IsMatch(click, @"\b" + Regex.Escape(verb) + @"\("), handler + " does not call " + verb + ", so the button and the keyboard would diverge.");
         Assert.True(WithheldRowTests.MemberText(page, "private void " + verb + "(string ") != null, "DomainsView." + verb + "(string) is gone.");

         string rows = ReadControlPanelFile(Path.Join("Views", "DomainsView.Rows.cs"));
         Assert.True(Regex.IsMatch(rows, @"\b" + Regex.Escape(verb) + @"\("), "The keyboard and the menu never reach " + verb + ".");
      }

      /// <summary>
      /// The menus' captions carry an Alt key in every language and no two in one
      /// menu share one: the checker exempts a caption inside a row template, since
      /// a caption repeated per row cannot have a unique key, and these are the
      /// captions that take their place for the keyboard.
      /// </summary>
      [Theory]
      [MemberData(nameof(Menus))]
      public void TheMenusCaptions_CarryAnAltKey_InEveryLanguage_AndNoneCollide(string list, string[] captions)
      {
         foreach (string caption in captions)
            Assert.True(MnemonicText.Parse(caption).KeyIndex >= 0, caption + " has no Alt key.");
         AssertDistinct(list, "en", captions);

         string resources = Path.GetDirectoryName(ControlPanelFile(Path.Join("Resources", "Strings.resx")));
         int catalogues = 0;
         foreach (string path in Directory.EnumerateFiles(resources, "Strings.*.resx"))
         {
            string tag = Path.GetFileName(path).Substring("Strings.".Length);
            tag = tag.Substring(0, tag.Length - ".resx".Length);
            Dictionary<string, string> catalogue = XDocument.Load(path).Root.Elements("data")
               .ToDictionary(d => (string)d.Attribute("name"), d => (string)d.Element("value") ?? "", StringComparer.Ordinal);

            var translated = new List<string>();
            foreach (string caption in captions)
            {
               Assert.True(catalogue.TryGetValue(caption, out string value) && value.Length > 0, tag + " does not translate " + caption + ".");
               Assert.True(MnemonicText.Parse(value).KeyIndex >= 0, tag + ": \"" + value + "\" (" + caption + ") has no Alt key.");
               translated.Add(value);
            }

            AssertDistinct(list, tag, translated);
            catalogues++;
         }

         Assert.Equal(17, catalogues);
      }

      private static void AssertDistinct(string list, string tag, IEnumerable<string> captions)
      {
         var seen = new Dictionary<string, string>(StringComparer.Ordinal);
         foreach (string caption in captions)
         {
            string key = MnemonicText.Key(caption);
            Assert.True(!seen.ContainsKey(key), tag + ": Alt+" + key + " in \"" + caption + "\" collides with \"" + seen.GetValueOrDefault(key) + "\" on " + list + "'s menu.");
            seen[key] = caption;
         }
      }

      // ---- helpers -------------------------------------------------------------------------

      private static XElement Page() => XDocument.Parse(ReadControlPanelFile(Path.Join("Views", "DomainsView.xaml"))).Root;

      private static string ReadControlPanelFile(string relative) => File.ReadAllText(ControlPanelFile(relative));

      private static string ControlPanelFile(string relative)
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", relative);
            if (File.Exists(candidate))
               return candidate;
            directory = directory.Parent;
         }

         Assert.Fail("ControlPanel/" + relative.Replace('\\', '/') + " could not be found from the test binaries.");
         return null;
      }
   }
}
