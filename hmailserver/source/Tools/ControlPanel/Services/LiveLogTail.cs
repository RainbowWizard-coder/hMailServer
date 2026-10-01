// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// Live logs' reading of the server's log and its following of the end, without the
   /// window (Views/LogsView draws what this decides), so that LiveLogTailTests can hold
   /// both.
   ///
   /// The reading: whole lines only, from where the last read stopped. The view read to
   /// the end of the file, so a line the server was part-way through writing became two
   /// rows - and a copy of them a line break the log does not have. An unfinished line
   /// now waits for the next read; a tail begun in the middle of a line (the last 64 KB
   /// of the file) starts at the next one; a byte order mark at the file's start is not
   /// a line's text; a file shorter than where the last read stopped has been replaced
   /// (rotated) and is read from its start.
   ///
   /// The following: the view follows the end of the log only while the end is in view.
   /// It used to jump to the newest line on every read, so on a busy server a line could
   /// not be read, let alone selected and copied, without Pause: somebody who scrolls up
   /// keeps their place, and scrolling back to the end follows again, as a terminal's
   /// tail does. The view keeps <see cref="MaxLines"/> lines, dropping the oldest; the
   /// list scrolls by line, so lines dropped from the top while somebody reads further
   /// down would slide what they read up by as many, and the view is moved back by that
   /// many instead.
   /// </summary>
   public sealed class LiveLogTail
   {
      /// <summary>The lines the view keeps; older ones are dropped from the top.</summary>
      public const int MaxLines = 2000;

      /// <summary>How far from the end of the file the first read starts: the last 64 KB.</summary>
      public const long TailBytes = 64 * 1024;

      private long position_;
      private bool startsMidLine_;

      /// <summary>Where the next read starts: the byte after the last whole line read.</summary>
      public long Position => position_;

      /// <summary>
      /// Starts the tail <see cref="TailBytes"/> before the end of a file this long, or
      /// at its start when it is shorter; the first read then starts at the first whole
      /// line from there.
      /// </summary>
      public void StartAt(long length)
      {
         position_ = Math.Max(0, length - TailBytes);
         startsMidLine_ = position_ > 0;
      }

      /// <summary>
      /// The whole lines written since the last read, in the log's order, each without
      /// its line ending (CR LF or LF), empty lines left out; none when nothing whole has
      /// been added.
      /// </summary>
      public IReadOnlyList<string> Read(Stream log)
      {
         if (log.Length < position_)
         {
            position_ = 0;   // rotated
            startsMidLine_ = false;
         }

         if (log.Length == position_)
            return Array.Empty<string>();

         // A tail begun mid-line reads from the byte before it: when that byte is a line
         // feed, the tail began exactly at a line's start and keeps that line.
         long from = startsMidLine_ ? position_ - 1 : position_;
         log.Seek(from, SeekOrigin.Begin);
         byte[] bytes = new byte[log.Length - from];
         int count = ReadFully_(log, bytes);
         int end = count > 0 ? Array.LastIndexOf(bytes, (byte)'\n', count - 1) : -1;
         if (end < 0)
            return Array.Empty<string>();

         int start = 0;
         if (startsMidLine_)
            start = Array.IndexOf(bytes, (byte)'\n', 0, end + 1) + 1;   // the byte before a line's start is a '\n'
         else if (from == 0 && count >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF)
            start = 3;                                                      // a byte order mark, as StreamReader dropped
         startsMidLine_ = false;
         position_ = from + end + 1;

         var lines = new List<string>();
         foreach (string raw in Encoding.UTF8.GetString(bytes, start, end + 1 - start).Split('\n'))
         {
            string line = raw.TrimEnd('\r');
            if (line.Length != 0)
               lines.Add(line);
         }

         return lines;
      }

      /// <summary>
      /// Whether a view scrolled to <paramref name="offset"/>, of the
      /// <paramref name="scrollable"/> it can scroll, shows the end of the log: within
      /// half a line of it.
      /// </summary>
      public static bool IsAtEnd(double offset, double scrollable) => offset >= scrollable - 0.5;

      /// <summary>
      /// Drops lines from the top of <paramref name="lines"/> until at most
      /// <paramref name="max"/> are left; how many were dropped.
      /// </summary>
      public static int Trim<T>(IList<T> lines, int max)
      {
         int dropped = 0;
         while (lines.Count > max)
         {
            lines.RemoveAt(0);
            dropped++;
         }

         return dropped;
      }

      /// <summary>
      /// Whether the view goes to the newest line after a read: only when it showed the
      /// end before the read, and the read added a line.
      /// </summary>
      public static bool Follows(bool wasAtEnd, int added) => wasAtEnd && added > 0;

      /// <summary>
      /// Where a view that did not show the end goes after <paramref name="dropped"/>
      /// lines were dropped from the top: back by as many lines, so that the lines being
      /// read stay where they are (not above the top). Null when it stays where it is: it
      /// showed the end and follows, or nothing was dropped.
      /// </summary>
      public static double? OffsetAfterDrop(bool wasAtEnd, double offset, int dropped)
         => wasAtEnd || dropped <= 0 ? null : Math.Max(0, offset - dropped);

      private static int ReadFully_(Stream stream, byte[] buffer)
      {
         int total = 0;
         while (total < buffer.Length)
         {
            int read = stream.Read(buffer, total, buffer.Length - total);
            if (read == 0)
               break;
            total += read;
         }

         return total;
      }
   }
}
