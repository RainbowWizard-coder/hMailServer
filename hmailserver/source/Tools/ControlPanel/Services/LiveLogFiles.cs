// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Globalization;
using System.IO;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>The logs the Live logs page can follow, as the server names their files (Logger::GetCurrentLogFileName).</summary>
   public enum LiveLogKind
   {
      /// <summary>hmailserver_&lt;date&gt;.log: every line but the errors' own file, the events and the backup.</summary>
      Everything,

      /// <summary>ERROR_hmailserver_&lt;date&gt;.log.</summary>
      Errors,

      /// <summary>hmailserver_events.log: what scripts write with EventLog.Write.</summary>
      Events,

      /// <summary>hmailserver_backup.log.</summary>
      Backup,

      /// <summary>hmailserver_SMTP_&lt;date&gt;.log, written while SepSvcLogs is on.</summary>
      Smtp,

      /// <summary>hmailserver_IMAP_&lt;date&gt;.log, written while SepSvcLogs is on.</summary>
      Imap,

      /// <summary>hmailserver_POP3_&lt;date&gt;.log, written while SepSvcLogs is on.</summary>
      Pop3
   }

   /// <summary>
   /// Which file the Live logs page reads and how one of its lines is shown, without the
   /// window (Views/LogsView draws what this decides), so that LiveLogFilesTests can hold
   /// both. Review finding F249: the page took the newest file matching hmailserver_*.log,
   /// which is as often the AWStats, backup or events log or a per-service log as the
   /// day's; it kept that file past midnight, when the server opens the next day's, and
   /// went quiet while it still said Streaming; and it showed every line with the file's
   /// quotes and tabs.
   ///
   /// The file is the one the server writes today, by its own name and its own date (the
   /// server's local date; the page reads the log folder on the server's computer, so
   /// the two clocks are one). A line is split into its time, its type, its thread, its
   /// session and its text, the quotes the file puts round the text fields taken off; a
   /// line in a shape this does not know - the JSON or NCSA format, a line of the AWStats
   /// log - is shown whole as its text.
   /// </summary>
   public static class LiveLogFiles
   {
      /// <summary>The file's name for that log on that day.</summary>
      public static string FileName(LiveLogKind kind, DateTime day)
      {
         string date = day.ToString("yyyy-MM-dd", CultureInfo.InvariantCulture);
         return kind switch
         {
            LiveLogKind.Errors => "ERROR_hmailserver_" + date + ".log",
            LiveLogKind.Events => "hmailserver_events.log",
            LiveLogKind.Backup => "hmailserver_backup.log",
            LiveLogKind.Smtp => "hmailserver_SMTP_" + date + ".log",
            LiveLogKind.Imap => "hmailserver_IMAP_" + date + ".log",
            LiveLogKind.Pop3 => "hmailserver_POP3_" + date + ".log",
            _ => "hmailserver_" + date + ".log"
         };
      }

      /// <summary>Whether the server starts a new file of that log every day.</summary>
      public static bool IsDaily(LiveLogKind kind) => kind is not (LiveLogKind.Events or LiveLogKind.Backup);

      /// <summary>
      /// The file to read from now on, when the day has turned over: the new day's file of
      /// a daily log once it exists, or null while the page should go on with the file it
      /// has - the same day, a log that is not daily, or a new day's file the server has
      /// not opened yet (it opens it with its first line after midnight).
      /// </summary>
      public static string NextDay(string reading, LiveLogKind kind, DateTime today, Func<string, bool> exists)
      {
         if (reading == null || !IsDaily(kind))
            return null;

         string folder = Path.GetDirectoryName(reading);
         string want = Path.Join(folder, FileName(kind, today));
         if (string.Equals(want, reading, StringComparison.OrdinalIgnoreCase))
            return null;

         return exists(want) ? want : null;
      }

      /// <summary>One line of a log, in the parts the page shows it in.</summary>
      public sealed class Parts
      {
         public string Time { get; init; } = "";
         public string Type { get; init; } = "";
         public string Thread { get; init; } = "";
         public string Session { get; init; } = "";
         public string Text { get; init; } = "";
      }

      /// <summary>
      /// A line of that log in its parts. The server's default format is tab-separated
      /// (Logger::Render_): "TYPE", thread, session, "time", "remote host", "text" for a
      /// line of a conversation, "TYPE", thread, "time", "text" for the others; the
      /// events log writes thread, "time", "text" and the backup log time and text. The
      /// remote host leads the text. A daily log's time is shown without its date, which
      /// is the file's; the others keep theirs.
      /// </summary>
      public static Parts Split(string line, LiveLogKind kind)
      {
         if (string.IsNullOrEmpty(line) || line[0] == '{')
            return new Parts { Text = line ?? "" };

         string[] f = line.Split('\t');

         if (kind == LiveLogKind.Backup)
         {
            return f.Length < 2
               ? new Parts { Text = line }
               : new Parts { Time = Time_(f[0], false), Text = string.Join("\t", f, 1, f.Length - 1) };
         }

         if (kind == LiveLogKind.Events)
         {
            return f.Length < 3 || !IsQuoted_(f[1])
               ? new Parts { Text = line }
               : new Parts { Thread = f[0], Time = Time_(Unquote_(f[1]), false), Text = Unquote_(string.Join("\t", f, 2, f.Length - 2)) };
         }

         bool daily = IsDaily(kind);
         if (f.Length >= 6 && IsQuoted_(f[0]) && IsQuoted_(f[3]) && IsQuoted_(f[4]))
         {
            string remote = Unquote_(f[4]);
            string text = Unquote_(string.Join("\t", f, 5, f.Length - 5));
            return new Parts
            {
               Type = Unquote_(f[0]),
               Thread = f[1],
               Session = f[2],
               Time = Time_(Unquote_(f[3]), daily),
               Text = remote.Length == 0 ? text : remote + "  " + text
            };
         }

         if (f.Length >= 4 && IsQuoted_(f[0]) && IsQuoted_(f[2]))
         {
            return new Parts
            {
               Type = Unquote_(f[0]),
               Thread = f[1],
               Time = Time_(Unquote_(f[2]), daily),
               Text = Unquote_(string.Join("\t", f, 3, f.Length - 3))
            };
         }

         return new Parts { Text = line };
      }

      private static bool IsQuoted_(string field) => field.Length >= 2 && field[0] == '"' && field[^1] == '"';

      private static string Unquote_(string field) => IsQuoted_(field) ? field.Substring(1, field.Length - 2) : field;

      // "2026-10-08 14:02:03.123": the time of day alone in a daily log, whose file
      // names the day; anything else as it was written.
      private static string Time_(string stamp, bool dropDate)
         => dropDate && stamp.Length > 11 && stamp[10] == ' ' && stamp[4] == '-' && stamp[7] == '-' ? stamp.Substring(11) : stamp;
   }
}
