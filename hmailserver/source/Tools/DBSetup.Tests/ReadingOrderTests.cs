// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Drawing;
using System.Windows.Forms;
using DBSetup.Pages;
using hMailServer.Shared;
using Xunit;

namespace DBSetup.Tests
{
   /// <summary>
   ///    The tab order the tools' wizard (ucWizard, in Shared, which DBSetup, the Data
   ///    Directory Synchronizer and the Import Tool share) gives each page before it shows it:
   ///    <see cref="ReadingOrder.ApplyTo" />, line by line from the top and left to right
   ///    along a line. The geometry of the first cases is that of the Import Tool's pages,
   ///    whose labels sit a few pixels lower than the field they name: up to 6.3.7 the order
   ///    went by Top alone, so that on the text import page Tab reached the Browse button
   ///    before the file name, and a screen reader named the domain list "Text file:", the
   ///    label before it in the order. These tests run in DBSetup.Tests because it is the
   ///    test project that reaches Shared, through DBSetup.
   /// </summary>
   public sealed class ReadingOrderTests
   {
      private static T Add<T>(Control parent, string name, int left, int top, int width, int height, string text = "")
         where T : Control, new()
      {
         var control = new T { Name = name, Location = new Point(left, top), Size = new Size(width, height), Text = text };
         parent.Controls.Add(control);
         return control;
      }

      private static string[] Order(Control page)
      {
         var names = new System.Collections.Generic.List<string>();
         for (Control control = page.GetNextControl(null, true); control != null; control = page.GetNextControl(control, true))
            names.Add(control.Name);
         return names.ToArray();
      }

      [Fact]
      public void ALabelSetLowerThanItsFieldComesBeforeItAndNamesIt()
      {
         using var page = new UserControl { Size = new Size(472, 195) };
         // In the Controls order the designer left: the order to be made is not this one.
         Add<Label>(page, "labelFormatHelp", 8, 80, 440, 100, "The file should contain one account per line");
         ComboBox domains = Add<ComboBox>(page, "comboDomains", 96, 41, 200, 21);
         Add<Label>(page, "labelDomain", 8, 44, 46, 13, "Domain:");
         Add<Button>(page, "buttonBrowse", 406, 7, 32, 23, "...");
         TextBox file = Add<TextBox>(page, "textFile", 96, 9, 304, 20);
         Add<Label>(page, "labelFile", 8, 12, 50, 13, "Text file:");

         ReadingOrder.ApplyTo(page);

         Assert.Equal(new[] { "labelFile", "textFile", "buttonBrowse", "labelDomain", "comboDomains", "labelFormatHelp" }, Order(page));
         Assert.Equal("Text file:", file.AccessibilityObject.Name);
         Assert.Equal("Domain:", domains.AccessibilityObject.Name);
      }

      [Fact]
      public void LabelsAboveTheirFieldsAreALineOfTheirOwn()
      {
         using var page = new UserControl { Size = new Size(400, 120) };
         Add<TextBox>(page, "textServerPort", 224, 24, 64, 21);
         Add<Label>(page, "labelPort", 224, 8, 26, 13, "Port");
         Add<TextBox>(page, "textServerAddress", 8, 24, 192, 21);
         Add<Label>(page, "labelDatabaseServerAddress", 8, 8, 120, 13, "Database server address");

         ReadingOrder.ApplyTo(page);

         Assert.Equal(new[] { "labelDatabaseServerAddress", "labelPort", "textServerAddress", "textServerPort" }, Order(page));
      }

      [Fact]
      public void AContainersChildrenAreNumberedInsideIt()
      {
         using var page = new UserControl { Size = new Size(300, 200) };
         var group = Add<GroupBox>(page, "groupAuthentication", 8, 40, 280, 100, "Authentication");
         Add<RadioButton>(group, "radioWindows", 16, 48, 200, 17, "Windows");
         Add<RadioButton>(group, "radioServer", 16, 20, 200, 17, "Server");
         Add<Button>(page, "buttonTest", 8, 160, 75, 23, "Test");
         Add<TextBox>(page, "textName", 8, 8, 200, 21);

         ReadingOrder.ApplyTo(page);

         Assert.Equal(0, page.Controls["textName"].TabIndex);
         Assert.Equal(1, group.TabIndex);
         Assert.Equal(2, page.Controls["buttonTest"].TabIndex);
         Assert.Equal(0, group.Controls["radioServer"].TabIndex);
         Assert.Equal(1, group.Controls["radioWindows"].TabIndex);
      }

      [Fact]
      public void ControlsAtOnePositionKeepTheirParentsOrder()
      {
         using var page = new UserControl { Size = new Size(300, 100) };
         Add<Label>(page, "first", 8, 8, 100, 13);
         Add<Label>(page, "second", 8, 8, 100, 13);
         Add<Label>(page, "third", 8, 8, 100, 13);

         ReadingOrder.ApplyTo(page);

         Assert.Equal(new[] { 0, 1, 2 }, new[] { page.Controls["first"].TabIndex, page.Controls["second"].TabIndex, page.Controls["third"].TabIndex });
      }

      [Fact]
      public void TheConnectionPageIsTabbedThroughInTheOrderItReads()
      {
         // The page's designer leaves textUsername at TabIndex 2, so that without the
         // wizard's numbering Tab went from the server address to the user name.
         using var page = new ucDBConnectionInfo();

         ReadingOrder.ApplyTo(page);

         Assert.Equal(new[]
         {
            "labelDatabaseServerAddress", "labelPort", "textServerAddress", "textServerPort",
            "labelDatabaseName", "textDatabaseName", "labelAuthentication", "radioUseServerAuthentication",
            "labelUsername", "textUsername", "labelPassword", "textPassword", "radioUseWindowsAuthentication",
         }, Order(page));
         Assert.Equal("Username", page.Controls["textUsername"].AccessibilityObject.Name);
         Assert.Equal("Password", page.Controls["textPassword"].AccessibilityObject.Name);
      }
   }
}
