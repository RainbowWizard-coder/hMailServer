// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

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
   ///    Controls of one line: they are ordered by Top, and a control is on the same line
   ///    as the first (topmost) control of the line when its vertical middle is above that
   ///    first control's Bottom. So a label set a few pixels lower than the text box it
   ///    names, to line up with the box's text, is on the box's line and before it, rather
   ///    than after it because its Top is larger. Within a line, controls go by Left;
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

         // OrderBy is stable, so the Controls collection's order breaks ties.
         List<Control> unnumbered = children.OrderBy(control => control.Top).ToList();

         int next = 0;
         while (unnumbered.Count > 0)
         {
            Control first = unnumbered[0];
            List<Control> line = unnumbered.Where(control => control == first || VerticalMiddle(control) < first.Bottom)
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

      private static int VerticalMiddle(Control control)
      {
         return control.Top + control.Height / 2;
      }
   }
}
