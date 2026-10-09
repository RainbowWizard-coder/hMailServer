// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Text.RegularExpressions;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The distribution list members dialog's add row was a star-width box and
   /// three buttons, which in German left the box about 30 pixels ("Hinzufügen",
   /// "Aus AD hinzufügen…", "Ausgewählte entfernen"), 100 in Japanese and 155 in
   /// English, too narrow to read an address in. The test fails against that
   /// dialog: the add row is the box and Add, and the two buttons that act on the
   /// list are under it, where they act.
   /// </summary>
   public class RecipientsDialogTests
   {
      [Fact]
      public void TheAddRow_IsTheBoxAndAdd_AndTheOtherTwoButtonsAreUnderTheList()
      {
         string ctor = WithheldRowTests.MemberText(Source(), "public RecipientsDialog(Window owner, string domainName, string listAddress)");
         Assert.True(ctor != null, "The RecipientsDialog constructor is gone.");

         // The add row: one star column for the box and one for Add, no more.
         int row = ctor.IndexOf("var bottom = new Grid", StringComparison.Ordinal);
         Assert.True(row >= 0, "The add row is gone.");
         string addRow = ctor.Substring(row, ctor.IndexOf("body.Children.Add(bottom);", StringComparison.Ordinal) - row);
         Assert.Equal(2, Regex.Matches(addRow, @"bottom\.ColumnDefinitions\.Add").Count);
         Assert.Contains("L(\"_Add\")", addRow, StringComparison.Ordinal);
         Assert.DoesNotContain("L(\"Add from A_D…\")", addRow, StringComparison.Ordinal);
         Assert.DoesNotContain("L(\"_Remove\")", addRow, StringComparison.Ordinal);

         // Add from AD… and Remove (Remove selected until review finding F076: a
         // list's Remove takes out the chosen entry) in a WrapPanel under the
         // list, before the delivery row and the add row.
         // The list sits on a card (as the public folders list does), with its
         // empty state laid over it, so the card's line is the list's place in
         // the body.
         int list = ctor.IndexOf("Content = listHost", StringComparison.Ordinal);
         int actions = ctor.IndexOf("body.Children.Add(listActions);", StringComparison.Ordinal);
         int delivery = ctor.IndexOf("body.Children.Add(deliveryRow);", StringComparison.Ordinal);
         Assert.True(list >= 0 && actions > list && delivery > actions, "The list's two buttons are not under the list.");
         string between = ctor.Substring(list, actions - list);
         Assert.Contains("new WrapPanel { HorizontalAlignment = HorizontalAlignment.Right", between, StringComparison.Ordinal);
         Assert.Contains("L(\"Add from A_D…\")", between, StringComparison.Ordinal);
         Assert.Contains("L(\"_Remove\")", between, StringComparison.Ordinal);
         Assert.Contains("ButtonStyles.DangerText(removeButton);", between, StringComparison.Ordinal);
         Assert.Contains("SelectionGate.Bind(listBox_, removeButton);", between, StringComparison.Ordinal);
      }

      private static string Source() => ReadControlPanelFile(Path.Join("Views", "RecipientsDialog.cs"));

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
