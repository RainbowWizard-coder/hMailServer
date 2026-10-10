// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Windows;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// The button styles the design system keys in App.xaml, for the pages that
   /// build their buttons in code, where a XAML page writes Style="...".
   /// </summary>
   public static class ButtonStyles
   {
      /// <summary>
      /// A list page's destructive action - Delete, Remove, Delete selected - as
      /// a secondary button with its words in the danger colour while it can be
      /// pressed and the theme's disabled grey while it cannot (DangerTextButton
      /// in App.xaml). One look for every list page: some had this, some a
      /// filled red button, and one a red word on a button that stayed red
      /// while disabled. The filled Danger appearance stays where the action is
      /// confirmed or cannot be undone - a confirmation's destructive choice,
      /// Start restore.
      /// </summary>
      public static void DangerText(System.Windows.Controls.Button button)
      {
         // The style targets Wpf.Ui's button; a plain WPF one is left as it is.
         if (button is not Wpf.Ui.Controls.Button fluent || Application.Current == null)
            return;

         fluent.Appearance = Wpf.Ui.Controls.ControlAppearance.Secondary;
         if (Application.Current.TryFindResource("DangerTextButton") is Style style)
            fluent.Style = style;
      }

      /// <summary>
      /// A button in a card that opens another page (CompactPageLink in App.xaml):
      /// the Caption size, 28 pixels tall at the least, its padding 8 by 4. Its
      /// appearance - Transparent for a link, Secondary or Primary on the setup
      /// checklist - is the caller's. The seven pages that built such a link each
      /// set 12 pixels and a 3-pixel padding of their own, a 24-pixel button beside
      /// the 32-pixel buttons of the same card (review finding F042).
      /// </summary>
      public static void CompactPageLink(System.Windows.Controls.Button button)
      {
         if (button is not Wpf.Ui.Controls.Button fluent || Application.Current == null)
            return;

         if (Application.Current.TryFindResource("CompactPageLink") is Style style)
            fluent.Style = style;
      }
   }
}
