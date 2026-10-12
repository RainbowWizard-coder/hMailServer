// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Globalization;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// The one way the Control Panel shows a date and time (review finding F321; the
   /// owner's decision 13: the reader's culture format in grids, with the clock named).
   /// The delivery queue, Server status and the quarantine showed the server's raw
   /// "2026-10-08 02:36:53", the audit trail Unix times as this computer's local time in
   /// the same form, the REST API keys "7 January 2027 10:00", and nothing said whose
   /// clock a time was read by, which matters to a Control Panel on another computer.
   ///
   /// A time the server writes is its own local time, in yyyy-MM-dd HH:mm:ss
   /// (<see cref="Parse"/>); it is shown in the reader's short date and time
   /// (<see cref="Cell"/>, or <see cref="CellWithSeconds"/> where the second counts, as
   /// on the audit trail), with "(server time)" in the column's header or after the
   /// value (<see cref="Marked"/>), and whole in a tool tip (<see cref="Full"/>). Where
   /// the server's clock is this computer's - a session on this computer - a grid may
   /// say how long ago or how soon instead (<see cref="Relative"/>), the time in its
   /// tool tip. A Unix time is an instant: it is shown in this computer's local time,
   /// which in a session on this computer is the server's (<see cref="FromUnix"/>).
   ///
   /// No WPF, so that ServerTimeTextTests holds every form.
   /// </summary>
   public static class ServerTimeText
   {
      /// <summary>The form the server writes its times in, its own local time.</summary>
      public const string ServerFormat = "yyyy-MM-dd HH:mm:ss"; // no-loc: the server's format

      // The forms a database gives a datetime column in, read back as text: MySQL and
      // PostgreSQL write the server's form, SQL Server can add a fraction, and a "T"
      // or a date alone come from the REST API and hand-written values.
      private static readonly string[] Forms =
      {
         ServerFormat, "yyyy-MM-dd HH:mm:ss.FFFFFFF", "yyyy-MM-dd'T'HH:mm:ss", "yyyy-MM-dd'T'HH:mm:ss.FFFFFFF", "yyyy-MM-dd HH:mm", "yyyy-MM-dd" // no-loc
      };

      // The server's "no date": OLE 0 (30 December 1899) and the queue's 1901-01-01,
      // which means "as soon as possible"; nothing real is dated before 1970.
      private static readonly DateTime Earliest = new(1970, 1, 1);

      /// <summary>A time as the server writes it, or null: empty, another form, or a date before 1970, the server's "no date".</summary>
      public static DateTime? Parse(string text)
      {
         if (!DateTime.TryParseExact((text ?? "").Trim(), Forms, CultureInfo.InvariantCulture, DateTimeStyles.None, out DateTime when))
            return null;

         return when < Earliest ? null : when;
      }

      /// <summary>Whether the text is the server's "no date" - a date before 1970 in its own form - rather than no time at all.</summary>
      public static bool IsNoDate(string text)
         => DateTime.TryParseExact((text ?? "").Trim(), Forms, CultureInfo.InvariantCulture, DateTimeStyles.None, out DateTime when) && when < Earliest;

      /// <summary>The reader's short date and time: "08/10/2026 02:36", "08.10.2026 02:36", "2026/10/08 2:36".</summary>
      public static string Cell(DateTime when) => when.ToString("g", CultureInfo.CurrentCulture);

      /// <summary>The reader's short date and long time, with the seconds, where they count.</summary>
      public static string CellWithSeconds(DateTime when) => when.ToString("G", CultureInfo.CurrentCulture);

      /// <summary>
      /// The reader's short date alone - "08/10/2026", "08.10.2026", "2026/10/08" - for a
      /// day whose hour says nothing, in a sentence: the day a certificate expires, the day
      /// a setting profile was applied.
      /// </summary>
      public static string Date(DateTime when) => when.ToString("d", CultureInfo.CurrentCulture);

      /// <summary>The short date and time marked as the server's, for a value outside a column whose header names the clock.</summary>
      public static string Marked(DateTime when) => F("{0} (server time)", Cell(when));

      /// <summary>
      /// An instant a log gives in UTC - the Sigstore transparency log's integrated time,
      /// "2026-10-10T09:12:00Z" - in the reader's short date and time, marked UTC; the
      /// text as it came where it is no such time.
      /// </summary>
      public static string Utc(string iso)
         => DateTime.TryParseExact((iso ?? "").Trim(), UtcForms, CultureInfo.InvariantCulture,
               DateTimeStyles.AssumeUniversal | DateTimeStyles.AdjustToUniversal, out DateTime utc)
            ? F("{0} UTC", Cell(utc))
            : (iso ?? "").Trim();

      // The forms an instant in UTC comes in: with the zone's Z, an offset, or none.
      private static readonly string[] UtcForms =
      {
         "yyyy-MM-dd'T'HH:mm:ss'Z'", "yyyy-MM-dd'T'HH:mm:ssK", "yyyy-MM-dd'T'HH:mm:ss.FFFFFFF'Z'", "yyyy-MM-dd'T'HH:mm:ss" // no-loc
      };

      /// <summary>The whole time, marked as the server's: a cell's tool tip.</summary>
      public static string Full(DateTime when) => F("{0} (server time)", CellWithSeconds(when));

      /// <summary>A time the server wrote, in the reader's short date and time; the text as it came where it is not such a time.</summary>
      public static string Cell(string serverText)
         => Parse(serverText) is DateTime when ? Cell(when) : (serverText ?? "").Trim();

      /// <summary>A time the server wrote, in the reader's short date and the time to the second; the text as it came where it is not such a time.</summary>
      public static string CellWithSeconds(string serverText)
         => Parse(serverText) is DateTime when ? CellWithSeconds(when) : (serverText ?? "").Trim();

      /// <summary>
      /// A grid cell for a time the server wrote: how long ago or how soon it is where the
      /// server's clock is this computer's (<paramref name="now"/> given, a session on this
      /// computer), the reader's short date and time where it is not, whose column's
      /// header then says "(server time)"; <paramref name="noDate"/> for the server's "no
      /// date", and the text as it came where it is no time this reads.
      /// </summary>
      public static string GridCell(string serverText, DateTime? now, string noDate = "")
      {
         if (Parse(serverText) is DateTime when)
            return now is DateTime clock ? Relative(when, clock) : Cell(when);

         return IsNoDate(serverText) ? noDate : (serverText ?? "").Trim();
      }

      /// <summary>The whole time of a grid cell, marked as the server's, for its tool tip; null where there is no time.</summary>
      public static string GridTip(string serverText) => Parse(serverText) is DateTime when ? Full(when) : null;

      /// <summary>
      /// How long ago or how soon <paramref name="when"/> is from <paramref name="now"/>, in
      /// a grid cell's short units - "12 min ago", "in 12 min", "in 3 h", "in 2 d" - and
      /// "now" within a minute either way. Only where the two are one clock.
      /// </summary>
      public static string Relative(DateTime when, DateTime now)
      {
         TimeSpan ahead = when - now;
         if (ahead.Duration() < TimeSpan.FromMinutes(1))
            return L("now");

         if (ahead < TimeSpan.Zero)
            return AgeWords.Short(-ahead);

         if (ahead.TotalHours < 1)
            return F("in {0} min", (int)ahead.TotalMinutes);

         if (ahead.TotalDays < 2)
            return F("in {0} h", (int)ahead.TotalHours);

         return F("in {0} d", (int)ahead.TotalDays);
      }

      /// <summary>
      /// A Unix time as this computer's local time, the instant it names; null for 0 or
      /// less, the store's "never". In a session on this computer that is the server's
      /// time too, and the column says "(server time)"; in one from another computer it
      /// says "(this computer's time)".
      /// </summary>
      public static DateTime? FromUnix(long seconds)
         => seconds > 0 ? DateTimeOffset.FromUnixTimeSeconds(seconds).ToLocalTime().DateTime : null;
   }
}
