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
   ///
   /// The encoding: the server writes its script events and backup logs on Windows in
   /// UTF-16LE with a byte order mark, as it always has (Logger.cpp, WritesUtf16_), and
   /// every other log in UTF-8. Read as UTF-8, each line of those two came out with a
   /// NUL after every character and the mark as two replacement characters, and could
   /// not be split into its parts. A file is UTF-16LE where it starts with the mark FF
   /// FE, or where its second byte is NUL under a first that is not - the mark gone
   /// with the rest of a file logrotate's copytruncate emptied, as Logger.cpp reads it -
   /// and UTF-8 otherwise; it is decided at the first read with two bytes to read, and
   /// again after the file has been replaced. In UTF-16LE a line ends with the pair 0A
   /// 00 at an even offset, the tail begins on an even offset, and half a character at
   /// the end waits with the rest of its line.
   /// </summary>
   public sealed class LiveLogTail
   {
      /// <summary>The lines the view keeps; older ones are dropped from the top.</summary>
      public const int MaxLines = 2000;

      /// <summary>How far from the end of the file the first read starts: the last 64 KB.</summary>
      public const long TailBytes = 64 * 1024;

      private long position_;
      private bool startsMidLine_;

      // The file's encoding, null until a read has had two bytes to decide it by; and
      // the bytes of one of its code units, 1 for UTF-8 and 2 for UTF-16LE.
      private Encoding encoding_;
      private int unit_ = 1;

      /// <summary>Where the next read starts: the byte after the last whole line read.</summary>
      public long Position => position_;

      /// <summary>The encoding the file is read in: UTF-8 or UTF-16LE; null before the first read that could tell.</summary>
      public Encoding Encoding => encoding_;

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
            position_ = 0;   // rotated, and perhaps written in another encoding now
            startsMidLine_ = false;
            encoding_ = null;
         }

         if (encoding_ == null)
         {
            // Too short to tell: waited for, as an unfinished line is.
            if (log.Length < 2)
               return Array.Empty<string>();

            DecideEncoding_(log);
         }

         if (log.Length == position_)
            return Array.Empty<string>();

         // A tail begun mid-line reads from the code unit before it: when that unit is
         // a line feed, the tail began exactly at a line's start and keeps that line.
         long from = startsMidLine_ ? position_ - unit_ : position_;
         log.Seek(from, SeekOrigin.Begin);
         byte[] bytes = new byte[log.Length - from];
         int count = ReadFully_(log, bytes);
         int end = LastLineEnd_(bytes, count);
         if (end < 0)
            return Array.Empty<string>();

         int start = 0;
         if (startsMidLine_)
            start = AfterFirstLineEnd_(bytes, end);   // the unit before a line's start is a line feed
         else if (from == 0)
            start = MarkLength_(bytes, count);        // a byte order mark, as StreamReader dropped
         startsMidLine_ = false;
         position_ = from + end + 1;

         var lines = new List<string>();
         foreach (string raw in encoding_.GetString(bytes, start, end + 1 - start).Split('\n'))
         {
            string line = raw.TrimEnd('\r');
            if (line.Length != 0)
               lines.Add(line);
         }

         return lines;
      }

      /// <summary>
      /// UTF-16LE where the file starts with FF FE, or with a byte that is not NUL
      /// over one that is (Logger.cpp's ConvertUtf16Log_ asks the same); UTF-8
      /// otherwise. A tail of a UTF-16LE file begins on a code unit: an odd start, from
      /// a file whose last character was half written, goes back a byte.
      /// </summary>
      private void DecideEncoding_(Stream log)
      {
         var head = new byte[2];
         log.Seek(0, SeekOrigin.Begin);
         bool utf16 = ReadFully_(log, head) == 2 && ((head[0] == 0xFF && head[1] == 0xFE) || (head[0] != 0 && head[1] == 0));

         encoding_ = utf16 ? new UnicodeEncoding(bigEndian: false, byteOrderMark: false) : new UTF8Encoding(false);
         unit_ = utf16 ? 2 : 1;

         if (utf16 && position_ % 2 != 0)
         {
            position_--;
            startsMidLine_ = position_ > 0;
         }
      }

      /// <summary>
      /// The index of the last byte of the last whole line ending in the first
      /// <paramref name="count"/> bytes - a '\n', or in UTF-16LE the 00 of an 0A 00 on
      /// a code unit's boundary - or -1 where there is none.
      /// </summary>
      private int LastLineEnd_(byte[] bytes, int count)
      {
         if (unit_ == 1)
            return count > 0 ? Array.LastIndexOf(bytes, (byte)'\n', count - 1) : -1;

         for (int i = (count - 2) & ~1; i >= 0; i -= 2)
         {
            if (bytes[i] == (byte)'\n' && bytes[i + 1] == 0)
               return i + 1;
         }

         return -1;
      }

      /// <summary>The index after the first line ending at or before <paramref name="last"/>, which is one.</summary>
      private int AfterFirstLineEnd_(byte[] bytes, int last)
      {
         if (unit_ == 1)
            return Array.IndexOf(bytes, (byte)'\n', 0, last + 1) + 1;

         for (int i = 0; i < last; i += 2)
         {
            if (bytes[i] == (byte)'\n' && bytes[i + 1] == 0)
               return i + 2;
         }

         return last + 1;
      }

      /// <summary>The length of the byte order mark the file starts with in its encoding, or 0.</summary>
      private int MarkLength_(byte[] bytes, int count)
      {
         if (unit_ == 2)
            return count >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE ? 2 : 0;

         return count >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF ? 3 : 0;
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
