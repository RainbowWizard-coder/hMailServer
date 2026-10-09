// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Linq;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// What a screen reader says for a row of a list. A DataGrid or a ListBox
   /// announces a row by its item's ToString, and an item that does not override
   /// it is announced by its class name - "hMailServer.ControlPanel.Views.
   /// AuditTrailView+RowView" - which is what several pages' rows said (review
   /// findings F070, F285). A row class overrides ToString with one of these: its
   /// cells that have something in them, joined with commas, so a reader hears one
   /// statement rather than fragments; a cell whose value says nothing without its
   /// column - Yes, 3, 127.0.0.2 - goes with its column's name, "Active: Yes".
   ///
   /// Plain functions, without WPF, so that the tests hold the sentences.
   /// </summary>
   public static class RowSpeech
   {
      /// <summary>The cells that have something in them, in order, joined with commas.</summary>
      public static string Of(params string[] cells)
         => cells == null ? "" : string.Join(", ", cells.Where(c => !string.IsNullOrWhiteSpace(c)).Select(c => c.Trim()));

      /// <summary>A cell with its column's name - "Active: Yes" - or nothing when the cell is empty.</summary>
      public static string Labelled(string column, string value)
         => string.IsNullOrWhiteSpace(value) ? "" : string.IsNullOrWhiteSpace(column) ? value.Trim() : F("{0}: {1}", column.Trim(), value.Trim());
   }
}
