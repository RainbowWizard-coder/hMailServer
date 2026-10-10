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
         Window window = Application.Current?.MainWindow;
         FontFamily family = window?.FontFamily ?? new FontFamily("Segoe UI Variable Text, Segoe UI"); // no-loc
         // In the window's text formatting mode and at its DPI, as CellWidth measures:
         // measured by ideal metrics, a header of Latin letters came out a few pixels
         // narrower than the window draws it, and a column floored at it - the
         // Suppression list's "Last status code" - was cut by its last letters.
         TextFormattingMode mode = window != null ? TextOptions.GetTextFormattingMode(window) : TextFormattingMode.Ideal;
         double pixelsPerDip = window != null ? VisualTreeHelper.GetDpi(window).PixelsPerDip : 1.0;
         var words = new FormattedText(header, CultureInfo.CurrentUICulture, FlowDirection.LeftToRight,
            new Typeface(family, FontStyles.Normal, weight, FontStretches.Normal), size, Brushes.Black, null, mode, pixelsPerDip);
         // In whole device pixels, up, as the header is drawn: at 150 % a column floored
         // at 147 was drawn 146.67 wide and cut "Asks for a confirmation" by a third of
         // a pixel.
         return WholeDevicePixels(words.WidthIncludingTrailingWhitespace + padding.Left + padding.Right);
      }

      /// <summary>
      /// A width rounded up to the window's whole device pixels. Layout rounding draws
      /// an element at the nearest device pixel, so that a column floored at a width
      /// that falls between two - 147 at 150 % is 220.5 of them - is drawn at the one
      /// below and cuts by a fraction of a pixel the words it was floored for.
      /// </summary>
      public static double WholeDevicePixels(double width)
      {
         Window window = Application.Current?.MainWindow;
         double pixelsPerDip = window != null ? VisualTreeHelper.GetDpi(window).PixelsPerDip : 1.0;
         return Math.Ceiling(width * pixelsPerDip - 1e-6) / pixelsPerDip;
      }

      /// <summary>
      /// Floors each of a grid's bound columns, but those named, at its header and at
      /// the widest of its words among the items shown, in whole device pixels. A grid
      /// whose star column is floored takes what the star lacks from its other columns
      /// once it is narrowed, down to 20 pixels each, and cuts their words - the audit
      /// trail's at 1280 once the window had been wider; floored at their words they
      /// keep them, and the grid scrolls sideways. Each item's value is read by the
      /// column's binding path, a property of the item.
      /// </summary>
      public static void FloorAtTheirWords(DataGrid grid, System.Collections.IEnumerable items, params DataGridColumn[] except)
      {
         if (grid == null)
            return;

         foreach (DataGridColumn column in grid.Columns)
         {
            if (Array.IndexOf(except, column) >= 0 || column is not DataGridBoundColumn bound || bound.Binding is not System.Windows.Data.Binding binding || binding.Path == null)
               continue;

            double floor = HeaderWidth(column.Header as string);
            if (items != null)
            {
               foreach (object item in items)
               {
                  object value = item?.GetType().GetProperty(binding.Path.Path)?.GetValue(item);
                  floor = Math.Max(floor, CellWidth(value as string ?? value?.ToString()));
               }
            }

            column.MinWidth = WholeDevicePixels(floor);
         }
      }

      /// <summary>
      /// The width a cell's words take in the cell's font - 13 pixels regular in
      /// the window's family, read off the application's cell style - with the
      /// cell's padding and whatever else the cell draws before them, so that a
      /// column can be floored at its widest word. A fixed column shrinks to its
      /// MinWidth when the grid is narrower than its columns and a star column
      /// has given up all it can, and a cell's TextBlock does not trim by itself,
      /// so a word wider than the column is cut mid-glyph with nothing to say so:
      /// the TCP/IP ports page's Listening column read "待ち受けていませ" for
      /// 待ち受けていません (Not listening) once the page kept 12 pixels clear
      /// of its scroll bar.
      /// </summary>
      public static double CellWidth(string text, double before = 0)
      {
         if (string.IsNullOrEmpty(text))
            return 0;

         Style style = Application.Current?.TryFindResource(typeof(DataGridCell)) as Style;
         double size = Setting(style, Control.FontSizeProperty, 13.0);
         FontWeight weight = Setting(style, Control.FontWeightProperty, FontWeights.Normal);
         Window window = Application.Current?.MainWindow;
         FontFamily family = window?.FontFamily ?? new FontFamily("Segoe UI Variable Text, Segoe UI"); // no-loc
         // Measured the way the window draws its words: in its text formatting
         // mode (Display snaps each glyph's advance to a whole pixel, so a word
         // comes out a little wider than its ideal metrics) and at its DPI.
         TextFormattingMode mode = window != null ? TextOptions.GetTextFormattingMode(window) : TextFormattingMode.Ideal;
         double pixelsPerDip = window != null ? VisualTreeHelper.GetDpi(window).PixelsPerDip : 1.0;
         var words = new FormattedText(text, CultureInfo.CurrentUICulture, FlowDirection.LeftToRight,
            new Typeface(family, FontStyles.Normal, weight, FontStretches.Normal), size, Brushes.Black, null, mode, pixelsPerDip);
         return Math.Ceiling(words.WidthIncludingTrailingWhitespace) + CellTextMargin.Left + CellTextMargin.Right + CellPadding.Left + CellPadding.Right + before;
      }

      /// <summary>The cell template's padding (App.xaml, the DataGridCell style's Border): 10 pixels each side.</summary>
      private static readonly Thickness CellPadding = new(10, 0, 10, 0);

      /// <summary>
      /// The margin WPF's DataGridTextColumn gives the TextBlock in each of its
      /// cells (DataGridTextColumn.DefaultElementStyle): 2 pixels each side,
      /// which a floor measured from the words and the padding alone left out,
      /// so that "STARTTLS (required)" lost its bracket at the floor. A template
      /// column's content carries no such margin; there the four pixels are slack.
      /// </summary>
      private static readonly Thickness CellTextMargin = new(2, 0, 2, 0);

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
