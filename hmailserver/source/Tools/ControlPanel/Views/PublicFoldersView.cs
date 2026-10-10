// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
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
   /// </summary>
   public class PublicFoldersView : UserControl, IPageLifecycle
   {
      private readonly ListBox list_ = new()
      {
         FontSize = Typography.Body,
         BorderThickness = new Thickness(0),
         Background = System.Windows.Media.Brushes.Transparent
      };
      private readonly PageHeader header_ = new();
      private readonly InlineNotice notice_ = new() { Visibility = Visibility.Collapsed };
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
         header_.Subtitle = L("Shared IMAP folders that several accounts can access. Use the hierarchy delimiter to create sub-folders, and edit permissions to grant access.");
         System.Windows.Automation.AutomationProperties.SetName(list_, L("Public folders"));

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

         Grid.SetRow(notice_, 2);
         root.Children.Add(notice_);

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
      }

      private void Reload()
      {
         var names = new List<string>();
         try
         {
            // Inside the try with the rest: the collection itself was read on the
            // line before it, so a link that had gone threw past every catch here
            // and the page kept "No public folders yet." and its count.
            dynamic folders = ServerSession.Current.Application.Settings.PublicFolders;
            try
            {
               int count = (int)folders.Count;
               for (int i = 0; i < count; i++)
               {
                  dynamic f = folders.Item[i];
                  names.Add((string)f.Name);
                  ServerSession.Release(f);
               }
            }
            finally
            {
               ServerSession.Release(folders);
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
            return;
         }

         ReadFailure.Clear(notice_, addButton_);
         list_.ItemsSource = names;
         count_.Visibility = Visibility.Visible;
         // The empty state says what to do, not only that there is nothing:
         // "0 public folders." left a first-time administrator with a blank card.
         StatusText.Show(empty_, notice_, names.Count, null, EmptySentence_);
         ShowSearch_();
      }

      /// <summary>
      /// The search over the folders read: the list filtered as it is typed in, the
      /// count beside the title saying how many it shows while it is ("Shown: 3 of
      /// 4", Services/ListSearchCount), and where it hides every folder, the empty
      /// state saying that none has what was typed in its name.
      /// </summary>
      private void ShowSearch_()
      {
         if (ReadFailure.IsShown(notice_) || list_.ItemsSource is not List<string> names)
            return;

         string query = search_.SearchText?.Trim() ?? "";
         ListSearch.Apply(list_, query);
         int shown = names.FindAll(name => ListSearch.Contains(name, query)).Count;
         count_.Text = ListSearchCount.PillText(query, shown, names.Count, Plural(names.Count, "{0} public folder", "{0} public folders"));
         if (ListSearchCount.NothingMatches(query, shown, names.Count))
         {
            empty_.Text = F("No public folder has \"{0}\" in its name.", query);
            empty_.Visibility = Visibility.Visible;
            return;
         }

         StatusText.Show(empty_, null, names.Count, null, EmptySentence_);
      }

      private void AddFolder()
      {
         string name = InputDialog.Prompt(Window.GetWindow(this), L("New public folder"), L("Folder name:"));
         if (string.IsNullOrWhiteSpace(name))
            return;

         dynamic folders = ServerSession.Current.Application.Settings.PublicFolders;
         try
         {
            dynamic created = folders.Add(name.Trim());
            ServerSession.Release(created);
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            MessageBox.Show(F("Could not create the folder: {0}", ServerSession.DescribeComError(ex)), L("Control Panel"));
         }
         finally
         {
            ServerSession.Release(folders);
         }
         Reload();
      }

      private void DeleteFolder()
      {
         if (list_.SelectedItem is not string name)
         {
            Notice_(StatusLevel.Information, L("Select a folder first."));
            return;
         }
         if (!MessageBox.Confirm(Confirmations.DeletePublicFolder(name), L("Control Panel"), L("_Delete folder"), destructive: true))
            return;

         dynamic folders = ServerSession.Current.Application.Settings.PublicFolders;
         try
         {
            dynamic folder = folders.ItemByName[name];
            folder.Delete();
            ServerSession.Release(folder);
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            MessageBox.Show(F("Could not delete the folder: {0}", ServerSession.DescribeComError(ex)), L("Control Panel"));
         }
         finally
         {
            ServerSession.Release(folders);
         }
         Reload();
      }

      private void EditPermissions()
      {
         if (list_.SelectedItem is not string name)
         {
            Notice_(StatusLevel.Information, L("Select a folder first."));
            return;
         }
         new FolderPermissionsDialog(Window.GetWindow(this), name).ShowDialog();
      }
   }
}
