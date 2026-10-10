// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Collections.Generic;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using hMailServer.ControlPanel.Services;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Views
{
   /// <summary>
   /// An IP range's delivery directions, one row each, on the IP ranges page's
   /// permissions card and the range dialog's Delivery tab alike: the direction's own
   /// box, which allows it, and Only after SMTP authentication beside it (review
   /// finding F229). The card showed relaying apart from its authentication, which the
   /// dialog alone held on a tab of its own, and the two called every setting by
   /// another name.
   ///
   /// The direction's box carries the direction as its caption and key - which is
   /// also how the settings search index finds it (build/generate-settings-index.ps1
   /// reads a hand-written page's controls by their captions); the authentication box
   /// beside it, which has no caption of its own, is named for its row and its column,
   /// which a screen reader does not see, and counts only while the direction is
   /// allowed, and follows it.
   /// </summary>
   internal static class DeliveryRow
   {
      /// <summary>Names a row's authentication box and makes it follow the direction's box.</summary>
      public static void Wire(CheckBox allowed, CheckBox authentication)
      {
         string direction = MnemonicText.Strip(allowed.Content as string ?? "");
         AutomationProperties.SetName(authentication, F("{0}: only after SMTP authentication", direction));
         DialogFields.EnabledBy(allowed, authentication);
      }

      /// <summary>The rows under their column headers, as the dialog's Delivery tab lays them out.</summary>
      public static Grid BuildGrid(IEnumerable<(string Caption, CheckBox Allowed, CheckBox Authentication)> rows)
      {
         var grid = new Grid();
         grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto, MinWidth = 240 });
         grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
         grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });

         Header_(grid, 0, L("Allow delivery"));
         Header_(grid, 1, L("Only after SMTP authentication"));

         int row = 1;
         foreach ((string caption, CheckBox allowed, CheckBox authentication) in rows)
         {
            grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            allowed.Content = caption;
            allowed.Margin = new Thickness(0, 4, 16, 4);
            Place_(grid, allowed, row, 0);
            authentication.Margin = new Thickness(0, 4, 0, 4);
            Place_(grid, authentication, row, 1);
            Wire(allowed, authentication);
            row++;
         }

         return grid;
      }

      private static void Header_(Grid grid, int column, string text)
      {
         var header = new TextBlock { Text = text, TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 0, column == 0 ? 16 : 0, 4) };
         header.SetResourceReference(FrameworkElement.StyleProperty, "TextBodyStrong");
         Place_(grid, header, 0, column);
      }

      private static void Place_(Grid grid, UIElement element, int row, int column)
      {
         System.Windows.Controls.Grid.SetRow(element, row);
         System.Windows.Controls.Grid.SetColumn(element, column);
         grid.Children.Add(element);
      }
   }
}
