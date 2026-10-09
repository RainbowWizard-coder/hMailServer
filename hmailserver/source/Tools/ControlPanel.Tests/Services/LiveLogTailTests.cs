// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// Live logs (issue #48): what the page reads from the server's log and where its
   /// view goes as lines arrive (Services/LiveLogTail.cs; Views/LogsView draws it).
   /// The reading is held against a log written into memory a piece at a time, as the
   /// server writes it; the following against the numbers WPF's scroll viewer gives a
   /// list that scrolls by line, which is how the live log's list scrolls (its
   /// virtualising panel's unit is the item, held below).
   /// </summary>
   public class LiveLogTailTests
   {
      private static readonly Encoding Utf8 = new UTF8Encoding(false);

      // ---- reading -------------------------------------------------------------------

      [Fact]
      public void ALineTheServerIsStillWriting_IsReadWhenItEnds_AsOneLine()
      {
         using var log = new MemoryStream();
         var tail = new LiveLogTail();

         Append(log, "\"SMTPD\"\t1\t\"first\"\r\n\"SMTPD\"\t2\t\"half a li");
         Assert.Equal(new[] { "\"SMTPD\"\t1\t\"first\"" }, tail.Read(log));

         Assert.Empty(tail.Read(log));   // still unfinished: nothing new, and nothing half

         Append(log, "ne, then the rest\"\r\n");
         Assert.Equal(new[] { "\"SMTPD\"\t2\t\"half a line, then the rest\"" }, tail.Read(log));
         Assert.Equal(log.Length, tail.Position);
      }

      [Fact]
      public void LinesKeepTheirWords_LoseOnlyTheirLineEnding_AndEmptyOnesAreLeftOut()
      {
         using var log = new MemoryStream();
         var tail = new LiveLogTail();

         Append(log, "a\t tabs and  spaces \r\n\r\nb\nc ü 日本\r\n");
         Assert.Equal(new[] { "a\t tabs and  spaces ", "b", "c ü 日本" }, tail.Read(log));
      }

      [Fact]
      public void AByteOrderMarkAtTheStart_IsNotPartOfTheFirstLine()
      {
         using var log = new MemoryStream();
         var tail = new LiveLogTail();

         log.Write(new byte[] { 0xEF, 0xBB, 0xBF });
         Append(log, "first\r\n");
         Assert.Equal(new[] { "first" }, tail.Read(log));
      }

      [Fact]
      public void ATailBegunInTheMiddleOfALine_StartsAtTheNextLine()
      {
         // A thousand lines of 100 bytes with their CR LF: the last TailBytes of the
         // 100,000 begin 64 bytes into line 344, so the first line read is line 345, whole.
         string[] lines = Enumerable.Range(0, 1000).Select(i => ("\"SMTPD\"\t" + i + "\t\"line\"").PadRight(98, 'x')).ToArray();
         using var log = new MemoryStream();
         Append(log, string.Concat(lines.Select(line => line + "\r\n")));
         Assert.Equal(100_000, log.Length);

         var tail = new LiveLogTail();
         tail.StartAt(log.Length);
         Assert.Equal(100_000 - LiveLogTail.TailBytes, tail.Position);
         Assert.Equal(64, tail.Position % 100);

         IReadOnlyList<string> read = tail.Read(log);
         Assert.Equal(lines[345..], read);
         Assert.Equal(log.Length, tail.Position);
      }

      [Fact]
      public void ATailBegunExactlyAtALinesStart_KeepsThatLine()
      {
         // The last TailBytes of the file are exactly its last line, so the tail
         // begins at that line's first byte, just after the line feed before it.
         string last = new string('w', (int)LiveLogTail.TailBytes - 2);
         using var log = new MemoryStream();
         Append(log, "zzzzzzzzzz\r\n" + last + "\r\n");

         var tail = new LiveLogTail();
         tail.StartAt(log.Length);
         Assert.Equal(12, tail.Position);
         Assert.Equal(new[] { last }, tail.Read(log));
      }

      [Fact]
      public void AReplacedLog_IsReadFromItsStart()
      {
         using var log = new MemoryStream();
         var tail = new LiveLogTail();
         Append(log, "one\r\ntwo\r\nthree\r\n");
         Assert.Equal(3, tail.Read(log).Count);

         log.SetLength(0);
         Append(log, "new\r\n");
         Assert.Equal(new[] { "new" }, tail.Read(log));
      }

      // ---- the UTF-16LE logs ------------------------------------------------------------

      // The script events and backup logs as the server writes them on Windows: UTF-16LE,
      // the byte order mark first (Logger.cpp, WritesUtf16_; File::Write(String)).
      private static readonly Encoding Utf16 = new UnicodeEncoding(bigEndian: false, byteOrderMark: false);

      private static void AppendUtf16(Stream log, string text)
      {
         byte[] bytes = Utf16.GetBytes(text);
         log.Seek(0, SeekOrigin.End);
         log.Write(bytes, 0, bytes.Length);
      }

      private static MemoryStream Utf16Log(string text)
      {
         var log = new MemoryStream();
         log.Write(new byte[] { 0xFF, 0xFE });
         AppendUtf16(log, text);
         return log;
      }

      [Fact]
      public void AUtf16Log_WithItsMark_IsReadAsText_WithoutTheMarkOrANul()
      {
         // An events line - thread, "time", "text" - and a backup line - time, text.
         using MemoryStream log = Utf16Log("4712\t\"2026-10-09 10:00:00.123\"\t\"OnAcceptMessage ran: ü 日本\"\r\n2026-10-09 10:01:00.456\tBackup started\r\n");
         var tail = new LiveLogTail();

         IReadOnlyList<string> read = tail.Read(log);
         Assert.Equal(new[] { "4712\t\"2026-10-09 10:00:00.123\"\t\"OnAcceptMessage ran: ü 日本\"", "2026-10-09 10:01:00.456\tBackup started" }, read);
         Assert.DoesNotContain(read, line => line.Contains('\0') || line.Contains('�') || line.Contains('﻿'));
         Assert.Equal(log.Length, tail.Position);
         Assert.Equal(1200, tail.Encoding.CodePage);

         // So each splits into its parts, its quotes and tabs taken off.
         LiveLogFiles.Parts events = LiveLogFiles.Split(read[0], LiveLogKind.Events);
         Assert.Equal("4712", events.Thread);
         Assert.Equal("2026-10-09 10:00:00.123", events.Time);
         Assert.Equal("OnAcceptMessage ran: ü 日本", events.Text);
         LiveLogFiles.Parts backup = LiveLogFiles.Split(read[1], LiveLogKind.Backup);
         Assert.Equal("2026-10-09 10:01:00.456", backup.Time);
         Assert.Equal("Backup started", backup.Text);
      }

      [Fact]
      public void AUtf16Line_TheServerIsStillWriting_WaitsForItsEnd_HalfACharacterIncluded()
      {
         using MemoryStream log = Utf16Log("first\r\nsec");
         var tail = new LiveLogTail();
         Assert.Equal(new[] { "first" }, tail.Read(log));

         // Half of the next character: the line waits whole, and so does the byte.
         log.Seek(0, SeekOrigin.End);
         log.WriteByte((byte)'o');
         Assert.Empty(tail.Read(log));

         log.WriteByte(0);
         AppendUtf16(log, "nd\r\n");
         Assert.Equal(new[] { "second" }, tail.Read(log));
         Assert.Equal(log.Length, tail.Position);
      }

      [Fact]
      public void AUtf16Line_EndsOnlyAtALineFeedOnACharactersBoundary()
      {
         // U+0A0A followed by U+0100 is the bytes 0A 0A 00 01: a line feed and a NUL
         // side by side, across two characters - not a line's end.
         using MemoryStream log = Utf16Log("aਊĀb\r\nc\r\n");
         Assert.Equal(new[] { "aਊĀb", "c" }, new LiveLogTail().Read(log));
      }

      [Fact]
      public void AUtf16TailBegunMidFile_AtAnOddByte_StartsAtTheNextWholeLine()
      {
         // A thousand lines of 100 characters with their CR LF, 200 bytes each, after
         // the mark; the last character half written makes the length odd, so the
         // tail would begin in the middle of a character, and begins a byte earlier.
         string[] lines = Enumerable.Range(0, 1000).Select(i => (i + "\t\"2026-10-09 10:00:00.000\"\t\"event\"").PadRight(98, 'x')).ToArray();
         using MemoryStream log = Utf16Log(string.Concat(lines.Select(line => line + "\r\n")));
         log.Seek(0, SeekOrigin.End);
         log.WriteByte((byte)'h');
         Assert.Equal(2 + 200_000 + 1, log.Length);

         var tail = new LiveLogTail();
         tail.StartAt(log.Length);
         Assert.Equal(1, tail.Position % 2);

         // The tail's first byte, 200,003 - 65,536 = 134,467, rounds to 134,466: 64
         // bytes, 32 characters, into line 672 (whose first byte is 2 + 672 * 200).
         IReadOnlyList<string> read = tail.Read(log);
         Assert.Equal(lines[673..], read);
         Assert.Equal(log.Length - 1, tail.Position);
      }

      [Fact]
      public void AUtf16TailBegunExactlyAtALinesStart_KeepsThatLine()
      {
         // The last TailBytes of the file are exactly its last line.
         string last = new string('w', (int)LiveLogTail.TailBytes / 2 - 2);
         using MemoryStream log = Utf16Log("zzzzzzzzzz\r\n" + last + "\r\n");

         var tail = new LiveLogTail();
         tail.StartAt(log.Length);
         Assert.Equal(2 + 24, tail.Position);
         Assert.Equal(new[] { last }, tail.Read(log));
      }

      [Fact]
      public void AUtf16LogWithoutItsMark_IsUtf16Still_AsCopytruncateLeavesIt()
      {
         // logrotate's copytruncate empties the file and the server goes on writing
         // UTF-16LE with no mark: the second byte NUL under a first that is not.
         using var log = new MemoryStream();
         AppendUtf16(log, "2026-10-09 10:01:00.456\tBackup started\r\n");
         var tail = new LiveLogTail();
         Assert.Equal(new[] { "2026-10-09 10:01:00.456\tBackup started" }, tail.Read(log));
         Assert.Equal(1200, tail.Encoding.CodePage);
      }

      [Fact]
      public void AReplacedLog_IsDecidedAgain_UTF8AfterUTF16()
      {
         using MemoryStream log = Utf16Log("old one\r\nold two\r\nold three\r\n");
         var tail = new LiveLogTail();
         Assert.Equal(3, tail.Read(log).Count);

         log.SetLength(0);
         Append(log, "new\r\n");
         Assert.Equal(new[] { "new" }, tail.Read(log));
         Assert.Equal(65001, tail.Encoding.CodePage);
      }

      [Fact]
      public void AUtf8Log_StaysUtf8_AndAOneByteFileWaits()
      {
         using var log = new MemoryStream();
         var tail = new LiveLogTail();
         Append(log, "\"");
         Assert.Empty(tail.Read(log));
         Assert.Null(tail.Encoding);

         Append(log, "SMTPD\"\t1\t\"ok\"\r\n");
         Assert.Equal(new[] { "\"SMTPD\"\t1\t\"ok\"" }, tail.Read(log));
         Assert.Equal(65001, tail.Encoding.CodePage);
      }

      // ---- following -----------------------------------------------------------------

      [Fact]
      public void TheEnd_IsInViewWithinHalfALine()
      {
         Assert.True(LiveLogTail.IsAtEnd(0, 0));          // nothing to scroll
         Assert.True(LiveLogTail.IsAtEnd(120, 120));
         Assert.True(LiveLogTail.IsAtEnd(119.6, 120));
         Assert.False(LiveLogTail.IsAtEnd(119.4, 120));
         Assert.False(LiveLogTail.IsAtEnd(5, 120));       // scrolled up to read
      }

      [Fact]
      public void TheViewFollows_OnlyWhileTheEndWasInView_AndOnlyWhenALineCame()
      {
         Assert.True(LiveLogTail.Follows(wasAtEnd: true, added: 3));
         Assert.False(LiveLogTail.Follows(wasAtEnd: true, added: 0));
         Assert.False(LiveLogTail.Follows(wasAtEnd: false, added: 3));   // somebody scrolled up: they keep their place
      }

      [Fact]
      public void LinesDroppedFromTheTop_MoveAReadersViewBackByAsMany()
      {
         // Reading at line 900 when three lines are dropped from the top: the lines
         // read were at 900 and are now at 897, so the view goes to 897.
         Assert.Equal(897, LiveLogTail.OffsetAfterDrop(wasAtEnd: false, offset: 900, dropped: 3));
         // Not above the top.
         Assert.Equal(0, LiveLogTail.OffsetAfterDrop(wasAtEnd: false, offset: 2, dropped: 5));
         // Nothing dropped: it stays. Following the end: it goes to the end instead.
         Assert.Null(LiveLogTail.OffsetAfterDrop(wasAtEnd: false, offset: 900, dropped: 0));
         Assert.Null(LiveLogTail.OffsetAfterDrop(wasAtEnd: true, offset: 900, dropped: 3));
      }

      [Fact]
      public void TheViewKeepsTheNewestMaxLines_DroppingTheOldest()
      {
         var lines = Enumerable.Range(0, LiveLogTail.MaxLines + 100).ToList();
         Assert.Equal(100, LiveLogTail.Trim(lines, LiveLogTail.MaxLines));
         Assert.Equal(LiveLogTail.MaxLines, lines.Count);
         Assert.Equal(100, lines[0]);
         Assert.Equal(LiveLogTail.MaxLines + 99, lines[^1]);
         Assert.Equal(0, LiveLogTail.Trim(lines, LiveLogTail.MaxLines));
         Assert.Equal(2000, LiveLogTail.MaxLines);
      }

      /// <summary>
      /// A whole tick, as LogsView runs it: a reader scrolled up holds the same line in
      /// view while lines arrive and the oldest go; one at the end follows the new ones.
      /// The list is modelled as WPF's scroll viewer gives a list that scrolls by line:
      /// the offset is a line's index, the scrollable range the lines less a screenful.
      /// </summary>
      [Fact]
      public void AReaderHoldsTheirLine_AndAFollowerSeesTheNewest_AsTheLogGrows()
      {
         const int screen = 30;
         var view = new List<string>();
         using var log = new MemoryStream();
         var tail = new LiveLogTail();
         int written = 0;
         double offset = 0;

         void Tick(int count)
         {
            for (int i = 0; i < count; i++)
               Append(log, "line " + written++ + "\r\n");

            IReadOnlyList<string> read = tail.Read(log);
            double scrollable = Math.Max(0, view.Count - screen);
            bool atEnd = LiveLogTail.IsAtEnd(offset, scrollable);
            view.AddRange(read);
            int dropped = LiveLogTail.Trim(view, LiveLogTail.MaxLines);
            if (LiveLogTail.Follows(atEnd, read.Count))
               offset = Math.Max(0, view.Count - screen);
            else if (LiveLogTail.OffsetAfterDrop(atEnd, offset, dropped) is double back)
               offset = back;
         }

         Tick(LiveLogTail.MaxLines);
         Assert.Equal("line " + (LiveLogTail.MaxLines - 1), view[(int)offset + screen - 1]);   // following: the newest is on screen

         offset = 900;                       // the reader scrolls up to line 900
         string held = view[900];
         Tick(3);                            // three arrive, three oldest go
         Assert.Equal(held, view[(int)offset]);
         Tick(250);
         Assert.Equal(held, view[(int)offset]);

         offset = Math.Max(0, view.Count - screen);   // back to the end: following again
         Tick(5);
         Assert.Equal("line " + (written - 1), view[(int)offset + screen - 1]);
      }

      /// <summary>
      /// The holding above rests on the live log's list scrolling by line: its offset
      /// counts lines only while the list's panel scrolls by item and the scroll viewer
      /// lets the panel scroll (CanContentScroll). A list scrolled by pixel would move
      /// back three pixels for three lines.
      /// </summary>
      [Fact]
      public void TheLiveLogsList_ScrollsByLine()
      {
         string xaml = ReadControlPanelFile(Path.Join("Views", "LogsView.xaml"));
         Assert.Contains("VirtualizingPanel.IsVirtualizing=\"True\"", xaml);
         Assert.DoesNotContain("ScrollUnit=\"Pixel\"", xaml);
         Assert.DoesNotContain("CanContentScroll=\"False\"", xaml);

         string code = ReadControlPanelFile(Path.Join("Views", "LogsView.xaml.cs"));
         Assert.DoesNotContain("ScrollUnit.Pixel", code);
         Assert.DoesNotContain("CanContentScroll", code);
      }

      /// <summary>LogsView reads through the tail and decides through it, in this order.</summary>
      [Fact]
      public void TheLiveLogsPage_ReadsAndFollowsThroughTheTail()
      {
         string code = ReadControlPanelFile(Path.Join("Views", "LogsView.xaml.cs"));
         string poll = WithheldRowTests.MemberText(code, "private void Poll()");
         Assert.True(poll != null, "LogsView.Poll is gone.");

         // Poll reads, and Show_ puts what it read into the view - or, while the view
         // is paused, among the lines waiting, which the subtitle counts (review
         // finding F249) - so the two are read as one here.
         string show = WithheldRowTests.MemberText(code, "private void Show_(IReadOnlyList<string> read)");
         Assert.True(show != null, "LogsView.Show_ is gone.");
         Assert.Contains("Show_(tail_.Read(stream));", poll);
         poll += show;

         int read = poll.IndexOf("tail_.Read(stream)", StringComparison.Ordinal);
         int atEnd = poll.IndexOf("LiveLogTail.IsAtEnd(scroller.VerticalOffset, scroller.ScrollableHeight)", StringComparison.Ordinal);
         int add = poll.IndexOf("lines_.Add(", StringComparison.Ordinal);
         int trim = poll.IndexOf("LiveLogTail.Trim(lines_, LiveLogTail.MaxLines)", StringComparison.Ordinal);
         int follows = poll.IndexOf("LiveLogTail.Follows(atEnd, read.Count)", StringComparison.Ordinal);
         int back = poll.IndexOf("LiveLogTail.OffsetAfterDrop(atEnd, offset, dropped)", StringComparison.Ordinal);

         Assert.True(read >= 0 && atEnd > read, "Poll no longer reads through LiveLogTail before it looks at the view.");
         Assert.True(add > atEnd, "Whether the end was in view is asked after the new lines are added, when it never is.");
         Assert.True(trim > add && follows > trim && back > follows, "Poll no longer trims, then follows or holds, through LiveLogTail.");
         Assert.Contains("scroller.ScrollToVerticalOffset(back);", poll);
         Assert.Contains("LogList.ScrollIntoView(LogList.Items[LogList.Items.Count - 1]);", poll);
      }

      private static void Append(Stream log, string text)
      {
         byte[] bytes = Utf8.GetBytes(text);
         log.Seek(0, SeekOrigin.End);
         log.Write(bytes, 0, bytes.Length);
      }

      private static string ReadControlPanelFile(string relative)
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", relative);
            if (File.Exists(candidate))
               return File.ReadAllText(candidate);
            directory = directory.Parent;
         }

         Assert.Fail("ControlPanel/" + relative.Replace('\\', '/') + " could not be found from the test binaries.");
         return null;
      }
   }
}
