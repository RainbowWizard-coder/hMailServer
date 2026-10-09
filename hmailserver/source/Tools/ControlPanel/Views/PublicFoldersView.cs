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
      private readonly TextBlock count_ = new() { Margin = new Thickness(0, 12, 0, 0) };

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
         root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
         root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });

         header_.PageKey = "publicfolders";
         header_.Subtitle = L("Shared IMAP folders that several accounts can access. Use the hierarchy delimiter to create sub-folders, and edit permissions to grant access.");
         System.Windows.Automation.AutomationProperties.SetName(list_, L("Public folders"));

         var actions = new StackPanel { Orientation = Orientation.Horizontal };
         addButton_ = MakeButton(L("_Add folder"), Wpf.Ui.Controls.ControlAppearance.Primary, (_, _) => AddFolder());
         actions.Children.Add(addButton_);
         var permissions = MakeButton(L("_Permissions"), Wpf.Ui.Controls.ControlAppearance.Secondary, (_, _) => EditPermissions());
         actions.Children.Add(permissions);
         var delete = MakeButton(L("_Delete"), Wpf.Ui.Controls.ControlAppearance.Secondary, (_, _) => DeleteFolder());
         hMailServer.ControlPanel.Services.ButtonStyles.DangerText(delete);
         actions.Children.Add(delete);
         hMailServer.ControlPanel.Services.SelectionGate.Bind(list_, permissions, delete);
         actions.Children.Add(MakeButton(L("_Refresh"), Wpf.Ui.Controls.ControlAppearance.Secondary, (_, _) => Reload()));
         header_.Actions = actions;
         root.Children.Add(header_);

         Grid.SetRow(notice_, 1);
         root.Children.Add(notice_);

         var host = new Grid();
         host.Children.Add(list_);
         host.Children.Add(empty_);
         var card = new Card { Padding = new Thickness(8), Content = host };
         Grid.SetRow(card, 2);
         root.Children.Add(card);

         // Enter and F2 open the selected folder's permissions, Delete deletes it
         // and Insert adds one; a double-click on a folder opens its permissions.
         GridKeys.Wire(list_, permissions, delete, addButton_);

         count_.SetResourceReference(StyleProperty, "TextCaption");
         Grid.SetRow(count_, 3);
         root.Children.Add(count_);

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
               count_.Text = Plural(count, "{0} public folder.", "{0} public folders.");
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
            count_.Text = "";
            return;
         }

         ReadFailure.Clear(notice_, addButton_);
         list_.ItemsSource = names;
         // The empty state says what to do, not only that there is nothing:
         // "0 public folders." left a first-time administrator with a blank card.
         StatusText.Show(empty_, notice_, names.Count, null,
            L("No public folders yet. Add folder creates one that several accounts can open; Permissions then decides who may read and write it."));
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
