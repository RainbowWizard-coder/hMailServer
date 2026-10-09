// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Globalization;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;

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

      /// <summary>
      /// The width a column header's words take in the header's font - 12 pixels
      /// semibold in the window's family, read off the application's header style
      /// where there is one - with the header's padding, so that a column can be
      /// floored at it. An Auto column's header is not always counted once the
      /// grid is narrower than its columns and scrolls sideways, and a fixed
      /// column's header is cut wherever a translation is wider than the column:
      /// the Suppression list's bounce counts read "メール…" for メールボックス満杯
      /// (Mailbox full) over a 64-pixel floor and "最後のステ…" for 最後のステータス
      /// (Last status) in a 90-pixel column, in the smallest window.
      /// </summary>
      public static double HeaderWidth(string header)
      {
         if (string.IsNullOrEmpty(header))
            return 0;

         Style style = Application.Current?.TryFindResource(typeof(System.Windows.Controls.Primitives.DataGridColumnHeader)) as Style;
         double size = Setting(style, Control.FontSizeProperty, 12.0);
         FontWeight weight = Setting(style, Control.FontWeightProperty, FontWeights.SemiBold);
         Thickness padding = Setting(style, Control.PaddingProperty, new Thickness(10, 8, 10, 8));
         FontFamily family = Application.Current?.MainWindow?.FontFamily ?? new FontFamily("Segoe UI Variable Text, Segoe UI"); // no-loc
         var words = new FormattedText(header, CultureInfo.CurrentUICulture, FlowDirection.LeftToRight,
            new Typeface(family, FontStyles.Normal, weight, FontStretches.Normal), size, Brushes.Black, 1.0);
         return Math.Ceiling(words.WidthIncludingTrailingWhitespace) + padding.Left + padding.Right;
      }

      /// <summary>A style's own value of a property, or the nearest base style's, or the fallback.</summary>
      private static T Setting<T>(Style style, DependencyProperty property, T fallback)
      {
         for (Style s = style; s != null; s = s.BasedOn)
         {
            foreach (SetterBase setter in s.Setters)
            {
               if (setter is Setter set && set.Property == property && set.Value is T value)
                  return value;
            }
         }

         return fallback;
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

      /// <summary>
      /// A header of a few words standing for a caption that is a sentence: the
      /// caption is the header's tool tip and its help text. The header's words
      /// are drawn by a template of this column's own, trimmed as the shared
      /// header template trims them, because that template's tool tip is the
      /// words themselves and would say "SPF" over a header that stands for
      /// "Requires an SPF pass". The style builds on the column's own header
      /// style - a number column's right-aligned one - or the application's.
      /// </summary>
      public static void HeaderCaption(DataGridColumn column, string caption)
      {
         if (column == null || string.IsNullOrEmpty(caption))
            return;

         var words = new FrameworkElementFactory(typeof(TextBlock));
         words.SetBinding(TextBlock.TextProperty, new System.Windows.Data.Binding());
         words.SetValue(TextBlock.TextTrimmingProperty, TextTrimming.CharacterEllipsis);
         words.SetValue(FrameworkElement.ToolTipProperty, caption);

         Style basis = column.HeaderStyle
            ?? (Application.Current?.TryFindResource(typeof(System.Windows.Controls.Primitives.DataGridColumnHeader)) as Style);
         var style = new Style(typeof(System.Windows.Controls.Primitives.DataGridColumnHeader), basis);
         style.Setters.Add(new Setter(ContentControl.ContentTemplateProperty, new DataTemplate { VisualTree = words }));
         style.Setters.Add(new Setter(FrameworkElement.ToolTipProperty, caption));
         style.Setters.Add(new Setter(System.Windows.Automation.AutomationProperties.HelpTextProperty, caption));
         column.HeaderStyle = style;
      }
   }

   /// <summary>
   /// The template of a grid header's words - trimmed, with the whole of them in
   /// a tool tip - for a header that is words, and none for a header that is a
   /// control. The Quarantine page's first two headers are a check box and four
   /// buttons; given the words' template for every header, each was printed as
   /// the name of its type - "System.Windows.Controls.CheckBox" - and could not
   /// be pressed.
   /// </summary>
   public sealed class HeaderTemplateSelector : DataTemplateSelector
   {
      /// <summary>The template for a header that is words.</summary>
      public DataTemplate TextTemplate { get; set; }

      public override DataTemplate SelectTemplate(object item, DependencyObject container)
         => item is UIElement ? null : TextTemplate;
   }
}
