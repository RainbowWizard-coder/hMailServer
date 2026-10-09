// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
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

   /// <summary>What opening a log file, or listing the log folder, found (LiveLogFiles.TryOpen, FolderAccess).</summary>
   public enum LiveLogAccess
   {
      /// <summary>There, and this Control Panel may read it.</summary>
      Readable,

      /// <summary>Not there, or not yet.</summary>
      NotThere,

      /// <summary>This Control Panel may not read it: it belongs to the administrators (hMailServer.exe /ProtectFolders).</summary>
      Denied,

      /// <summary>There, and held by somebody else for a moment: looked at again on the next tick.</summary>
      Busy
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

      /// <summary>
      /// Opens a log file to read beside the server, which goes on writing it; the
      /// stream is the caller's to close, and null unless the file is Readable.
      ///
      /// The file is opened, and not looked for first: in a log folder that belongs to
      /// the administrators - which hMailServer.exe /ProtectFolders leaves - a Control
      /// Panel not run as administrator finds the folder with Directory.Exists and no
      /// file in it with File.Exists, which answer false where they may not look, and
      /// only opening the file says that it may not be read (review finding F249: the
      /// page said Streaming over a log it could no longer read, or that nothing had
      /// been logged today).
      /// </summary>
      public static LiveLogAccess TryOpen(string path, out FileStream stream)
      {
         stream = null;
         try
         {
            stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            return LiveLogAccess.Readable;
         }
         catch (FileNotFoundException)
         {
            return LiveLogAccess.NotThere;
         }
         catch (DirectoryNotFoundException)
         {
            return LiveLogAccess.NotThere;
         }
         catch (UnauthorizedAccessException)
         {
            return LiveLogAccess.Denied;
         }
         catch (IOException)
         {
            return LiveLogAccess.Busy;
         }
         catch (ArgumentException)
         {
            return LiveLogAccess.NotThere;   // not a path at all
         }
      }

      /// <summary>What opening the file finds, the file closed again.</summary>
      public static LiveLogAccess Access(string path)
      {
         LiveLogAccess access = TryOpen(path, out FileStream stream);
         stream?.Dispose();
         return access;
      }

      /// <summary>
      /// Whether the log folder is there and may be listed, by listing it: a folder that
      /// belongs to the administrators cannot be, and Directory.Exists finds it there or
      /// not as its parent may be listed or not.
      /// </summary>
      public static LiveLogAccess FolderAccess(string folder)
      {
         if (string.IsNullOrEmpty(folder))
            return LiveLogAccess.NotThere;

         try
         {
            using IEnumerator<string> entries = Directory.EnumerateFileSystemEntries(folder).GetEnumerator();
            entries.MoveNext();
            return LiveLogAccess.Readable;
         }
         catch (UnauthorizedAccessException)
         {
            return LiveLogAccess.Denied;
         }
         catch (IOException)
         {
            return LiveLogAccess.NotThere;   // not found, or a file and not a folder
         }
         catch (ArgumentException)
         {
            return LiveLogAccess.NotThere;
         }
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

      // The quotation mark the server puts round a text field, by its code: written
      // as a character literal it reads to build/check-localisation.py as the start
      // of a caption.
      private const char Quote = (char)34;

      private static bool IsQuoted_(string field) => field.Length >= 2 && field[0] == Quote && field[^1] == Quote;

      private static string Unquote_(string field) => IsQuoted_(field) ? field.Substring(1, field.Length - 2) : field;

      // "2026-10-08 14:02:03.123": the time of day alone in a daily log, whose file
      // names the day; anything else as it was written.
      private static string Time_(string stamp, bool dropDate)
         => dropDate && stamp.Length > 11 && stamp[10] == ' ' && stamp[4] == '-' && stamp[7] == '-' ? stamp.Substring(11) : stamp;
   }
}
