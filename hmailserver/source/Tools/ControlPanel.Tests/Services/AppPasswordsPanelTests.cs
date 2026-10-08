// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The account dialog's App passwords tab put an inline "Name", a 220-pixel
   /// box, Create, Revoke / restore, Delete and Refresh in one horizontal
   /// StackPanel, so in the dialog's 592 pixels Delete lost its right edge and
   /// Refresh was off the tab, and the panel started four pixels further in than
   /// the other tabs. The test fails against that panel: the name is a captioned
   /// field with Create beside it, the three buttons that act on the selected row
   /// are under the list, and the panel starts where the other tabs do.
   /// </summary>
   public class AppPasswordsPanelTests
   {
      [Fact]
      public void TheName_IsAFieldRow_WithCreateBesideIt_AndTheRowActionsUnderTheList()
      {
         string code = Source();
         string build = WithheldRowTests.MemberText(code, "private void Build()");
         Assert.True(build != null, "AppPasswordsPanel.Build is gone.");

         Assert.Contains("nameRow_ = DialogFields.Field(L(\"_Name\"), newName_);", build, StringComparison.Ordinal);
         Assert.DoesNotContain("Text = L(\"Name\")", build, StringComparison.Ordinal);
         Assert.DoesNotContain("Width = 220", code, StringComparison.Ordinal);

         int list = build.IndexOf("root.Children.Add(card);", StringComparison.Ordinal);
         int actions = build.IndexOf("root.Children.Add(rowActions);", StringComparison.Ordinal);
         Assert.True(list >= 0 && actions > list, "The row actions are not under the list.");
         string between = build.Substring(list, actions - list);
         Assert.Contains("new WrapPanel", between, StringComparison.Ordinal);
         foreach (string caption in new[] { "L(\"Re_voke / restore\")", "L(\"_Delete\")", "L(\"_Refresh\")" })
            Assert.Contains(caption, between, StringComparison.Ordinal);
         Assert.Contains("ButtonStyles.DangerText(delete);", between, StringComparison.Ordinal);
         Assert.Contains("SelectionGate.Bind(list_, revoke, delete);", between, StringComparison.Ordinal);
         Assert.DoesNotContain("ControlAppearance.Danger,", build, StringComparison.Ordinal);

         // No inset of the panel's own, and the component Card rather than the old Border style.
         Assert.Contains("new StackPanel { Margin = new Thickness(0, 8, 0, 0) }", build, StringComparison.Ordinal);
         Assert.DoesNotContain("SetResourceReference(StyleProperty, \"Card\")", code, StringComparison.Ordinal);
      }

      [Fact]
      public void AMissingName_IsSaidOnTheField()
      {
         string create = WithheldRowTests.MemberText(Source(), "private void Create()");
         Assert.True(create != null && create.Contains("DialogFields.ShowError(nameRow_,", StringComparison.Ordinal), "A missing name is not reported on the Name field.");
      }

      private static string Source() => ReadControlPanelFile(Path.Join("Views", "AppPasswordsPanel.cs"));

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
