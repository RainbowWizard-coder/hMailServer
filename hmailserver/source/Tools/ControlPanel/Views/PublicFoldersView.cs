// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Linq;
using System.Windows;
using System.Windows.Controls;
using hMailServer.ControlPanel.Services;
using hMailServer.ControlPanel.Views.Scaffold;
using MessageBox = hMailServer.ControlPanel.Views.Dialogs;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Views
{
   /// <summary>
   /// Public IMAP folders (Settings.PublicFolders): shared mailboxes visible to
   /// multiple accounts. Folders can be added/removed and each folder's ACL
   /// permissions edited via <see cref="FolderPermissionsDialog"/>.
   ///
   /// The list is the folders' tree - each folder under its parent, indented a
   /// level at a time, with a folder's glyph - read through each folder's
   /// SubFolders, and the subtitle names the server's hierarchy delimiter in an
   /// example (review finding F206). A name with the delimiter in it makes the
   /// folder under the folders it names, as a mail client's CREATE does
   /// (Services/PublicFolderTree says why the server's own Add does not).
   /// </summary>
   public class PublicFoldersView : UserControl, IPageLifecycle
   {
      // A level of the tree is this much further in.
      private const double IndentPerLevel = 20;

      private readonly ListBox list_ = new()
      {
         FontSize = Typography.Body,
         BorderThickness = new Thickness(0),
         Background = System.Windows.Media.Brushes.Transparent
      };
      private readonly PageHeader header_ = new();
      private readonly InlineNotice notice_ = new() { Visibility = Visibility.Collapsed };

      // The folders whose own name holds the delimiter, which a mail client cannot open.
      private readonly InlineNotice misnamed_ = new() { Level = StatusLevel.Warning, Visibility = Visibility.Collapsed };
      private readonly EmptyState empty_ = new() { Icon = Wpf.Ui.Controls.SymbolRegular.FolderPeople24, Visibility = Visibility.Collapsed };
      // How many folders there are, beside the title, where a caption under the
      // list said "4 public folders." (review finding F288).
      private readonly StatusPill count_ = new() { Level = StatusLevel.Normal, Visibility = Visibility.Collapsed };

      // The search, live over the folders' names, as on every other list page
      // (review finding F210).
      private readonly Toolbar search_ = new() { SearchPlaceholder = L("Search public folders") };

      // What an empty list means, said by a read that finds no folder and by a
      // search cleared over one.
      private static string EmptySentence_ => L("No public folders yet. Add folder creates one that several accounts can open; Permissions then decides who may read and write it.");

      // Add folder waits for a read that works (Services/ReadFailure).
      private Wpf.Ui.Controls.Button addButton_;

      // The server's hierarchy delimiter, read with the folders.
      private string delimiter_ = PublicFolderTree.DefaultDelimiter;

      public PublicFoldersView() => Build();

      public void OnEnter() => Reload();
      public void OnLeave() { }

      private void Build()
      {
         var root = new Grid();
         root.SetResourceReference(MarginProperty, "AppPagePadding");
         root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
         root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
         root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
         root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });

         header_.PageKey = "publicfolders";
         header_.Subtitle = PublicFolderTree.Subtitle(delimiter_);
         System.Windows.Automation.AutomationProperties.SetName(list_, L("Public folders"));
         list_.ItemTemplate = FolderRowTemplate_();

         // The list page pattern (docs/ControlPanelDesign.md, List pages; review
         // findings F288, F289): the count and Refresh in the header; the selected
         // folder's Permissions and Delete, then Add folder, the Primary, at the
         // right edge of the toolbar over the list, where the Primary came first.
         var actions = new WrapPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Right };
         var permissions = MakeButton(L("_Permissions"), Wpf.Ui.Controls.ControlAppearance.Secondary, (_, _) => EditPermissions());
         actions.Children.Add(permissions);
         var delete = MakeButton(L("_Delete"), Wpf.Ui.Controls.ControlAppearance.Secondary, (_, _) => DeleteFolder());
         hMailServer.ControlPanel.Services.ButtonStyles.DangerText(delete);
         actions.Children.Add(delete);
         hMailServer.ControlPanel.Services.SelectionGate.Bind(list_, permissions, delete);
         addButton_ = MakeButton(L("_Add folder"), Wpf.Ui.Controls.ControlAppearance.Primary, (_, _) => AddFolder());
         actions.Children.Add(addButton_);
         foreach (FrameworkElement button in actions.Children)
            button.Margin = new Thickness(button.Margin.Left, 2, button.Margin.Right, 2);
         search_.Actions = actions;
         // The count is a polite live region, so that a screen reader hears it as
         // it changes, as the notice it replaced was heard (Scaffold/StatusPill).
         System.Windows.Automation.AutomationProperties.SetLiveSetting(count_, System.Windows.Automation.AutomationLiveSetting.Polite);
         header_.Status = count_;
         header_.Actions = MakeButton(L("_Refresh"), Wpf.Ui.Controls.ControlAppearance.Secondary, (_, _) => Reload());
         root.Children.Add(header_);

         search_.SearchTextChanged += (_, _) => ShowSearch_();
         Grid.SetRow(search_, 1);
         root.Children.Add(search_);

         var notices = new StackPanel();
         notices.Children.Add(notice_);
         notices.Children.Add(misnamed_);
         Grid.SetRow(notices, 2);
         root.Children.Add(notices);

         var host = new Grid();
         host.Children.Add(list_);
         host.Children.Add(empty_);
         var card = new Card { Padding = new Thickness(8), Content = host };
         Grid.SetRow(card, 3);
         root.Children.Add(card);

         // Enter and F2 open the selected folder's permissions, Delete deletes it
         // and Insert adds one; a double-click on a folder opens its permissions.
         GridKeys.Wire(list_, permissions, delete, addButton_);

         Content = root;
      }

      /// <summary>
      /// A folder's row: its own name, a folder's glyph before it, as far in as the
      /// folder is down the tree. The row is the folder's full name to a screen
      /// reader and the search (PublicFolderTree.Folder.ToString), Sales.Leads, so
      /// that where it is in the tree is heard as well as seen.
      /// </summary>
      private static DataTemplate FolderRowTemplate_()
      {
         var row = new FrameworkElementFactory(typeof(StackPanel));
         row.SetValue(StackPanel.OrientationProperty, Orientation.Horizontal);
         row.SetBinding(MarginProperty, new System.Windows.Data.Binding(nameof(PublicFolderTree.Folder.Depth)) { Converter = IndentConverter.Instance });

         var glyph = new FrameworkElementFactory(typeof(Wpf.Ui.Controls.SymbolIcon));
         glyph.SetValue(Wpf.Ui.Controls.SymbolIcon.SymbolProperty, Wpf.Ui.Controls.SymbolRegular.Folder24);
         glyph.SetValue(Wpf.Ui.Controls.SymbolIcon.FontSizeProperty, 16.0);
         glyph.SetValue(VerticalAlignmentProperty, VerticalAlignment.Center);
         glyph.SetValue(MarginProperty, new Thickness(0, 0, 8, 0));
         row.AppendChild(glyph);

         var name = new FrameworkElementFactory(typeof(TextBlock));
         name.SetBinding(TextBlock.TextProperty, new System.Windows.Data.Binding(nameof(PublicFolderTree.Folder.Name)));
         name.SetValue(TextBlock.TextTrimmingProperty, TextTrimming.CharacterEllipsis);
         name.SetValue(VerticalAlignmentProperty, VerticalAlignment.Center);
         row.AppendChild(name);

         return new DataTemplate { VisualTree = row };
      }

      /// <summary>A folder's depth as the left margin of its row.</summary>
      private sealed class IndentConverter : System.Windows.Data.IValueConverter
      {
         public static readonly IndentConverter Instance = new();

         public object Convert(object value, Type targetType, object parameter, System.Globalization.CultureInfo culture)
            => new Thickness((value is int depth ? depth : 0) * IndentPerLevel, 0, 0, 0);

         public object ConvertBack(object value, Type targetType, object parameter, System.Globalization.CultureInfo culture)
            => throw new NotSupportedException();
      }

      private static Wpf.Ui.Controls.Button MakeButton(string text, Wpf.Ui.Controls.ControlAppearance appearance, RoutedEventHandler onClick)
      {
         var b = new Wpf.Ui.Controls.Button { Content = text, Appearance = appearance, Margin = new Thickness(8, 0, 0, 0), MinWidth = 92 };
         b.Click += onClick;
         return b;
      }

      /// <summary>What the last action said, at its level.</summary>
      private void Notice_(StatusLevel level, string text)
      {
         ReadFailure.Clear(notice_);
         notice_.Level = level;
         notice_.Text = text;
         notice_.Visibility = Visibility.Visible;
         Announce.Request(notice_);
      }

      /// <summary>The folders as the page lists them, after a read that worked; empty otherwise.</summary>
      private List<PublicFolderTree.Folder> Folders_ => list_.ItemsSource as List<PublicFolderTree.Folder> ?? new List<PublicFolderTree.Folder>();

      private void Reload() => Reload_(null, -1);

      /// <summary>
      /// Reads the folders' tree, then selects the folder of <paramref name="selectPath"/>
      /// (the one just added) or, where it is gone, the row at <paramref name="selectIndex"/>
      /// (the one that took a deleted folder's place), and brings it into view; with
      /// neither, the folder that was selected stays selected. A folder is found by its
      /// path, not its full name, which a folder at the top whose own name holds the
      /// delimiter shares with a sub-folder (PublicFolderTree.SamePath).
      /// </summary>
      private void Reload_(IReadOnlyList<string> selectPath, int selectIndex)
      {
         IReadOnlyList<string> kept = selectPath ?? (list_.SelectedItem as PublicFolderTree.Folder)?.Path;
         var paths = new List<IReadOnlyList<string>>();
         try
         {
            // Inside the try with the rest: the collection itself was read on the
            // line before it, so a link that had gone threw past every catch here
            // and the page kept "No public folders yet." and its count.
            dynamic settings = ServerSession.Current.Application.Settings;
            string delimiter = null;
            try
            {
               delimiter = (string)settings.IMAPHierarchyDelimiter;
            }
            catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
            {
               // The server's default, where it says none.
            }
            delimiter_ = PublicFolderTree.DelimiterOf(delimiter);

            dynamic folders = settings.PublicFolders;
            try
            {
               ReadLevel_(folders, new List<string>(), paths);
            }
            finally
            {
               ServerSession.Release(folders);
               ServerSession.Release(settings);
            }
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            // Neither the names of the last read nor "No public folders yet." nor
            // a count is a fact this page has now: all three go, the notice says
            // what happened, with Try again, and Add folder waits for a read that
            // works (Services/ReadFailure).
            ReadFailure.Show(notice_, empty_, list_, L("Could not read the public folders: "), ex, Reload, addButton_);
            count_.Visibility = Visibility.Collapsed;
            misnamed_.Visibility = Visibility.Collapsed;
            return;
         }

         IReadOnlyList<PublicFolderTree.Folder> folderList = PublicFolderTree.Build(paths, delimiter_);
         ReadFailure.Clear(notice_, addButton_);
         list_.ItemsSource = folderList.ToList();
         header_.Subtitle = PublicFolderTree.Subtitle(delimiter_);
         count_.Visibility = Visibility.Visible;

         string misnamed = PublicFolderTree.MisnamedNotice(folderList, delimiter_);
         misnamed_.Text = misnamed;
         misnamed_.Visibility = misnamed == null ? Visibility.Collapsed : Visibility.Visible;

         // The empty state says what to do, not only that there is nothing:
         // "0 public folders." left a first-time administrator with a blank card.
         StatusText.Show(empty_, notice_, folderList.Count, null, EmptySentence_);
         ShowSearch_();

         PublicFolderTree.Folder again = PublicFolderTree.Find(Folders_, kept);
         if (again == null && selectIndex >= 0)
         {
            int at = ListOutcome.IndexAfterDelete(Folders_.Count, selectIndex);
            again = at >= 0 ? Folders_[at] : null;
         }

         if (again != null)
         {
            // A folder just made, or kept after a delete that failed, is not left
            // out of the list by a search typed in before.
            if (selectPath != null)
               ListSearchState.Reveal(search_, list_, again);
            list_.SelectedItem = again;
            if (selectPath != null || selectIndex >= 0)
               list_.ScrollIntoView(again);
         }
      }

      /// <summary>Every folder of one level and, under each, the folders of the next, as the names from the top down.</summary>
      private static void ReadLevel_(dynamic folders, List<string> above, List<IReadOnlyList<string>> into)
      {
         if (above.Count >= PublicFolderTree.MaxDepth)
            return;

         int count = (int)folders.Count;
         for (int i = 0; i < count; i++)
         {
            dynamic folder = folders.Item[i];
            dynamic subFolders = null;
            try
            {
               var path = new List<string>(above) { (string)folder.Name };
               into.Add(path);
               subFolders = folder.SubFolders;
               ReadLevel_(subFolders, path, into);
            }
            finally
            {
               ServerSession.Release((object)subFolders);
               ServerSession.Release((object)folder);
            }
         }
      }

      /// <summary>
      /// The public folder at <paramref name="path"/>, the names from the top down,
      /// reached through each folder's SubFolders - where the folder of a name is
      /// found under its parent, never at the top. The caller releases it.
      /// FolderPermissionsDialog.OpenFolder walks the same way.
      /// </summary>
      internal static dynamic OpenFolderAt(dynamic folders, IReadOnlyList<string> path)
      {
         dynamic folder = folders.ItemByName[path[0]];
         for (int i = 1; i < path.Count; i++)
         {
            dynamic subFolders = folder.SubFolders;
            try
            {
               dynamic next = subFolders.ItemByName[path[i]];
               ServerSession.Release((object)folder);
               folder = next;
            }
            finally
            {
               ServerSession.Release((object)subFolders);
            }
         }

         return folder;
      }

      /// <summary>
      /// The search over the folders read: the list filtered as it is typed in, the
      /// count beside the title saying how many it shows while it is ("Shown: 3 of
      /// 4", Services/ListSearchCount), and where it hides every folder, the empty
      /// state saying that none has what was typed in its name. A folder's name is
      /// its full name, Sales.Leads.
      /// </summary>
      private void ShowSearch_()
      {
         if (ReadFailure.IsShown(notice_) || list_.ItemsSource is not List<PublicFolderTree.Folder> folders)
            return;

         string query = search_.SearchText?.Trim() ?? "";
         ListSearch.Apply(list_, query, item => (item as PublicFolderTree.Folder)?.FullName ?? "");
         int shown = folders.Count(folder => ListSearch.Contains(folder.FullName, query));
         count_.Text = ListSearchCount.PillText(query, shown, folders.Count, Plural(folders.Count, "{0} public folder", "{0} public folders"));
         if (ListSearchCount.NothingMatches(query, shown, folders.Count))
         {
            empty_.Text = F("No public folder has \"{0}\" in its name.", query);
            empty_.Visibility = Visibility.Visible;
            return;
         }

         StatusText.Show(empty_, null, folders.Count, null, EmptySentence_);
      }

      /// <summary>
      /// Add folder: the name asked for in a dialog that opens on the selected
      /// folder's name and the delimiter, and makes the folder while it is open, so
      /// that a name refused is said under its box; the new folder is selected and
      /// the page says it was added (review finding F206).
      /// </summary>
      private void AddFolder()
      {
         string prefill = PublicFolderTree.Prefill(list_.SelectedItem as PublicFolderTree.Folder, delimiter_);
         var dialog = new PublicFolderNameDialog(Window.GetWindow(this), delimiter_, prefill, CreateFolder_);
         if (dialog.ShowDialog() != true)
            return;

         IReadOnlyList<string> path = PublicFolderTree.Split(dialog.CreatedName, delimiter_);
         string full = string.Join(delimiter_, path);
         Reload_(path, -1);
         if (!ReadFailure.IsShown(notice_))
            Notice_(StatusLevel.Good, ListOutcome.Added(full, DateTime.Now));
      }

      /// <summary>
      /// Makes the folder of a typed name: each name of its path under the one before
      /// it, the folders that are there already used as they are and the others made,
      /// through Settings.PublicFolders at the top and each folder's SubFolders below.
      /// Null once it is made; otherwise what to say under the name's box.
      /// </summary>
      private string CreateFolder_(string typed)
      {
         string problem = PublicFolderTree.NameProblem(typed, delimiter_, Folders_);
         if (problem != null)
            return problem;

         IReadOnlyList<string> path = PublicFolderTree.Split(typed, delimiter_);
         var held = new List<object>();
         try
         {
            dynamic level = ServerSession.Current.Application.Settings.PublicFolders;
            held.Add((object)level);
            for (int i = 0; i < path.Count; i++)
            {
               dynamic folder = FindChild_(level, path[i]) ?? level.Add(path[i]);
               held.Add((object)folder);
               if (i == path.Count - 1)
                  break;

               level = folder.SubFolders;
               held.Add((object)level);
            }

            return null;
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            return F("Could not create the folder: {0}", ServerSession.DescribeComError(ex));
         }
         finally
         {
            for (int i = held.Count - 1; i >= 0; i--)
               ServerSession.Release(held[i]);
         }
      }

      /// <summary>The folder of this name in one level of the tree, without regard to case as the server compares, or null.</summary>
      private static dynamic FindChild_(dynamic folders, string name)
      {
         int count = (int)folders.Count;
         for (int i = 0; i < count; i++)
         {
            dynamic folder = folders.Item[i];
            if (string.Equals((string)folder.Name, name, StringComparison.OrdinalIgnoreCase))
               return folder;
            ServerSession.Release((object)folder);
         }

         return null;
      }

      private void DeleteFolder()
      {
         if (list_.SelectedItem is not PublicFolderTree.Folder folder)
         {
            Notice_(StatusLevel.Information, L("Select a folder first."));
            return;
         }

         // The question says how many folders go with it: Delete takes the
         // folders under a folder with it, which "Delete Sales and every message
         // in it?" asked over Sales.Leads did not say (review finding F206).
         if (!MessageBox.Confirm(PublicFolderTree.DeleteQuestion(folder), L("Control Panel"), L("_Delete folder"), destructive: true))
            return;

         int index = list_.SelectedIndex;
         string failure = null;
         dynamic folders = ServerSession.Current.Application.Settings.PublicFolders;
         try
         {
            dynamic target = OpenFolderAt(folders, folder.Path);
            target.Delete();
            ServerSession.Release((object)target);
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            failure = F("Could not delete the folder: {0}", ServerSession.DescribeComError(ex));
         }
         finally
         {
            ServerSession.Release(folders);
         }

         // The list first, then the outcome: the row that took the folder's place
         // is selected; a delete that failed keeps the folder selected.
         Reload_(failure == null ? null : folder.Path, failure == null ? index : -1);
         if (ReadFailure.IsShown(notice_))
            return;
         if (failure != null)
            Notice_(StatusLevel.Critical, failure);
         else
            Notice_(StatusLevel.Good, ListOutcome.Deleted(folder.FullName));
      }

      private void EditPermissions()
      {
         if (list_.SelectedItem is not PublicFolderTree.Folder folder)
         {
            Notice_(StatusLevel.Information, L("Select a folder first."));
            return;
         }
         new FolderPermissionsDialog(Window.GetWindow(this), folder.Path, folder.FullName).ShowDialog();
      }
   }
}
