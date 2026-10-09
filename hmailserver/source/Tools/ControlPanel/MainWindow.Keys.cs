// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Linq;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Input;
using System.Windows.Media;
using hMailServer.ControlPanel.Services;
using hMailServer.ControlPanel.Views;

namespace hMailServer.ControlPanel
{
   /// <summary>
   /// The keys the window answers on every page (Services/KeyboardShortcuts holds
   /// the table): Ctrl+K and F1 as before, and F5, Ctrl+F, Ctrl+S and F6, which
   /// did nothing on any page until October 2026, so that a keyboard user had to
   /// Tab through a page's header to refresh it, find its search box or save it,
   /// and could move between the command bar, the navigation, the page and the
   /// help pane only by tabbing through all of them (review finding F014). Escape
   /// with the keyboard in the help pane closes the pane and puts the keyboard on
   /// the Help button that opens it, where the pane closed before only by its own
   /// button or a second F1.
   /// </summary>
   public partial class MainWindow
   {
      // The element of the page that last had the keyboard, so that F6 back into
      // the page returns to it rather than to the top of the page.
      private WeakReference<IInputElement> pageFocus_;

      private void WireShellKeys_()
      {
         ContentHost.GotKeyboardFocus += (s, e) =>
         {
            if (e.NewFocus is DependencyObject d && IsWithin_(d, ContentHost))
               pageFocus_ = new WeakReference<IInputElement>(e.NewFocus);
         };

         // Bubbling, so that a control inside the pane that answers Escape itself
         // keeps it.
         HelpPanel.KeyDown += (s, e) =>
         {
            if (e.Handled || e.Key != Key.Escape || Keyboard.Modifiers != ModifierKeys.None || !helpOpen_)
               return;

            e.Handled = true;
            CloseHelp_();
         };
      }

      private bool HandleShellKey_(KeyEventArgs e)
         => HandleShellKey_(e.Key == Key.System ? e.SystemKey : e.Key, Keyboard.Modifiers);

      /// <summary>
      /// Answers a key the window owns on every page, and says whether it did.
      /// F6 moves between the window's parts whether or not a server is signed in
      /// to; the others act on a page and wait for one.
      /// </summary>
      private bool HandleShellKey_(Key key, ModifierKeys modifiers)
      {
         ShellCommand command = KeyboardShortcuts.Resolve(key.ToString(),
            (modifiers & ModifierKeys.Control) != 0, (modifiers & ModifierKeys.Shift) != 0, (modifiers & ModifierKeys.Alt) != 0);

         switch (command)
         {
            case ShellCommand.NextPart:
            case ShellCommand.PreviousPart:
               return MoveToPart_(command == ShellCommand.PreviousPart);

            case ShellCommand.Palette:
               if (!connected_)
                  return false;
               ShowPalette();
               return true;

            case ShellCommand.Help:
               if (!connected_)
                  return false;
               ToggleHelp_();
               return true;

            case ShellCommand.Refresh:
               if (!connected_)
                  return false;
               // The page's Refresh or Reload, or a page without one read again.
               PageCommands.Refresh(ContentHost.Content as DependencyObject);
               return true;

            case ShellCommand.Save:
               if (!connected_)
                  return false;
               PageCommands.Press(ContentHost.Content as DependencyObject, PageCommand.Save);
               return true;

            case ShellCommand.Search:
               if (!connected_)
                  return false;
               // A page without a search box of its own is searched from the
               // palette, which searches every page.
               if (!PageCommands.FocusSearch(ContentHost.Content as DependencyObject, Keyboard.FocusedElement as DependencyObject))
                  ShowPalette();
               return true;

            default:
               return false;
         }
      }

      /// <summary>The parts of the window that are on screen and have something to take the keyboard.</summary>
      private List<ShellPart> OpenParts_()
      {
         var open = new List<ShellPart>();
         if (FirstFocusable_(CommandBar) != null)
            open.Add(ShellPart.CommandBar);
         if (NavigationTarget_() != null)
            open.Add(ShellPart.Navigation);
         if (FirstFocusable_(ContentHost) != null)
            open.Add(ShellPart.Page);
         if (helpOpen_ && FirstFocusable_(HelpPanel) != null)
            open.Add(ShellPart.Help);
         if (PendingBar.IsVisible && FirstFocusable_(PendingBar) != null)
            open.Add(ShellPart.PendingBar);
         return open;
      }

