// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Linq;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Input;

namespace hMailServer.ControlPanel.Views
{
   /// <summary>
   /// Enter submits a form on a page, as it does in a dialog, and Escape empties
   /// what was typed into it.
   ///
   /// A dialog's primary button takes Enter (FluentDialogWindow.UseFrame), but
   /// the add forms laid out on the pages themselves - a route, a port, a rule,
   /// a relay, a certificate, an IP range, the domains page's four - and the
   /// boxes of the MX query and the diagnostics did nothing with it (review
   /// finding F211): a keyboard user typed the last field and then tabbed to the
   /// button. Enter in any box of the form now presses its button, as a click
   /// does, and does nothing while the button is disabled; a box that takes
   /// Enter itself - one of several lines, a list that is open - keeps it.
   /// Escape empties the boxes the page names, and only when there is something
   /// in them, so that an empty form leaves Escape to whatever else wants it; a
   /// box that holds a default, such as a port's 25, is not named.
   /// </summary>
   public static class FormSubmit
   {
      public static void Bind(UIElement form, ButtonBase submit, params Control[] clearOnEscape)
      {
         if (form == null || submit == null)
            return;

         form.PreviewKeyDown += (_, e) =>
         {
            if (e.Handled || Keyboard.Modifiers != ModifierKeys.None)
               return;

            if (e.Key == Key.Enter && TakesEnter_(e.OriginalSource as DependencyObject))
            {
               if (GridKeys.Press(submit))
                  e.Handled = true;
            }
            else if (e.Key == Key.Escape && clearOnEscape.Length > 0 && IsField_(e.OriginalSource as DependencyObject)
               && clearOnEscape.Any(c => Text_(c).Length > 0))
            {
               foreach (Control box in clearOnEscape)
               {
                  if (box is Wpf.Ui.Controls.PasswordBox masked)
                     masked.Password = "";
                  else if (box is PasswordBox password)
                     password.Clear();
                  else if (box is TextBox text)
                     text.Clear();
               }

               e.Handled = true;
            }
         };
      }

      /// <summary>Whether Enter in <paramref name="source"/> is the form's: a box of one line, a closed list, a tick box.</summary>
      private static bool TakesEnter_(DependencyObject source)
      {
         switch (FieldOf_(source))
         {
            case TextBox text:
               return !text.AcceptsReturn;
            case PasswordBox:
               return true;
            case ComboBox combo:
               return !combo.IsDropDownOpen;
            case CheckBox:
               return true;
            default:
               return false;
         }
      }

      private static bool IsField_(DependencyObject source) => FieldOf_(source) != null;

      /// <summary>The form's field an event came from - the box, not the text box inside a combo box's template - or null.</summary>
      private static Control FieldOf_(DependencyObject source)
      {
         Control found = null;
         for (DependencyObject d = source; d != null; d = d is System.Windows.Media.Visual ? System.Windows.Media.VisualTreeHelper.GetParent(d) : LogicalTreeHelper.GetParent(d))
         {
            if (d is ComboBox || d is PasswordBox || d is CheckBox)
               return (Control)d;
            if (d is TextBox t && found == null)
               found = t;
         }

         return found;
      }

      private static string Text_(Control box) => box switch
      {
         Wpf.Ui.Controls.PasswordBox masked => masked.Password ?? "",
         PasswordBox password => password.Password,
         TextBox text => text.Text,
         _ => ""
      };
   }
}
