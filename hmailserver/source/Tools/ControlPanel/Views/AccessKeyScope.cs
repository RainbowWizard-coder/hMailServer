// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using hMailServer.ControlPanel.Views.Scaffold;

namespace hMailServer.ControlPanel.Views
{
   /// <summary>
   /// Access keys answered by the card that holds the keyboard, for a page whose
   /// cards together hold more captions than there are letters. The Backup &amp;
   /// restore page is one scrolling page of five cards and some thirty-five
   /// captions, so letters were shared - Alt+P was the encryption passphrase,
   /// the archives to keep and Put the message back - and WPF, given two
   /// enabled targets for one key, moves the focus between them and presses
   /// neither (review finding F287).
   ///
   /// On a page whose root says <c>local:AccessKeyScope.ByCard="True"</c>, a key
   /// pressed while the keyboard is in a card that has a caption with that key
   /// goes to that card's caption and no other; a key the card does not have
   /// goes to the page as a whole, as on any other page, so a key that is the
   /// page's own - outside every card - works from anywhere. That is the rule
   /// build/check-mnemonics.py holds such a page to: within each card no two
   /// captions share a key, and no caption outside the cards shares one with
   /// any card's.
   ///
   /// It is WPF's own scoping: the access-key manager raises AccessKeyPressed on
   /// the focused element and on every element registered for the key, and keeps
   /// only the targets whose scope - the Scope a handler on the event's route
   /// sets, or the window when none does - is the focused element's. The handler
   /// here, on the page, sets the scope to the card for the elements inside a
   /// card that holds the keyboard and answers the key, and leaves it unset for
   /// everything else.
   /// </summary>
   public static class AccessKeyScope
   {
      public static readonly DependencyProperty ByCardProperty = DependencyProperty.RegisterAttached(
         "ByCard", typeof(bool), typeof(AccessKeyScope), new PropertyMetadata(false, OnByCardChanged)); // no-loc

      public static bool GetByCard(DependencyObject element) => (bool)element.GetValue(ByCardProperty);

      public static void SetByCard(DependencyObject element, bool value) => element.SetValue(ByCardProperty, value);

      private static readonly AccessKeyPressedEventHandler Handler = OnAccessKeyPressed;

      private static void OnByCardChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
      {
         if (d is not UIElement page)
            return;

         // Handled events too: a control may mark the event handled on its way up,
         // and the scope is still the page's to set.
         page.RemoveHandler(AccessKeyManager.AccessKeyPressedEvent, Handler);
         if ((bool)e.NewValue)
            page.AddHandler(AccessKeyManager.AccessKeyPressedEvent, Handler, true);
      }

      private static void OnAccessKeyPressed(object sender, AccessKeyPressedEventArgs e)
      {
         // A scope set nearer the element (a menu, a popup) is that element's.
         if (e.Scope != null || sender is not DependencyObject page)
            return;

         Card card = CardOf(e.OriginalSource as DependencyObject, page);
         if (card == null || !card.IsKeyboardFocusWithin || !Answers(card, e.Key))
            return;

         e.Scope = card;
      }

      /// <summary>The innermost card between an element and the page, or null.</summary>
      public static Card CardOf(DependencyObject element, DependencyObject page)
      {
         for (DependencyObject d = element; d != null && d != page; d = Parent(d))
         {
            if (d is Card card)
               return card;
         }

         return null;
      }

      /// <summary>
      /// Whether the card holds a caption with the key that can be pressed: a
      /// button's or a check box's underlined letter (its AccessText), or a field
      /// caption's, which <see cref="Mnemonic"/> tells UI Automation on the editor
      /// as "Alt+K" - visible and enabled, since the manager passes over any other.
      /// </summary>
      public static bool Answers(DependencyObject card, string key)
      {
         if (string.IsNullOrEmpty(key))
            return false;

         string upper = key.ToUpperInvariant();
         string automation = "Alt+" + upper; // no-loc
         return Any(card, d =>
            d is UIElement element && element.IsVisible && element.IsEnabled
            && ((d is AccessText text && char.ToUpperInvariant(text.AccessKey).ToString() == upper)
               || automation.Equals(AutomationProperties.GetAccessKey(d), System.StringComparison.OrdinalIgnoreCase)));
      }

      private static bool Any(DependencyObject root, System.Func<DependencyObject, bool> test)
      {
         if (test(root))
            return true;

         int count = root is Visual ? VisualTreeHelper.GetChildrenCount(root) : 0;
         for (int i = 0; i < count; i++)
         {
            if (Any(VisualTreeHelper.GetChild(root, i), test))
               return true;
         }

         return false;
      }

      private static DependencyObject Parent(DependencyObject d)
         => (d is Visual ? VisualTreeHelper.GetParent(d) : null) ?? LogicalTreeHelper.GetParent(d);
   }
}