      /// <summary>The part the keyboard is in, or null when it is in none of them (nothing, a tour's card).</summary>
      private ShellPart? CurrentPart_()
      {
         if (Keyboard.FocusedElement is not DependencyObject focused)
            return null;

         if (IsWithin_(focused, CommandBar))
            return ShellPart.CommandBar;
         if (IsWithin_(focused, NavTree) || IsWithin_(focused, NavRail))
            return ShellPart.Navigation;
         if (IsWithin_(focused, ContentHost) || IsWithin_(focused, BreadcrumbBar))
            return ShellPart.Page;
         if (IsWithin_(focused, HelpPanel))
            return ShellPart.Help;
         if (IsWithin_(focused, PendingBar))
            return ShellPart.PendingBar;
         return null;
      }

      private bool MoveToPart_(bool backwards)
      {
         ShellPart? next = KeyboardShortcuts.NextPart(OpenParts_(), CurrentPart_(), backwards);
         if (next == null)
            return false;

         IInputElement target = next.Value switch
         {
            ShellPart.CommandBar => SearchButton.IsEnabled && SearchButton.IsVisible ? SearchButton : FirstFocusable_(CommandBar),
            ShellPart.Navigation => NavigationTarget_(),
            ShellPart.Page => RememberedPageFocus_() ?? FirstFocusable_(ContentHost),
            ShellPart.Help => FirstFocusable_(HelpPanel),
            ShellPart.PendingBar => FirstFocusable_(PendingBar),
            _ => null
         };

         if (target == null)
            return false;

         Keyboard.Focus(target);
         (target as FrameworkElement)?.BringIntoView();
         return true;
      }

      /// <summary>
      /// Where the keyboard goes in the navigation: the rail's glyph for the page
      /// on screen while the rail shows, otherwise the sidebar's row for it - the
      /// row the arrow keys then move from - or the first row before any page.
      /// </summary>
      private IInputElement NavigationTarget_()
      {
         if (railShowing_)
         {
            if (!NavRail.IsVisible || !NavRail.IsEnabled)
               return null;
            return railItems_.FirstOrDefault(r => r.IsChecked == true && r.IsVisible && r.IsEnabled)
               ?? railItems_.FirstOrDefault(r => r.IsVisible && r.IsEnabled);
         }

         if (!NavTree.IsVisible || !NavTree.IsEnabled)
            return null;

         if (NavTree.SelectedItem is TreeViewItem selected && selected.IsVisible)
            return selected;

         return NavTree.Items.OfType<TreeViewItem>().FirstOrDefault(i => i.IsVisible && i.IsEnabled);
      }

      private IInputElement RememberedPageFocus_()
      {
         if (pageFocus_ != null && pageFocus_.TryGetTarget(out IInputElement element)
            && element is UIElement ui && ui.IsVisible && ui.IsEnabled && ui.Focusable && IsWithin_(ui, ContentHost))
            return element;

         return null;
      }

      /// <summary>The first element under <paramref name="root"/> that Tab would stop on, in visual order.</summary>
      private static UIElement FirstFocusable_(DependencyObject root)
      {
         if (root is UIElement r && (!r.IsVisible || !r.IsEnabled))
            return null;

         return PageCommands.Descendants_(root).OfType<UIElement>().FirstOrDefault(IsTabStop_);
      }

      private static bool IsTabStop_(UIElement element)
      {
         if (!element.Focusable || !element.IsVisible || !element.IsEnabled)
            return false;
         // A page's scroll viewer takes the keyboard to scroll by, which says
         // nothing on screen; the part's first control is where F6 should land.
         if (element is ScrollViewer)
            return false;
         if (element is Control control && !control.IsTabStop)
            return false;
         return KeyboardNavigation.GetIsTabStop(element);
      }

      private static bool IsWithin_(DependencyObject element, DependencyObject root)
      {
         for (DependencyObject d = element; d != null; d = d is Visual || d is System.Windows.Media.Media3D.Visual3D ? VisualTreeHelper.GetParent(d) : LogicalTreeHelper.GetParent(d))
         {
            if (ReferenceEquals(d, root))
               return true;
         }

         return false;
      }

      /// <summary>The list of keys, from the palette's Keyboard shortcuts row.</summary>
      private void ShowKeyboardShortcuts_()
      {
         new KeyboardShortcutsDialog(this).ShowDialog();
      }
   }
}
