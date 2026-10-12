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
   /// The Domains &amp; accounts page's four forms say a value they refuse on the
   /// box at fault (review finding F064). Add domain answered a bad name with a
   /// message box, Create account answered either box left empty with one message
   /// box that named neither, and Add alias and Add list did nothing at all with
   /// an empty box; every FieldRow has a message slot, and the page's rows had no
   /// name, so the code could not reach them. Each refusal is on its row now, the
   /// keyboard in the first box refused, every message cleared on the next press;
   /// a message box is left for the server's own refusal. And the button beside a
   /// box stays beside the box when a message appears under it.
   /// </summary>
   public class DomainsFormRefusalTests
   {
      [Fact]
      public void AddDomain_RefusesOnTheDomainBox()
      {
         string add = Member("private void AddDomain_Click(object sender, RoutedEventArgs e)");
         Assert.Contains("DialogFields.ClearErrors(NewDomainRow);", add, StringComparison.Ordinal);
         Assert.Contains("DialogFields.ShowError(NewDomainRow, L(\"Enter a domain name such as example.com.\"));", add, StringComparison.Ordinal);
         Assert.DoesNotContain("MessageBox.Show(L(\"Enter a valid domain name.\")", add, StringComparison.Ordinal);

         // The server's refusal stays a message box.
         Assert.Contains("MessageBox.Show(F(\"Could not create the domain: {0}\"", add, StringComparison.Ordinal);
      }

      [Fact]
      public void CreateAccount_RefusesEachEmptyBox_OnThatBox()
      {
         string add = Member("private void AddAccount_Click(object sender, RoutedEventArgs e)");
         Assert.Contains("DialogFields.ClearErrors(NewAccountRow, PasswordRow);", add, StringComparison.Ordinal);
         Assert.Contains("NewAccountRow.Error = L(\"Enter the account's address.\");", add, StringComparison.Ordinal);
         Assert.Contains("PasswordRow.Error = L(\"Enter a password, or press Generate.\");", add, StringComparison.Ordinal);
         Assert.Contains("if (DialogFields.ShowFirstError(NewAccountRow, PasswordRow))", add, StringComparison.Ordinal);
         Assert.DoesNotContain("Address and password are required.", add, StringComparison.Ordinal);
      }

      [Fact]
      public void AddAliasAndAddList_SayAnEmptyBox_WhereThePressDidNothing()
      {
         string alias = Member("private void AddAlias_Click(object sender, RoutedEventArgs e)");
         Assert.Contains("NewAliasNameRow.Error = L(\"Enter the alias address.\");", alias, StringComparison.Ordinal);
         Assert.Contains("NewAliasValueRow.Error = L(\"Enter where the alias delivers.\");", alias, StringComparison.Ordinal);
         Assert.Contains("if (DialogFields.ShowFirstError(NewAliasNameRow, NewAliasValueRow))", alias, StringComparison.Ordinal);
         Assert.DoesNotContain("name.Length == 0 || value.Length == 0)\n            return;", alias.Replace("\r\n", "\n"), StringComparison.Ordinal);

         string list = Member("private void AddDistList_Click(object sender, RoutedEventArgs e)");
         Assert.Contains("DialogFields.ShowError(NewDistAddressRow, L(\"Enter the list's address.\"));", list, StringComparison.Ordinal);
      }

      [Fact]
      public void TheMessages_GoWithThePageMetAnew_AndTheDomainsFormsWithTheDomain()
      {
         string enter = Member("public void OnEnter()");
         Assert.Contains("DialogFields.ClearErrors(NewDomainRow);", enter, StringComparison.Ordinal);
         Assert.Contains("ClearDomainFormErrors_();", enter, StringComparison.Ordinal);

         string selection = Member("private void DomainList_SelectionChanged(object sender, SelectionChangedEventArgs e)");
         Assert.Contains("ClearDomainFormErrors_();", selection, StringComparison.Ordinal);

         string clear = Member("private void ClearDomainFormErrors_()");
         Assert.Contains("DialogFields.ClearErrors(NewAccountRow, PasswordRow, NewAliasNameRow, NewAliasValueRow, NewDistAddressRow)", clear, StringComparison.Ordinal);
      }

      [Theory]
      [InlineData("AddDomainForm", "NewDomainRow", "AddDomainButton")]
      public void EachFormsButton_StaysBesideItsBox_WhenAMessageAppearsUnderIt(string form, string row, string button)
      {
         FormLineAssert.ButtonLinesUpWithItsRow(Page(), form, row, button, "DomainsView.xaml");
      }

      /// <summary>
      /// The alias and list forms, in half a card: each box as wide as the card and the
      /// button on a row of its own under the boxes at the card's right edge, where beside
      /// the button the box was about 120 pixels at 1280 and its example read
      /// "person@examp" and "team@example." (the final check of Tier 2, g3 defect 5). A
      /// message under a box moves the button down with it.
      /// </summary>
      [Theory]
      [InlineData("AddAliasForm", "AddAliasButton", 2, "NewAliasNameRow", "NewAliasValueRow")]
      [InlineData("AddListForm", "AddListButton", 1, "NewDistAddressRow")]
      public void TheHalfCardForms_GiveEachBoxTheCardsWidth_TheButtonUnderThem(string form, string button, int buttonRow, params string[] rows)
      {
         XElement page = Page();
         XElement grid = FormLineAssert.Named(page, form, "DomainsView.xaml");
         Assert.Null(grid.Element(XName.Get("Grid.ColumnDefinitions", "http://schemas.microsoft.com/winfx/2006/xaml/presentation")));
         for (int i = 0; i < rows.Length; i++)
         {
            XElement row = FormLineAssert.Named(page, rows[i], "DomainsView.xaml");
            Assert.Equal(i == 0 ? null : i.ToString(System.Globalization.CultureInfo.InvariantCulture), (string)row.Attribute("Grid.Row"));
            Assert.Null(row.Attribute("Grid.Column"));
            Assert.Null(row.Attribute("LabelGroup"));
         }

         XElement add = FormLineAssert.Named(page, button, "DomainsView.xaml");
         Assert.Equal(buttonRow.ToString(System.Globalization.CultureInfo.InvariantCulture), (string)add.Attribute("Grid.Row"));
         Assert.Equal("Right", (string)add.Attribute("HorizontalAlignment"));
         Assert.Same(grid, add.Parent);
      }

      [Fact]
      public void TheAccountButtons_LineUpWithThePassword_BesideIt()
      {
         FormLineAssert.ButtonLinesUpWithItsRow(Page(), "AccountForm", "PasswordRow", "AccountButtons", "DomainsView.xaml");
         XElement caption = FormLineAssert.Named(Page(), "AccountButtonsCaption", "DomainsView.xaml");
         Assert.Equal("PasswordLine", (string)caption.Attribute("SharedSizeGroup"));
      }

      // ---- helpers -------------------------------------------------------------------------

      private static XElement Page() => XDocument.Parse(FormLineAssert.ReadControlPanelFile(Path.Join("Views", "DomainsView.xaml"))).Root;

      private static string Member(string signature)
      {
         string code = FormLineAssert.ReadControlPanelFile(Path.Join("Views", "DomainsView.xaml.cs"));
         string member = WithheldRowTests.MemberText(code, signature);
         Assert.True(member != null, "DomainsView." + signature + " is gone.");
         return member;
      }
   }

   /// <summary>
   /// The shape a line of fields and the button beside them keeps (FieldRow.LabelGroup):
   /// the form a shared-size scope, the row naming a group, and the button under a
   /// grid row of that group, at the top of the line's box row - so that the button
   /// lines up with the box whatever the caption wraps to, and a message under the
   /// box moves neither. The button sat on the line's bottom edge, beside the message.
   /// </summary>
   internal static class FormLineAssert
   {
      private static readonly XNamespace X = "http://schemas.microsoft.com/winfx/2006/xaml";

      public static void ButtonLinesUpWithItsRow(XElement page, string formName, string rowName, string buttonName, string file)
      {
         XElement form = Named(page, formName, file);
         Assert.Equal("True", (string)form.Attribute("Grid.IsSharedSizeScope"));

         XElement row = Named(form, rowName, file);
         string group = (string)row.Attribute("LabelGroup");
         Assert.False(string.IsNullOrEmpty(group), rowName + " names no LabelGroup.");
         Assert.Null(row.Attribute("VerticalAlignment"));

         XElement button = Named(form, buttonName, file);
         XElement line = button.Parent;
         Assert.True(line != null && line.Name.LocalName == "Grid", buttonName + " is not in a grid of its own.");
         Assert.Contains(line.Descendants().Where(e => e.Name.LocalName == "RowDefinition"), d => (string)d.Attribute("SharedSizeGroup") == group);
         Assert.Equal("1", (string)button.Attribute("Grid.Row"));
         Assert.Equal("Top", (string)button.Attribute("VerticalAlignment"));
      }

      public static XElement Named(XElement root, string name, string file)
      {
         XElement found = root.DescendantsAndSelf().FirstOrDefault(e => (string)e.Attribute(X + "Name") == name);
         Assert.True(found != null, file + " has no element named " + name + ".");
         return found;
      }

      public static string ReadControlPanelFile(string relative)
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
