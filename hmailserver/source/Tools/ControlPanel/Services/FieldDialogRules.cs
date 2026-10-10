// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Text.RegularExpressions;
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
   ///
   /// And the two things it decides for a dialog that stays open on a refused
   /// save (review finding F209): the Alt keys of fields whose captions carry
   /// none, and the field a refusal of the server names. Held by
   /// FieldDialogSaveTests.
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

      /// <summary>
      /// The captions with an Alt key each, in the dialog's order. The specs'
      /// captions carry none, and one spec's dialog has any number of fields, so
      /// the dialog assigns them: a caption that has a key keeps it; otherwise the
      /// first letter of a word not yet taken, then any letter not yet taken,
      /// takes the underscore. Where <paramref name="markAfter"/> - the language
      /// marks a key after the caption, "名前(_N)", as Japanese and Chinese do - or
      /// where no letter of the caption is free, "(_X)" goes after it, X the first
      /// free letter of the field's English property name, then of the alphabet.
      /// A key taken by <paramref name="taken"/> (the dialog's buttons) or by an
      /// earlier field is not given again; a caption that finds none is left as
      /// it is.
      /// </summary>
      public static List<string> WithAccessKeys(IReadOnlyList<string> captions, IReadOnlyList<string> props,
                                                IEnumerable<char> taken, bool markAfter)
      {
         var used = new HashSet<char>();
         foreach (char key in taken ?? Array.Empty<char>())
            used.Add(char.ToUpperInvariant(key));

         // The captions that have a key keep it, and it is taken for the others.
         var keyed = new List<string>(captions.Count);
         for (int i = 0; i < captions.Count; i++)
         {
            char existing = KeyOf(captions[i] ?? "");
            if (existing != '\0')
               used.Add(char.ToUpperInvariant(existing));
         }

         for (int i = 0; i < captions.Count; i++)
         {
            string caption = captions[i] ?? "";
            if (caption.Length == 0 || KeyOf(caption) != '\0')
            {
               keyed.Add(caption);
               continue;
            }

            string text = caption.Replace("_", "__");
            int at = markAfter ? -1 : FreeLetter_(text, used, wordStart: true);
            if (!markAfter && at < 0)
               at = FreeLetter_(text, used, wordStart: false);

            if (at >= 0)
            {
               used.Add(char.ToUpperInvariant(text[at]));
               keyed.Add(text.Insert(at, "_"));
               continue;
            }

            string prop = i < props?.Count ? props[i] ?? "" : "";
            char letter = '\0';
            foreach (char c in prop.ToUpperInvariant() + "ABCDEFGHIJKLMNOPQRSTUVWXYZ") // no-loc: the letters a key can be
            {
               if (c >= 'A' && c <= 'Z' && !used.Contains(c))
               {
                  letter = c;
                  break;
               }
            }

            if (letter == '\0')
            {
               keyed.Add(caption);
               continue;
            }

            used.Add(letter);

            // Before a closing ellipsis or colon, as the catalogues put the mark.
            int end = text.Length;
            while (end > 0 && (text[end - 1] == '\u2026' || text[end - 1] == ':' || text[end - 1] == '\uFF1A'))
               end--;
            keyed.Add(text.Substring(0, end) + "(_" + letter + ")" + text.Substring(end));
         }

         return keyed;
      }

      /// <summary>The Alt keys the captions carry - the dialog's buttons, as the catalogue words them.</summary>
      public static List<char> KeysOf(IEnumerable<string> captions)
      {
         var keys = new List<char>();
         foreach (string caption in captions ?? Array.Empty<string>())
         {
            char key = KeyOf(caption ?? "");
            if (key != '\0')
               keys.Add(key);
         }

         return keys;
      }

      /// <summary>Whether a caption marks its key after itself, "追加(_A)", as the Japanese and Chinese catalogues do.</summary>
      public static bool MarksKeyAfter(string caption)
         => caption != null && Regex.IsMatch(caption, @"\(_[A-Za-z0-9]\)");

      /// <summary>
      /// Which field a refusal of the server names: the field whose caption,
      /// without its Alt key, or whose property name the refusal's words contain,
      /// the longest such caption where several do - or -1 where it names none,
      /// and the dialog says it on its first field.
      /// </summary>
      public static int FieldNamedIn(string refusal, IReadOnlyList<string> captions, IReadOnlyList<string> props)
      {
         if (string.IsNullOrEmpty(refusal))
            return -1;

         int best = -1;
         int bestLength = 0;
         for (int i = 0; i < captions.Count; i++)
         {
            string caption = WithoutAltKey(captions[i] ?? "");
            if (caption.Length > bestLength && refusal.IndexOf(caption, StringComparison.CurrentCultureIgnoreCase) >= 0)
            {
               best = i;
               bestLength = caption.Length;
            }

            string prop = i < props?.Count ? props[i] : null;
            if (best < 0 && !string.IsNullOrEmpty(prop)
                && Regex.IsMatch(refusal, @"\b" + Regex.Escape(prop) + @"\b", RegexOptions.IgnoreCase))
            {
               best = i;
               bestLength = 0;
            }
         }

         return best;
      }

      // The letter after the first single underscore, or NUL for a caption with no key.
      private static char KeyOf(string caption)
      {
         for (int i = 0; i + 1 < caption.Length; i++)
         {
            if (caption[i] != '_')
               continue;
            if (caption[i + 1] == '_')
            {
               i++;
               continue;
            }

            return char.IsWhiteSpace(caption[i + 1]) ? '\0' : caption[i + 1];
         }

         return '\0';
      }

      // The index of the first free letter a key can be - a letter of the Latin,
      // Greek or Cyrillic alphabets, which a keyboard types - at the start of a word
      // or anywhere, or -1; never one of a doubled underscore's escape.
      private static int FreeLetter_(string text, HashSet<char> used, bool wordStart)
      {
         for (int i = 0; i < text.Length; i++)
         {
            char c = text[i];
            if (c == '_')
            {
               i++;
               continue;
            }

            if (!char.IsLetter(c) || c >= '\u0530' || used.Contains(char.ToUpperInvariant(c)))
               continue;

            if (wordStart && i > 0 && char.IsLetter(text[i - 1]))
               continue;

            return i;
         }

         return -1;
      }

      /// <summary>The message on a required field left empty, naming the field by its caption.</summary>
      public static string RequiredMessage(string label)
         => F("{0} is required.", WithoutAltKey(label ?? ""));

      /// <summary>
      /// The text a box opens on for the value the server holds: a date in the server's
      /// own form, yyyy-MM-dd HH:mm:ss, which the allow and block list's caption names and
      /// the server reads back, and nothing for its "no date", before 1970; any other value
      /// as it was. The box opened on the reader's general form - "12/31/2026 12:00:00 AM",
      /// "31.12.2026 00:00:00" - or "12/30/1899 12:00:00 AM" for an entry that never
      /// expires, which the server refused on Save, so that an entry could not be edited
      /// without typing its date again.
      /// </summary>
      public static string BoxText(object current)
      {
         if (current is DateTime when)
            return when.Year < 1970 ? "" : when.ToString(ServerTimeText.ServerFormat, System.Globalization.CultureInfo.InvariantCulture);

         return Convert.ToString(current, System.Globalization.CultureInfo.CurrentCulture) ?? "";
      }

      /// <summary>
      /// Takes out of the values to send each value that applies only while a switch is on
      /// - the allow and block list's expiry date while "Stops mattering at a date" is
      /// ticked (FieldSpec.GridOnlyWhen) - where the dialog has the switch off, so that the
      /// server keeps what it holds. The empty date was sent, which the server refuses, and
      /// no entry that never expires could be added or saved.
      /// </summary>
      public static void DropWhereSwitchedOff(IDictionary<string, object> values, IEnumerable<(string Prop, string Switch)> gated)
      {
         foreach ((string prop, string onlyWhen) in gated)
         {
            if (values.TryGetValue(onlyWhen, out object on) && on is false)
               values.Remove(prop);
         }
      }

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
