// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// What the generated Add and Edit dialog (Views/FieldDialog, the editor behind
   /// every collection page) decides without WPF: the order it lays its fields out
   /// in, and the words for a required field left empty.
   ///
   /// Add SURBL server and Add blacklist opened on an unticked Active box, listed
   /// first, and on an empty Expected result with nothing to say what goes in it,
   /// so a list added and saved was never asked, or never a match (review finding
   /// F174). The tick boxes that lead a spec - Active, Enabled - go after the
   /// fields that say what the entry is now, on the short specs that ask for it
   /// (CollectionSpec.TickBoxesLast: an external account's Enabled is the master
   /// switch of a dialog that scrolls, and stays first), and a field the entry
   /// is useless without is refused empty, on the field - every such field on
   /// one press, not one a press. Held by BlockListDialogTests.
   /// </summary>
   public static class FieldDialogRules
   {
      /// <summary>
      /// Whether every committer accepts its field. Every one runs, whatever
      /// the ones before it said, so that a dialog with two required fields
      /// left empty marks both on one press of Add rather than the second only
      /// after the first is filled; the dialog then puts the keyboard in the
      /// first field refused.
      /// </summary>
      public static bool AllAccept(IEnumerable<Func<bool>> committers)
      {
         bool all = true;
         foreach (Func<bool> commit in committers)
         {
            if (!commit())
               all = false;
         }

         return all;
      }

      /// <summary>
      /// The fields in the order the dialog lays them out: the run of tick boxes
      /// that leads the spec moved after the rest, each run in its own order. A
      /// box that follows another kind of field keeps its place, since its caption
      /// may refer to its neighbour ("needed for the option above"). The grid keeps
      /// the spec's order, where a leading Active column is where it belongs.
      /// </summary>
      public static List<T> TickBoxesAfterTheRest<T>(IReadOnlyList<T> fields, Func<T, bool> isTickBox)
      {
         int lead = 0;
         while (lead < fields.Count && isTickBox(fields[lead]))
            lead++;

         var ordered = new List<T>(fields.Count);
         for (int i = lead; i < fields.Count; i++)
            ordered.Add(fields[i]);
         for (int i = 0; i < lead; i++)
            ordered.Add(fields[i]);
         return ordered;
      }

      /// <summary>The message on a required field left empty, naming the field by its caption.</summary>
      public static string RequiredMessage(string label)
         => F("{0} is required.", WithoutAltKey(label ?? ""));

      /// <summary>
      /// The caption without its Alt-key mark: the first single underscore goes,
      /// and a doubled one is the underscore it stands for (MnemonicText's rule,
      /// which is not compiled into ControlPanel.Core).
      /// </summary>
      private static string WithoutAltKey(string caption)
      {
         var text = new System.Text.StringBuilder(caption.Length);
         bool marked = false;
         for (int i = 0; i < caption.Length; i++)
         {
            if (caption[i] != '_')
            {
               text.Append(caption[i]);
               continue;
            }

            if (i + 1 < caption.Length && caption[i + 1] == '_')
            {
               text.Append('_');
               i++;
            }
            else if (!marked)
               marked = true;
            else
               text.Append('_');
         }

         return text.ToString();
      }
   }
}
