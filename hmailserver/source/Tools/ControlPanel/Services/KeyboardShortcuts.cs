// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>What a key the window answers on every page does.</summary>
   public enum ShellCommand
   {
      None,

      /// <summary>Ctrl+K: the search palette.</summary>
      Palette,

      /// <summary>F1: the help pane, open or closed.</summary>
      Help,

      /// <summary>F5: the page's Refresh, or Reload on a settings page, which asks first when edits would be lost.</summary>
      Refresh,

      /// <summary>Ctrl+F: the page's search box, or the palette on a page without one.</summary>
      Search,

      /// <summary>Ctrl+S: the page's Save changes.</summary>
      Save,

      /// <summary>F6: the next part of the window.</summary>
      NextPart,

      /// <summary>Shift+F6: the part before.</summary>
      PreviousPart
   }

   /// <summary>
   /// The parts of the window F6 and Shift+F6 move between, in the order they are
   /// read: the command bar across the top, the navigation down the left, the
   /// page, the help pane on the right and the pending bar across the foot.
   /// </summary>
   public enum ShellPart
   {
      CommandBar,
      Navigation,
      Page,
      Help,
      PendingBar
   }

   /// <summary>One line of the keyboard shortcuts list: the keys, and what they do.</summary>
   public sealed class KeyboardShortcut
   {
      public KeyboardShortcut(string keys, string does)
      {
         Keys = keys;
         Does = does;
      }

      /// <summary>The keys as they are printed on a keyboard in the reader's language - Strg+K in German. Marked with N(); translated where it is shown.</summary>
      public string Keys { get; }

      /// <summary>What they do. Marked with N(); translated where it is shown.</summary>
      public string Does { get; }
   }

   /// <summary>
   /// The keys the Control Panel's window answers on every page, apart from the
   /// window that answers them, so that the table, the order F6 moves in and the
   /// list the palette shows can be tested without one.
   ///
   /// Only Ctrl+K and F1 existed until October 2026: F5, Ctrl+F, Ctrl+S and F6
   /// did nothing on any page, Toolbar.FocusSearch was never called, and the help
   /// pane closed only by its button or a second F1 (review finding F014). What
   /// each key reaches on a page - its Refresh or Reload, its search box, its
   /// Save changes - is Views/PageCommands; MainWindow.Keys.cs is the window half.
   /// </summary>
   public static class KeyboardShortcuts
   {
      /// <summary>
      /// The command for a key, by the key's name as WPF's Key enumeration spells
      /// it ("K", "F5"). Alt is never ours: it is the access keys'. A key with a
      /// modifier it does not take is no command, so that Ctrl+F5 or Shift+Ctrl+S
      /// fall through to whatever else answers them.
      /// </summary>
      public static ShellCommand Resolve(string key, bool control, bool shift, bool alt)
      {
         if (alt || string.IsNullOrEmpty(key))
            return ShellCommand.None;

         switch (key)
         {
            case "K":
               return control && !shift ? ShellCommand.Palette : ShellCommand.None;
            case "F":
               return control && !shift ? ShellCommand.Search : ShellCommand.None;
            case "S":
               return control && !shift ? ShellCommand.Save : ShellCommand.None;
            case "F1":
               return !control && !shift ? ShellCommand.Help : ShellCommand.None;
            case "F5":
               return !control && !shift ? ShellCommand.Refresh : ShellCommand.None;
            case "F6":
               if (control)
                  return ShellCommand.None;
               return shift ? ShellCommand.PreviousPart : ShellCommand.NextPart;
            default:
               return ShellCommand.None;
         }
      }

      /// <summary>
      /// The part F6 (or Shift+F6, <paramref name="backwards"/>) moves to from
      /// <paramref name="current"/>, among the parts that are on screen and can
      /// take the keyboard, in <see cref="ShellPart"/> order and round again at
      /// the end. From nowhere - nothing focused, or the focus somewhere that is
      /// none of them, such as a tour's card - F6 goes to the first and Shift+F6
      /// to the last. Null when no part can take the keyboard.
      /// </summary>
      public static ShellPart? NextPart(IReadOnlyCollection<ShellPart> open, ShellPart? current, bool backwards)
      {
         if (open == null || open.Count == 0)
            return null;

         var order = new List<ShellPart>();
         foreach (ShellPart part in Enum.GetValues<ShellPart>())
         {
            if (Contains_(open, part))
               order.Add(part);
         }

         int at = current.HasValue ? order.IndexOf(current.Value) : -1;
         if (at < 0)
            return backwards ? order[^1] : order[0];

         int next = (at + (backwards ? order.Count - 1 : 1)) % order.Count;
         return order[next];
      }

      private static bool Contains_(IReadOnlyCollection<ShellPart> open, ShellPart part)
      {
         foreach (ShellPart p in open)
         {
            if (p == part)
               return true;
         }

         return false;
      }

      /// <summary>
      /// What the keyboard shortcuts list says, in the order it says it: the keys
      /// of the window first, then the keys of a list, then the access keys. The
      /// keys and the words are both marked for translation, since a German
      /// keyboard prints Strg and Entf where an English one prints Ctrl and Del.
      /// </summary>
      public static IReadOnlyList<KeyboardShortcut> Listed { get; } = new[]
      {
         new KeyboardShortcut(N("Ctrl+K"), N("Search pages, settings and tasks")),
         new KeyboardShortcut("F1", N("Open or close the help for this page")), // no-loc: a key's name
         new KeyboardShortcut("F5", N("Refresh a list, or reload a settings page")), // no-loc: a key's name
         new KeyboardShortcut(N("Ctrl+F"), N("Go to the search box of this page")),
         new KeyboardShortcut(N("Ctrl+S"), N("Save the changes on this page")),
         new KeyboardShortcut(N("F6, Shift+F6"), N("Move to the next or the previous part of the window: the command bar, the navigation, the page and the help")),
         new KeyboardShortcut(N("Esc"), N("Close the help while the keyboard is in it, close a dialog, and empty a form on a page")),
         new KeyboardShortcut(N("Enter, F2"), N("Open the selected row of a list")),
         new KeyboardShortcut(N("Del"), N("Delete the selected row of a list, after asking")),
         new KeyboardShortcut(N("Ins"), N("Add a row to a list, or go to the form that adds one")),
         new KeyboardShortcut(N("Enter"), N("Press the button of the form the keyboard is in, on a page as in a dialog")),
         new KeyboardShortcut(N("Alt+underlined letter"), N("Press the button, or go to the field, whose caption has that letter underlined"))
      };
   }
}
