// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Runtime.InteropServices;
using System.Threading;
using System.Windows;
using System.Windows.Threading;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// Every Copy in the Control Panel puts its text on the clipboard here, as plain
   /// text: the Copy buttons (DNS records, the message source, the setup keys of
   /// two-factor sign-in, a new API key or app password, the MX query and the
   /// diagnostics, a chart's samples), a generated password, and the text and
   /// list rows that can be selected (SelectableText, SelectableList).
   ///
   /// In two steps, not Clipboard.SetText's one. SetText hands the text over and
   /// then flushes it - makes the clipboard hold its own copy, so the text
   /// outlives this program - at once, retrying with the UI thread asleep. A
   /// program watching the clipboard (Windows' clipboard history, the Remote
   /// Desktop client that shares the clipboard with WSL's windows) opens it the
   /// moment it changes and asks this program for the text; asleep, the UI thread
   /// cannot answer, the watcher keeps the clipboard open waiting, and every retry
   /// of the flush fails: CLIPBRD_E_CANT_OPEN after about a second, with the
   /// Control Panel frozen for that second, on every copy the watcher is quick
   /// enough for. .NET Framework's WPF and .NET 10's both did so on the laptop
   /// this was written on - measured, a chart's Copy with such a watcher running
   /// showed no "copied" toast five times in five, the samples reaching the
   /// clipboard only after 1.03 seconds, where this took 80 milliseconds and
   /// showed it. So the text goes on the clipboard first - it can be pasted from
   /// that moment - and the flush follows from the dispatcher, once the watcher
   /// has been answered, and again as the Control Panel exits
   /// (<see cref="FlushBeforeExit"/>).
   /// </summary>
   public static partial class ClipboardWriter
   {
      private const int FlushAttempts = 20;

      // The last text put on the clipboard and not yet flushed, if any.
      private static IDataObject pending_;

      /// <summary>Puts <paramref name="text"/> on the clipboard; false when it could not.</summary>
      public static bool TrySetText(string text) => TrySetText(text, out _);

      /// <summary>
      /// Puts <paramref name="text"/> on the clipboard, as plain text. False, with
      /// the reason in <paramref name="error"/>, when the clipboard could not be
      /// written - another program has held it open for a second or more. Nothing
      /// to copy (null or empty) is not a failure and leaves the clipboard as it was.
      /// </summary>
      public static bool TrySetText(string text, out string error)
      {
         error = null;
         if (string.IsNullOrEmpty(text))
            return true;

         var data = new DataObject(DataFormats.UnicodeText, text);
         try
         {
            Clipboard.SetDataObject(data, false);
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            error = ex.Message;
            return false;
         }

         pending_ = data;
         FlushWhenFree_(data, 0);
         return true;
      }

      /// <summary>
      /// Makes the clipboard keep its own copy of <paramref name="data"/>, so a copy
      /// made just before the Control Panel is closed still pastes. One attempt per
      /// tick, with the dispatcher running in between, while the text is still the
      /// clipboard's; given up after about three seconds, when the text is still
      /// there to paste for as long as the Control Panel runs, and flushed as it exits.
      /// </summary>
      private static void FlushWhenFree_(IDataObject data, int attempt)
      {
         var timer = new DispatcherTimer(DispatcherPriority.Background)
         {
            Interval = TimeSpan.FromMilliseconds(150)
         };
         timer.Tick += (s, e) =>
         {
            timer.Stop();
            if (!ReferenceEquals(pending_, data))
               return;   // a later copy has taken over, or the exit has flushed it

            if (Flushed_())
               pending_ = null;
            else if (attempt + 1 < FlushAttempts)
               FlushWhenFree_(data, attempt + 1);
         };
         timer.Start();
      }

      /// <summary>
      /// The last copy made, flushed now if it was not yet: called as the Control
      /// Panel exits, so that text copied a moment before it was closed - or while a
      /// watcher kept the clipboard open through every attempt - still pastes after
      /// (measured: without it, a copy made just before the exit was gone). Between
      /// attempts the messages other programs send this thread are answered - a
      /// watcher waiting for this program to render the text is one - and nothing
      /// posted to the Control Panel's own windows is taken off the queue.
      /// </summary>
      public static void FlushBeforeExit()
      {
         IDataObject data = pending_;
         pending_ = null;
         if (data == null)
            return;

         for (int attempt = 0; attempt < 10 && !Flushed_(); attempt++)
         {
            for (int i = 0; i < 10; i++)
            {
               PeekMessageW(out _, IntPtr.Zero, 0, 0, PmNoRemove);
               Thread.Sleep(10);
            }
         }
      }

      /// <summary>
      /// One flush: true when there is nothing more to do - the clipboard holds its
      /// own copy now, or what is on it is no longer this program's (OLE flushes only
      /// its own data object and leaves another program's copy alone: measured, a
      /// flush after another program had copied returned E_FAIL and left that copy
      /// on the clipboard). False only while the clipboard is held open elsewhere.
      /// Not Clipboard.IsCurrent: it retries with the thread asleep, as SetText does,
      /// and held a watcher waiting for this program for a second as the Control
      /// Panel exited.
      /// </summary>
      private static bool Flushed_()
      {
         int hr = OleFlushClipboard();
         return hr != ClipboardCantOpen && hr != ClipboardCantClose;
      }

      private const int ClipboardCantOpen = unchecked((int)0x800401D0);   // CLIPBRD_E_CANT_OPEN
      private const int ClipboardCantClose = unchecked((int)0x800401D4);  // CLIPBRD_E_CANT_CLOSE
      private const uint PmNoRemove = 0;

      [StructLayout(LayoutKind.Sequential)]
      private struct Msg
      {
         public IntPtr Hwnd;
         public uint Message;
         public IntPtr WParam;
         public IntPtr LParam;
         public uint Time;
         public int X;
         public int Y;
         public uint Private;
      }

      [LibraryImport("ole32.dll")]
      private static partial int OleFlushClipboard();

      [LibraryImport("user32.dll")]
      [return: MarshalAs(UnmanagedType.Bool)]
      private static partial bool PeekMessageW(out Msg msg, IntPtr hwnd, uint filterMin, uint filterMax, uint remove);
   }
}
