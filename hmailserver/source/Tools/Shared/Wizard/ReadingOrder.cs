// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Linq;
using System.Windows.Forms;

namespace hMailServer.Shared
{
   /// <summary>
   ///    Numbers the controls of a wizard page in the order a reader takes them in: line by
   ///    line from the top, and from left to right along a line. Tab and Shift+Tab then move
   ///    the way the page reads, and a label comes just before the field beside it, which is
   ///    what lets Windows Forms name the field after the label for a screen reader (the field
   ///    whose TabIndex follows a Label's takes the label's text as its accessible name).
   /// </summary>
   /// <remarks>
   ///    Controls of one line: they are taken by Top, then by Left, and a control is on the
   ///    line of the first control not yet numbered when their Tops differ by less than
   ///    half the height of the shorter of the two. So a label set a few pixels lower than
   ///    the text box it names, to line up with the box's text, is on the box's line and
   ///    before it, rather than after it because its Top is larger; and a tall control, a
   ///    list or a panel, shares a line only with the row that starts at its own Top, so
   ///    that the rows below that one beside it are lines of their own and each label of
   ///    them still comes just before its field. Within a line, controls go by Left;
   ///    controls with the same position keep the order of their parent's Controls
   ///    collection. TabIndex numbers each parent's children from 0 (TabIndex is relative to
   ///    the parent), and every child that has children of its own, a GroupBox or a Panel,
   ///    is numbered inside in the same way. Every control is numbered, a Label or a hidden
   ///    control too: TabStop, Enabled and Visible decide whether focus stops there, and
   ///    are left as they are.
   /// </remarks>
   public static class ReadingOrder
   {
      public static void ApplyTo(Control parent)
      {
         List<Control> children = parent.Controls.Cast<Control>().ToList();

         // OrderBy and ThenBy are stable, so the Controls collection's order breaks ties.
         List<Control> unnumbered = children.OrderBy(control => control.Top)
                                            .ThenBy(control => control.Left)
                                            .ToList();

         int next = 0;
         while (unnumbered.Count > 0)
         {
            Control first = unnumbered[0];
            List<Control> line = unnumbered.Where(control => control == first || OnOneLine(first, control))
                                           .OrderBy(control => control.Left)
                                           .ToList();

            foreach (Control control in line)
            {
               control.TabIndex = next++;
               unnumbered.Remove(control);
            }
         }

         foreach (Control child in children)
         {
            if (child.HasChildren)
               ApplyTo(child);
         }
      }

      // Tops less than half the shorter height apart; doubled, so that nothing is rounded.
      private static bool OnOneLine(Control one, Control other)
      {
         return Math.Abs(one.Top - other.Top) * 2 < Math.Min(one.Height, other.Height);
      }
   }
}
