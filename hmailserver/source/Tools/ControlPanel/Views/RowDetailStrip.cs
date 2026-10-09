// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using hMailServer.ControlPanel.Services;

namespace hMailServer.ControlPanel.Views
{
   /// <summary>
   /// The selected row's fact that only a tool tip held, under its list: why a
   /// message was held in quarantine, what the receiving server answered for a
   /// suppressed address, an audit row's whole detail, whether a traced event can
   /// be followed. Each was on screen only while the mouse rested on the row, so
   /// a keyboard user never saw it and a screen reader user heard it, if at all,
   /// as the row's help text (review finding F286).
   ///
   /// A compact card, titled with what the fact is, its words in TextBody - which
   /// can be selected and copied, as every text in the Control Panel can - shown
   /// while a row with the fact is selected and collapsed otherwise, so that a
   /// list with nothing selected keeps its height. The row keeps its tool tip,
   /// which its automation peer gives as the row's help text.
   /// </summary>
   public static class RowDetailStrip
   {
      /// <summary>
      /// The strip for <paramref name="list"/>: the card titled
      /// <paramref name="caption"/>'s words for the selected row (none for no
      /// title) and holding <paramref name="text"/>'s; collapsed while no row is
      /// selected or the row has no such fact.
      /// </summary>
      public static Scaffold.Card For(Selector list, Func<object, string> caption, Func<object, string> text)
      {
         var body = new TextBlock { TextWrapping = TextWrapping.Wrap };
         body.SetResourceReference(FrameworkElement.StyleProperty, "TextBody");

         var card = new Scaffold.Card
         {
            Padding = new Thickness(DesignTokens.Space.Lg, 10, DesignTokens.Space.Lg, DesignTokens.Space.Md),
            Margin = new Thickness(0, DesignTokens.Space.Md, 0, 0),
            Content = body,
            Visibility = Visibility.Collapsed
         };
         AutomationProperties.SetAutomationId(card, "row-detail");

         void Show()
         {
            object row = list.SelectedItem;
            string words = row == null ? null : text(row);
            if (string.IsNullOrWhiteSpace(words))
            {
               card.Visibility = Visibility.Collapsed;
               return;
            }

            card.Title = row == null ? null : caption?.Invoke(row);
            body.Text = words.Trim();
            card.Visibility = Visibility.Visible;
         }

         list.SelectionChanged += (_, _) => Show();
         Show();
         return card;
      }
   }
}
