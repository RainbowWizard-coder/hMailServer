// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Windows;
using System.Windows.Controls;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// The grid styles the design system keys - GridNumberText in Scaffold.xaml
   /// and GridNumberHeader in App.xaml - for the pages that build their columns
   /// in code, where a XAML page writes ElementStyle and HeaderStyle.
   /// </summary>
   public static class GridStyles
   {
      /// <summary>
      /// A text column whose cells end in an ellipsis when the column is too
      /// narrow, and say the whole of themselves in a tool tip. A grid cell's
      /// TextBlock does not trim by default, so a sentence in a fixed column was
      /// cut mid-word with nothing to say so - "The accounts, mailboxes and
      /// quarantine of nam" on the roles page.
      /// </summary>
      public static T Trimmed<T>(T column) where T : DataGridBoundColumn
      {
         if (column == null)
            return column;

         var style = new Style(typeof(TextBlock));
         if (column.ElementStyle != null)
            style.BasedOn = column.ElementStyle;
         style.Setters.Add(new Setter(TextBlock.TextTrimmingProperty, TextTrimming.CharacterEllipsis));
         style.Setters.Add(new Setter(FrameworkElement.ToolTipProperty,
            new System.Windows.Data.Binding(nameof(TextBlock.Text)) { RelativeSource = System.Windows.Data.RelativeSource.Self }));
         // An empty cell has nothing to say, and an empty tool tip is a small grey box.
         var empty = new Trigger { Property = TextBlock.TextProperty, Value = "" };
         empty.Setters.Add(new Setter(FrameworkElement.ToolTipProperty, null));
         style.Triggers.Add(empty);
         column.ElementStyle = style;
         return column;
      }

      /// <summary>Right-aligns a number column's cells and its header.</summary>
      public static void Number(DataGridBoundColumn column)
      {
         if (column == null || Application.Current == null)
            return;

         if (Application.Current.TryFindResource("GridNumberText") is Style cells)
            column.ElementStyle = cells;

         if (Application.Current.TryFindResource("GridNumberHeader") is Style header)
            column.HeaderStyle = header;
      }
   }
}
