// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// The choices a settings list offers, and what it does with a value that is
   /// none of them.
   ///
   /// A list that cannot show the value the server holds used to show its first
   /// choice instead - and the settings pages save every row, so the first save
   /// of the page wrote that choice over the value, with nothing on screen to say
   /// so. A value written by hand, or by a newer server that has a choice this
   /// Control Panel was built without, was lost to a save of an unrelated field.
   /// Now such a value is offered as itself, after the choices, and is what the
   /// list holds until somebody picks another: saved unchanged, it writes back
   /// what it read.
   /// </summary>
   public static class ChoiceOptions
   {
      /// <summary>
      /// The choices, with one more after them when <paramref name="current"/> is
      /// none of them: the value itself, said as <paramref name="shownAs"/> when
      /// given (a word the server answers) and as its number otherwise.
      ///
      /// <paramref name="sameAs"/> names the stored values the server reads as one
      /// of the choices - each key read as the choice its value names. When
      /// <paramref name="current"/> is such a value, it is not unknown: that
      /// choice is offered with its own words and carries <paramref name="current"/>,
      /// so the list shows what the server does with the value, and a save that
      /// leaves the list alone writes back the value it read rather than the
      /// choice's own number.
      /// </summary>
      public static IReadOnlyList<(int Value, string Label)> WithUnknown(
         IReadOnlyList<(int Value, string Label)> options, int current, string shownAs = null,
         IReadOnlyDictionary<int, int> sameAs = null)
      {
         var result = new List<(int Value, string Label)>(options ?? new (int, string)[0]);
         if (result.Any(option => option.Value == current))
            return result;

         if (sameAs != null && sameAs.TryGetValue(current, out int readAs))
         {
            int index = result.FindIndex(option => option.Value == readAs);
            if (index >= 0)
            {
               result[index] = (current, result[index].Label);
               return result;
            }
         }

         result.Add((current, UnknownLabel(shownAs ?? current.ToString(CultureInfo.InvariantCulture))));
         return result;
      }

      /// <summary>How a value that is none of the choices is named in the list.</summary>
      public static string UnknownLabel(string value)
         => F("{0} (a value this Control Panel does not know)", value ?? "");
   }
}
