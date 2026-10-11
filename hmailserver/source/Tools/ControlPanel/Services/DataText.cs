// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Globalization;
using System.Text;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// Data shown inside a sentence - a cipher list, a path, a header - in the
   /// monospaced face, the sentence in the UI face (review findings F135, F304). The
   /// transport encryption overview's "Cipher list for TLS 1.2 and below:
   /// ECDHE-ECDSA-AES128-GCM-SHA256:..." was caption prose that broke inside a suite
   /// name ("ECDHE-RSA-AES256-GCM-" / "SHA384"); the list is now a run of its own
   /// that breaks only after a colon. A run inside a sentence takes the sentence's
   /// size; a block or a box of such text takes Typography.Mono. Plain functions,
   /// without WPF, so that DataTextTests holds them; DataTextBlock sets the runs.
   /// </summary>
   public static class DataText
   {
      /// <summary>
      /// A colon-separated list - OpenSSL's cipher and group lists - with a place to
      /// break after each colon (a zero-width space) and none at its hyphens (each
      /// followed by a word joiner, U+2060), so that it wraps between its items and
      /// never inside one, whose names are joined by hyphens: a hyphen is a place to
      /// break, and with the colons alone the overview still broke "ECDHE-RSA-" /
      /// "AES256-GCM-". The hyphen stays the ASCII one, which both monospaced faces
      /// draw: the non-breaking hyphen (U+2011) this wrote first is not in Consolas,
      /// the face a machine without Cascadia Mono shows, which took it from the UI
      /// face, a narrower hyphen in a monospaced run. Neither face has the joiner,
      /// nor Cascadia Mono the zero-width space, and WPF draws neither: rendered on
      /// server1 in each face, the list breaks at its colons alone, with no box.
      /// </summary>
      public static string BreakAfterColons(string list)
         => string.IsNullOrEmpty(list)
            ? list ?? ""
            : list.Replace("-", "-\u2060", StringComparison.Ordinal).Replace(":", ":\u200B", StringComparison.Ordinal);

      /// <summary>
      /// A long path cut in its middle - its root and first folder, an ellipsis, its
      /// last two parts - so that a line shows where it starts and what it names,
      /// where a wrapped path broke at a backslash; the caller puts the whole path in
      /// the tool tip. A path of <paramref name="most"/> characters or fewer, or of
      /// four parts or fewer, is given back whole.
      /// </summary>
      public static string MiddleOfPath(string path, int most = 64)
      {
         if (string.IsNullOrEmpty(path) || path.Length <= most)
            return path ?? "";

         char separator = path.Contains('\\', StringComparison.Ordinal) ? '\\' : '/';
         string[] parts = path.Split(separator);
         if (parts.Length <= 4)
            return path;

         return string.Join(separator, parts[0], parts[1], "\u2026", parts[^2], parts[^1]);
      }

      /// <summary>
      /// A translated sentence whose placeholders are data - "{0}, then silence",
      /// "{0}, {1} or {2} slow" - cut into its pieces in order: each stretch of its
      /// words, and each {n} as the n-th of <paramref name="data"/>, marked as data, so
      /// that the data alone is set in the monospaced face and the words around it in
      /// the sentence's (review finding F304: the stalled-mail guide set whole cells in
      /// the monospaced face, and "with a large", "slow" and "then silence" read as text
      /// the log writes). A doubled brace is a brace, as string.Format reads it; a
      /// placeholder with no data stays as it is written.
      /// </summary>
      public static IReadOnlyList<(string Text, bool IsData)> Pieces(string template, IReadOnlyList<string> data)
      {
         var pieces = new List<(string Text, bool IsData)>();
         var words = new StringBuilder();
         template ??= "";
         for (int i = 0; i < template.Length; i++)
         {
            char c = template[i];
            if ((c == '{' || c == '}') && i + 1 < template.Length && template[i + 1] == c)
            {
               words.Append(c);
               i++;
               continue;
            }

            int close = c == '{' ? template.IndexOf('}', i + 1) : -1;
            if (close > i + 1 && data != null
                && int.TryParse(template.AsSpan(i + 1, close - i - 1), NumberStyles.None, CultureInfo.InvariantCulture, out int index)
                && index < data.Count)
            {
               if (words.Length > 0)
                  pieces.Add((words.ToString(), false));
               words.Clear();
               pieces.Add((data[index] ?? "", true));
               i = close;
               continue;
            }

            words.Append(c);
         }

         if (words.Length > 0)
            pieces.Add((words.ToString(), false));
         return pieces;
      }
   }
}
