// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;

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
      /// break after each colon (a zero-width space) and none at its hyphens (each a
      /// non-breaking hyphen), so that it wraps between its items and never inside
      /// one, whose names are joined by hyphens: a hyphen is a place to break, and
      /// with the colons alone the overview still broke "ECDHE-RSA-" / "AES256-GCM-".
      /// </summary>
      public static string BreakAfterColons(string list)
         => string.IsNullOrEmpty(list)
            ? list ?? ""
            : list.Replace("-", "\u2011", StringComparison.Ordinal).Replace(":", ":\u200B", StringComparison.Ordinal);

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
   }
}
