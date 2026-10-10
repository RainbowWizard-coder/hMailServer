// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Collections.Generic;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using hMailServer.ControlPanel.Services;

namespace hMailServer.ControlPanel.Views
{
   /// <summary>
   /// An LDAP search filter's box on the Directory synchronisation and Directory
   /// authentication (LDAP) pages, shown whole (review finding F131): the width of
   /// its card, wrapping over two lines at least, in the monospaced face filters
   /// are read in. The filter the server uses when the box is empty is said by
   /// each page in its hint, where it stays in view once the box holds a filter of
   /// its own, inside a sentence the hint already has: a sentence of its own
   /// appended here made the synchronisation filter's hint three sentences, past
   /// the two the owner's decision 11 keeps on a page (found re-checking
   /// 1f68715cf).
   ///
   /// The synchronisation filter was a single line capped at 620 pixels, with
   /// about 200 pixels of its card unused, and its placeholder - the one place
   /// the product stated the default - was cut at
   /// "...(!(userAccountControl:1.2.840.113556.1.4.80", the start of the clause
   /// that keeps disabled accounts out, which is the whole of what the caption
   /// promises. A placeholder cannot be scrolled, and goes as soon as the box has
   /// text.
   /// </summary>
   internal static class LdapFilterField
   {
      /// <summary>Gives <paramref name="box"/> the shape of a filter shown whole.</summary>
      public static void ShowWhole(Wpf.Ui.Controls.TextBox box)
      {
         if (box == null)
            return;

         box.HorizontalAlignment = HorizontalAlignment.Stretch;
         box.MaxWidth = double.PositiveInfinity;
         box.TextWrapping = TextWrapping.Wrap;
         box.MinLines = 2;
         box.FontFamily = new FontFamily(Typography.MonoFontFamily);
         box.FontSize = Typography.Mono;

         // The placeholder wraps with the text: the box's template draws it in a
         // TextBlock of its own, on one line, so that the default was cut at the
         // box's edge however wide the box was.
         box.Loaded += (_, _) => WrapPlaceholder_(box);
      }

      private static void WrapPlaceholder_(DependencyObject at)
      {
         if (at is not Wpf.Ui.Controls.TextBox box || string.IsNullOrEmpty(box.PlaceholderText))
            return;

         foreach (TextBlock words in Below_(box))
         {
            if (words.Text == box.PlaceholderText)
               words.TextWrapping = TextWrapping.Wrap;
         }
      }

      private static IEnumerable<TextBlock> Below_(DependencyObject parent)
      {
         int count = VisualTreeHelper.GetChildrenCount(parent);
         for (int i = 0; i < count; i++)
         {
            DependencyObject child = VisualTreeHelper.GetChild(parent, i);
            if (child is TextBlock words)
               yield return words;
            foreach (TextBlock below in Below_(child))
               yield return below;
         }
      }
   }
}
