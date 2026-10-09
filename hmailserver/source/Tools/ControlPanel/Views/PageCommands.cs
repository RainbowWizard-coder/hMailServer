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
using System.Windows.Threading;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Views
{
   /// <summary>What a page's button is to the window's keys.</summary>
   public enum PageCommand
   {
      None,

      /// <summary>F5: the page's Refresh, or a settings page's Reload.</summary>
      Refresh,

      /// <summary>Ctrl+S: the page's Save changes.</summary>
      Save
   }

   /// <summary>
   /// What the window's F5, Ctrl+S and Ctrl+F reach on the page on screen. Each
   /// presses the page's own button as a click presses it - the keyboard moving
   /// to the button first, so that what was typed in a box is taken in - so
   /// that a key does what the button does: Reload asks before it throws edits
   /// away, Save writes what is on screen and checks what it checks, and a button
   /// that cannot be pressed now - a read that failed, nothing to save - cannot
   /// be pressed by its key either.
   ///
   /// A page's buttons are found by their captions: Refresh on a list page and
   /// Reload on a settings page (the owner's decision of 9 October 2026, one fixed
   /// access key each), and Save changes. Those are the captions of every such
   /// button on every page, which PageCommandsTests holds; a button whose caption
   /// says more - the scripts page's "Reload from disk" - carries
   /// <see cref="CommandProperty"/> instead. The button in the page's header is
   /// preferred, then the first that can be seen and pressed, so a tab's own
   /// buttons are found only while its tab is the one showing.
   /// </summary>
   public static class PageCommands
   {
      /// <summary>Marks a button whose caption is not one of the page commands' as one of them.</summary>
      public static readonly DependencyProperty CommandProperty = DependencyProperty.RegisterAttached(
         "Command", typeof(PageCommand), typeof(PageCommands), new PropertyMetadata(PageCommand.None)); // no-loc: a property's name

      public static PageCommand GetCommand(DependencyObject element) => (PageCommand)element.GetValue(CommandProperty);

      public static void SetCommand(DependencyObject element, PageCommand value) => element.SetValue(CommandProperty, value);

      /// <summary>The captions that are a page's Refresh, in English: the keys of the catalogue.</summary>
      public static readonly IReadOnlyList<string> RefreshCaptions = new[] { N("_Refresh"), N("_Reload") };

      /// <summary>The caption that is a page's Save, in English.</summary>
      public static readonly IReadOnlyList<string> SaveCaptions = new[] { N("_Save changes") };

      /// <summary>
      /// Presses the page's button for <paramref name="command"/> as a click
      /// presses it, and says whether there was one to press.
      ///
      /// A click first moves the keyboard to the button, and so does this: the box
      /// that held the keyboard loses it, which is when a NumberBox reads what was
      /// typed into its Value and a binding updated on LostFocus writes, and which
      /// marks the page unsaved. Pressed without it - through the button's
      /// automation peer, which leaves the keyboard where it is - Ctrl+S saved the
      /// value the box held before the typing while the page said Saved over the
      /// typed one, and F5 saw no unsaved change and threw the typing away without
      /// asking. The button's handlers run before this returns (GridKeys.Press),
      /// and then the keyboard goes back to where it was, since a key, unlike a
      /// click, should not move it - unless the button put it somewhere itself.
      /// Where the box is gone - Reload builds the page's rows again - it stays on
      /// the button, where a click leaves it.
      /// </summary>
      public static bool Press(DependencyObject page, PageCommand command)
      {
         ButtonBase button = Find(page, command);
         if (button == null)
            return false;

         IInputElement had = Keyboard.FocusedElement;
         bool moved = button.Focusable && !ReferenceEquals(had, button) && ReferenceEquals(Keyboard.Focus(button), button);

         if (!GridKeys.Press(button))
         {
            // Not pressed - what the box took in left the button disabled - and
            // the keyboard goes back to the box rather than stay on a button that
            // cannot hold it.
            if (moved && Holds_(had, except: null))
               Keyboard.Focus(had);
            return false;
         }

         if (moved && !Holds_(Keyboard.FocusedElement, except: button))
         {
            if (Holds_(had, except: null))
            {
               Keyboard.Focus(had);
               KeepTheKeyboard_(had, button);
            }
            else if (Holds_(button, except: null))
            {
               Keyboard.Focus(button);
            }
         }

         return true;
      }

      /// <summary>
      /// Once the page has drawn what it read, puts the keyboard back on a list's
      /// selected row where <paramref name="had"/>, a row of that list, went with
      /// the rows the list read again - so that F5 in a list leaves the keyboard
      /// in it - or else on <paramref name="fallback"/>, where nothing holds it.
      /// </summary>
      private static void KeepTheKeyboard_(IInputElement had, UIElement fallback)
      {
         Selector list = null;
         for (DependencyObject d = had as DependencyObject; d != null && list == null; d = d is Visual ? VisualTreeHelper.GetParent(d) : LogicalTreeHelper.GetParent(d))
         {
            if (d is DataGrid || d is ListBox)
               list = (Selector)d;
         }

         if (list == null && fallback == null)
            return;

         Dispatcher.CurrentDispatcher.BeginInvoke(DispatcherPriority.Loaded, new Action(() =>
         {
            if (Holds_(Keyboard.FocusedElement, except: null))
               return;
            if (list != null && list.IsVisible)
               GridKeys.FocusSelectedRow(list);
            else if (Holds_(fallback, except: null))
               Keyboard.Focus(fallback);
         }));
      }

      /// <summary>Whether <paramref name="element"/> is a control on screen that can hold the keyboard, and not <paramref name="except"/>.</summary>
      private static bool Holds_(IInputElement element, IInputElement except)
         => element is UIElement ui && !ReferenceEquals(element, except) && element is not Window
            && ui.IsVisible && ui.IsEnabled && ui.Focusable && PresentationSource.FromDependencyObject(ui) != null;

      /// <summary>The page's button for <paramref name="command"/> that can be seen and pressed now, or null.</summary>
      public static ButtonBase Find(DependencyObject page, PageCommand command)
      {
         if (page == null || command == PageCommand.None)
            return null;

         IReadOnlyList<string> captions = command == PageCommand.Refresh ? RefreshCaptions : SaveCaptions;
         var wanted = new HashSet<string>(captions.Select(L), StringComparer.Ordinal);

         List<ButtonBase> found = Descendants_(page).OfType<ButtonBase>()
            .Where(b => b.IsVisible && b.IsEnabled
               && (GetCommand(b) == command || (GetCommand(b) == PageCommand.None && b.Content is string text && wanted.Contains(text))))
            .ToList();

         return found.FirstOrDefault(b => Ancestors_(b).Any(a => a is Scaffold.PageHeader)) ?? found.FirstOrDefault();
      }

      /// <summary>
      /// Puts the keyboard in the page's search box: the one nearest the element
      /// that has the keyboard now - on the domains page, the accounts' search
      /// while the keyboard is in the accounts - or else the first on the page.
      /// False when the page has no search box that can be seen.
      /// </summary>
      public static bool FocusSearch(DependencyObject page, DependencyObject focused)
      {
         if (page == null)
            return false;

         Scaffold.Toolbar toolbar = null;
         if (focused != null && Ancestors_(focused).Contains(page))
         {
            foreach (DependencyObject container in Ancestors_(focused))
            {
               toolbar = Searchable_(container).FirstOrDefault();
               if (toolbar != null || ReferenceEquals(container, page))
                  break;
            }
         }

         toolbar ??= Searchable_(page).FirstOrDefault();
         if (toolbar == null)
            return false;

         toolbar.FocusSearch();
         return true;
      }

      private static IEnumerable<Scaffold.Toolbar> Searchable_(DependencyObject root)
         => (root is Scaffold.Toolbar self ? new[] { self } : Enumerable.Empty<Scaffold.Toolbar>())
            .Concat(Descendants_(root).OfType<Scaffold.Toolbar>())
            .Where(t => t.ShowSearch && t.IsVisible && t.IsEnabled);

      /// <summary>Every element under <paramref name="root"/> in the visual tree, depth first, in tab order for everything here.</summary>
      internal static IEnumerable<DependencyObject> Descendants_(DependencyObject root)
      {
         if (root is not Visual && root is not System.Windows.Media.Media3D.Visual3D)
            yield break;

         int count = VisualTreeHelper.GetChildrenCount(root);
         for (int i = 0; i < count; i++)
         {
            DependencyObject child = VisualTreeHelper.GetChild(root, i);
            yield return child;
            foreach (DependencyObject below in Descendants_(child))
               yield return below;
         }
      }

      private static IEnumerable<DependencyObject> Ancestors_(DependencyObject element)
      {
         for (DependencyObject d = element; d != null; d = d is Visual || d is System.Windows.Media.Media3D.Visual3D ? VisualTreeHelper.GetParent(d) : LogicalTreeHelper.GetParent(d))
            yield return d;
      }
   }
}
