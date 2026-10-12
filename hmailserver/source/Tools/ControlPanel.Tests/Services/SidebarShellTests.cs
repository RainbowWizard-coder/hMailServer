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
      /// Before sign-in the sidebar, its toggle and the search are not shown -
      /// the owner's choice over dimming them - where the disabled tree and the
      /// search field were drawn exactly as they are when they work. Each
      /// follows whether it can be used: the window disables them before sign-in
      /// and after a refused second factor and enables them on sign-in, and their
      /// IsEnabledChanged shows or hides them, so nothing else has to remember to.
      /// </summary>
      [Fact]
      public void BeforeSignIn_TheSidebarAndTheSearchAreNotShown()
      {
         string source = ContrastTokensTests.ReadControlPanelFile("MainWindow.xaml.cs");
         string xaml = ContrastTokensTests.ReadControlPanelFile("MainWindow.xaml");
         if (source == null || xaml == null)
            return;

         // Wired before the constructor disables them, so the first change already hides them.
         int wiredTree = source.IndexOf("NavTree.IsEnabledChanged += (s, e) => ApplySidebar_();", StringComparison.Ordinal);
         int wiredSearch = source.IndexOf("SearchButton.IsEnabledChanged += (s, e) => ShowSearch_();", StringComparison.Ordinal);
         int disabled = source.IndexOf("NavTree.IsEnabled = false;", StringComparison.Ordinal);
         int connect = source.IndexOf("ContentHost.Content = new ConnectView(OnConnected);", StringComparison.Ordinal);
         Assert.True(wiredTree >= 0 && wiredSearch >= 0, "The sidebar or the search no longer follows whether it can be used.");
         Assert.True(wiredTree < disabled && wiredSearch < disabled && disabled < connect,
            "The window disables the navigation before it follows it, or after it shows the sign-in page.");

         string apply = WithheldRowTests.MemberText(source, "private void ApplySidebar_()");
         Assert.True(apply != null, "MainWindow.ApplySidebar_ is gone.");
         Assert.Contains("bool shown = NavTree.IsEnabled;", apply, StringComparison.Ordinal);
         Assert.Contains("SidebarPane.Visibility = shown ? Visibility.Visible : Visibility.Collapsed;", apply, StringComparison.Ordinal);
         Assert.Contains("SidebarToggle.Visibility = shown ? Visibility.Visible : Visibility.Collapsed;", apply, StringComparison.Ordinal);
         Assert.Contains("SidebarColumn.Width = new GridLength(0);", apply, StringComparison.Ordinal);
         Assert.Contains("shown == sidebarShown_", apply, StringComparison.Ordinal);

         string search = WithheldRowTests.MemberText(source, "private void ShowSearch_()");
         Assert.True(search != null, "MainWindow.ShowSearch_ is gone.");
         Assert.Contains("SearchButton.Visibility = SearchButton.IsEnabled ? Visibility.Visible : Visibility.Collapsed;", search, StringComparison.Ordinal);

         Assert.Contains("x:Name=\"SidebarPane\"", xaml, StringComparison.Ordinal);
         Assert.Contains("x:Name=\"ContentSurface\"", xaml, StringComparison.Ordinal);

         // Sign-in enables both, and a refused second factor disables the tree again -
         // through ShowSignIn_, the way Sign out goes back to the sign-in page (review
         // finding F012).
         string connected = WithheldRowTests.MemberText(source, "private void OnConnected()");
         Assert.True(connected != null, "MainWindow.OnConnected is gone.");
         Assert.Contains("NavTree.IsEnabled = true;", connected, StringComparison.Ordinal);
         Assert.Contains("SearchButton.IsEnabled = true;", connected, StringComparison.Ordinal);
         Assert.Contains("ShowSignIn_(", connected, StringComparison.Ordinal);
         string signIn = WithheldRowTests.MemberText(source, "private void ShowSignIn_(string host, string userName, bool focusServer)");
         Assert.True(signIn != null, "MainWindow.ShowSignIn_ is gone.");
         Assert.Contains("NavTree.IsEnabled = false;", signIn, StringComparison.Ordinal);
         Assert.Contains("SearchButton.IsEnabled = false;", signIn, StringComparison.Ordinal);
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

      /// <summary>
      /// The sidebar's rows keep clear of its scroll bar while it shows: WPF-UI draws
      /// the bar over the rows, and the keyboard's ring on a row ran under it, cut on
      /// the right (the final check of Tier 2, g4 defect 10).
      /// </summary>
      [Fact]
      public void TheRowsKeepClearOfTheSidebarsScrollBar()
      {
         string code = ContrastTokensTests.ReadControlPanelFile("MainWindow.xaml.cs");
         if (code == null)
            return;

         Assert.Contains("ScrollBarInset.ForList(NavTree);", code, StringComparison.Ordinal);

         // The tree is in a pane collapsed until sign-in, so it has no template when it
         // loads: the inset makes the template and tries again whenever the list is
         // shown, where it looked once at load, found no viewer, and the rows still ran
         // under the bar in the renders after the fix.
         string inset = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Services", "ScrollBarInset.cs"));
         string forList = WithheldRowTests.MemberText(inset, "public static void ForList(Control list)");
         Assert.True(forList != null, "ScrollBarInset.ForList is gone.");
         Assert.Contains("list.ApplyTemplate();", forList, StringComparison.Ordinal);
         Assert.Contains("list.IsVisibleChanged +=", forList, StringComparison.Ordinal);
      }
   }
}
