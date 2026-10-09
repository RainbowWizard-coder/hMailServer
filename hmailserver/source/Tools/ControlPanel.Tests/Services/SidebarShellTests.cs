// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The sidebar's rows, read from MainWindow. Scanned rather than rendered, as
   /// UnreadRowTests is: this assembly has no WPF.
   /// </summary>
   public class SidebarShellTests
   {
      /// <summary>
      /// Every row of the sidebar is the shell's row, the pages inside a group as
      /// much as the groups and the top-level pages: a TreeView gives its
      /// ItemContainerStyle to its own items alone, so each group passes it on to
      /// its pages before they are added, and they no longer fall back to
      /// WPF-UI's row with the Windows accent's bar.
      /// </summary>
      [Fact]
      public void EveryGroupGivesItsPagesTheShellsRowStyle()
      {
         string source = ContrastTokensTests.ReadControlPanelFile("MainWindow.xaml.cs");
         if (source == null)
            return;

         string build = WithheldRowTests.MemberText(source, "private TreeViewItem BuildNavItem(NavNode node)");
         Assert.True(build != null, "MainWindow.BuildNavItem is gone.");

         int style = build.IndexOf("item.ItemContainerStyle = NavTree.ItemContainerStyle;", StringComparison.Ordinal);
         int children = build.IndexOf("item.Items.Add(BuildNavItem(child))", StringComparison.Ordinal);
         Assert.True(style >= 0, "A group does not give its pages the tree's row style, so they take WPF-UI's.");
         Assert.True(children > style, "A group gives its pages the tree's row style after it has added them.");

         // A page's row returns before the group's part, so only groups set it.
         Assert.True(build.IndexOf("return item;", StringComparison.Ordinal) < style, "A page's row sets an item style it has no items for.");
      }

      /// <summary>
      /// The row style itself is the one the shell's tests read (the brand pill,
      /// the subtle grey, the keyboard border): it is still the TreeView's own,
      /// declared in MainWindow.xaml.
      /// </summary>
      [Fact]
      public void TheRowStyleIsTheTreesOwn()
      {
         string xaml = ContrastTokensTests.ReadControlPanelFile("MainWindow.xaml");
         if (xaml == null)
            return;

         Assert.Contains("<TreeView.ItemContainerStyle>", xaml, StringComparison.Ordinal);
         Assert.Contains("<Trigger Property=\"IsKeyboardFocused\" Value=\"True\">", xaml, StringComparison.Ordinal);
      }
   }
}
