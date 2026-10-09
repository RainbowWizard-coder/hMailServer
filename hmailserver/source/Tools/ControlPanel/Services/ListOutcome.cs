// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Globalization;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// What a list page says after an Add, an Edit or a Delete, and which row it
   /// keeps in view (review finding F208).
   ///
   /// The generic list editor set "Added at…", "Saved at…" or "Deleted." and then
   /// read the list again, and the read hid the notice on success, so none of the
   /// three was ever seen; the read rebuilt the rows, so the edited row lost its
   /// selection and the list jumped to the top, and a new entry was neither
   /// selected nor scrolled to. The page now reads the list first and says the
   /// outcome after it, naming the entry - "multi.surbl.org added." - and selects
   /// the entry it acted on, or after a delete the row that took its place.
   ///
   /// No WPF here, so the tests hold the words and the arithmetic.
   /// </summary>
   public static class ListOutcome
   {
      /// <summary>
      /// The words an entry is known by in a sentence: the first of its shown
      /// values that is not empty, trimmed and on one line; null when it has none.
      /// </summary>
      public static string EntryName(IEnumerable<string> shownValues)
      {
         if (shownValues == null)
            return null;

         foreach (string value in shownValues)
         {
            string line = string.Join(" ", (value ?? "").Split((char[])null, StringSplitOptions.RemoveEmptyEntries));
            if (line.Length > 0)
               return line.Length > 80 ? line.Substring(0, 79) + "…" : line;
         }

         return null;
      }

      /// <summary>"multi.surbl.org added.", or the time it was added when the entry has no name to say.</summary>
      public static string Added(string name, DateTime at)
         => string.IsNullOrEmpty(name) ? F("Added at {0}.", at.ToString("T", CultureInfo.CurrentCulture)) : F("{0} added.", name);

      /// <summary>"multi.surbl.org saved.", or the time it was saved when the entry has no name to say.</summary>
      public static string Saved(string name, DateTime at)
         => string.IsNullOrEmpty(name) ? F("Saved at {0}.", at.ToString("T", CultureInfo.CurrentCulture)) : F("{0} saved.", name);

      /// <summary>"multi.surbl.org deleted.", or "Deleted." when the entry has no name to say.</summary>
      public static string Deleted(string name)
         => string.IsNullOrEmpty(name) ? L("Deleted.") : F("{0} deleted.", name);

      /// <summary>
      /// The row to select after the row at <paramref name="deletedIndex"/> was
      /// deleted and the list read again with <paramref name="countAfter"/> rows:
      /// the row that took its place, or the last row when it was the last; -1
      /// when the list is empty, or when nothing was selected.
      /// </summary>
      public static int IndexAfterDelete(int countAfter, int deletedIndex)
      {
         if (countAfter <= 0 || deletedIndex < 0)
            return -1;

         return Math.Min(deletedIndex, countAfter - 1);
      }
   }
}
