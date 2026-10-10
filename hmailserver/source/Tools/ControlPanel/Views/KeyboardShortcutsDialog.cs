// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using hMailServer.ControlPanel.Services;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Views
{
   /// <summary>
   /// The keys the Control Panel answers on every page and in every list, from
   /// the palette's Keyboard shortcuts row: Services/KeyboardShortcuts.Listed, the
   /// keys in the reader's language beside what they do. One table, so that the
   /// list and the keys the window answers cannot drift apart. On the standard
   /// frame: Close takes Enter and Escape.
   /// </summary>
   public class KeyboardShortcutsDialog : FluentDialogWindow
   {
      public KeyboardShortcutsDialog(Window owner)
      {
         Owner = owner;
         Title = L("Keyboard shortcuts");

         var table = new Grid();
         table.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
         table.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });

         int row = 0;
         foreach (KeyboardShortcut shortcut in KeyboardShortcuts.Listed)
         {
            table.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });

            var keys = new TextBlock
            {
               Text = L(shortcut.Keys),
               Margin = new Thickness(0, 0, DesignTokens.Space.Lg, DesignTokens.Space.Sm),
               VerticalAlignment = VerticalAlignment.Top
            };
            keys.SetResourceReference(StyleProperty, "TextBodyStrong");
            Grid.SetRow(keys, row);
            table.Children.Add(keys);

            var does = new TextBlock
            {
               Text = L(shortcut.Does),
               Margin = new Thickness(0, 0, 0, DesignTokens.Space.Sm),
               VerticalAlignment = VerticalAlignment.Top
            };
            does.SetResourceReference(StyleProperty, "TextBody");
            Grid.SetRow(does, row);
            Grid.SetColumn(does, 1);
            table.Children.Add(does);

            // Each line is read as one: the keys, then what they do.
            AutomationProperties.SetName(does, F("{0}: {1}", L(shortcut.Keys), L(shortcut.Does)));
            row++;
         }

         var close = new Wpf.Ui.Controls.Button { Content = L("Close"), Appearance = Wpf.Ui.Controls.ControlAppearance.Primary, MinWidth = 88 };
         close.Click += (s, e) => Close();

         UseFrame(L("Keyboard shortcuts"), table, close, null, width: DesignTokens.Dialog.WideWidth);
      }
   }
}
