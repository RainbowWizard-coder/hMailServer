// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.IO;
using System.Linq;
using System.Text;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Threading;
using hMailServer.ControlPanel.Services;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Views
{
   public partial class LogsView : UserControl, IPageLifecycle
   {
      private const int MaxLines = 2000;

      public class LogLine
      {
         public string Text { get; set; }
         public Brush Brush { get; set; }

         // The log is a ListBox with an ItemTemplate, so a ListBoxItem's accessible
         // name falls back to ToString() on the bound object rather than the text
         // shown. Without this a screen reader announces the type name for every
         // line of the live log, which is the page an administrator is most likely
         // to be reading aloud when something is wrong.
         public override string ToString() => Text;
      }

      private readonly ObservableCollection<LogLine> lines_ = new();
      private readonly DispatcherTimer timer_;
      private string logFile_;
      private long position_;
      private bool startsMidLine_;
      private bool paused_;

      // Theme-aware, shared brush instances. Because each line holds a reference
      // to one of these, recolouring on a theme switch updates every visible
      // line live (see Services.ThemeTokens).
      private static Brush DefaultBrush => Services.ThemeTokens.LogDefault;
      private static Brush SmtpBrush => Services.ThemeTokens.LogSmtp;
      private static Brush ImapBrush => Services.ThemeTokens.LogImap;
      private static Brush Pop3Brush => Services.ThemeTokens.LogPop3;
      private static Brush ErrorBrush => Services.ThemeTokens.LogError;
      private static Brush AppBrush => Services.ThemeTokens.LogApp;

      public LogsView()
      {
         InitializeComponent();
         LogList.ItemsSource = lines_;

         // Copying the log (issue #48): Ctrl+C, and Copy on the list's menu, take the
         // selected lines. The lines are the list's rows, so they are chosen the way
         // rows are - click, Shift+click, Ctrl+click, Ctrl+A - and not as text inside
         // a row, which SelectableText leaves to the list for that reason.
         LogList.CommandBindings.Add(new CommandBinding(ApplicationCommands.Copy,
            (s, e) =>
            {
               CopySelection_();
               e.Handled = true;
            },
            (s, e) =>
            {
               e.CanExecute = LogList.SelectedItems.Count > 0;
               e.Handled = true;
            }));
         LogList.ContextMenu = SelectableText.CopyMenu(CopySelection_, () => LogList.SelectedItems.Count > 0, LogList.SelectAll);

         timer_ = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(750) };
         timer_.Tick += (s, e) => Poll();
      }

      public void OnEnter()
      {
         var store = new IniFeatureStore();
         string folder = store.GetLogFolder();

         if (folder == null || !Directory.Exists(folder))
         {
            Header.Subtitle = L("Log folder not found on this machine (live logs need a local server).");
            return;
         }

         var newest = new DirectoryInfo(folder).GetFiles("hmailserver_*.log")
            .OrderByDescending(f => f.LastWriteTimeUtc)
            .FirstOrDefault();

         if (newest == null)
         {
            Header.Subtitle = F("No log files in {0} yet. Enable logging in the server settings.", folder);
            return;
         }

         logFile_ = newest.FullName;
         position_ = Math.Max(0, newest.Length - 64 * 1024); // start with the last 64 KB
         startsMidLine_ = position_ > 0;
         Header.Subtitle = F("Streaming {0}", newest.Name);

         Poll();
         timer_.Start();
      }

      public void OnLeave() => timer_.Stop();

      private void Poll()
      {
         if (paused_ || logFile_ == null)
            return;

         try
         {
            using var stream = new FileStream(logFile_, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            if (stream.Length < position_)
            {
               position_ = 0; // rotated
               startsMidLine_ = false;
            }

            if (stream.Length == position_)
               return;

            // Whole lines only. The view read to the end of the file, so a line the
            // server was part-way through writing became two rows - and a copy of
            // them a line break the log does not have. The unfinished line is now
            // left for the next tick, and a tail begun in the middle of a line (the
            // last 64 KB) starts at the next one.
            long from = startsMidLine_ ? position_ - 1 : position_;
            stream.Seek(from, SeekOrigin.Begin);
            byte[] bytes = new byte[stream.Length - from];
            int count = ReadFully_(stream, bytes);
            int end = count > 0 ? Array.LastIndexOf(bytes, (byte)'\n', count - 1) : -1;
            if (end < 0)
               return;

            int start = 0;
            if (startsMidLine_)
               start = Array.IndexOf(bytes, (byte)'\n', 0, end + 1) + 1;   // the byte before a line's start is a '\n'
            else if (from == 0 && count >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF)
               start = 3;                                                      // a byte order mark, as StreamReader dropped
            startsMidLine_ = false;
            position_ = from + end + 1;
            string chunk = Encoding.UTF8.GetString(bytes, start, end + 1 - start);

            // The view follows the end of the log only while the end is in view. It
            // used to jump to the newest line on every tick, so on a busy server a
            // line could not be read, let alone selected and copied, without Pause:
            // somebody who scrolls up now keeps their place, and scrolling back to
            // the end follows again - as a terminal's tail does.
            ScrollViewer scroller = Scroller_();
            bool following = scroller == null || scroller.VerticalOffset >= scroller.ScrollableHeight - 0.5;
            double offset = scroller?.VerticalOffset ?? 0;

            bool added = false;
            foreach (string line in chunk.Split('\n')
                                        .Select(raw => raw.TrimEnd('\r'))
                                        .Where(l => l.Length != 0))
            {
               lines_.Add(new LogLine { Text = line, Brush = Classify(line) });
               added = true;
            }

            int dropped = 0;
            while (lines_.Count > MaxLines)
            {
               lines_.RemoveAt(0);
               dropped++;
            }

            if (following)
            {
               if (added && LogList.Items.Count > 0)
                  LogList.ScrollIntoView(LogList.Items[LogList.Items.Count - 1]);
            }
            else if (dropped > 0 && scroller != null)
            {
               // The list scrolls by line, so the lines dropped from the top would
               // otherwise slide the ones being read up by as many.
               scroller.ScrollToVerticalOffset(Math.Max(0, offset - dropped));
            }
         }
         catch (IOException)
         {
            // File momentarily locked; retry next tick.
         }
      }

      private static Brush Classify(string line)
      {
         if (line.Contains("\"ERROR\"") || line.Contains("Severity: 1") || line.Contains("Severity: 2"))
            return ErrorBrush;
         if (line.StartsWith("\"SMTPD\"") || line.Contains("\"SMTPD\""))
            return SmtpBrush;
         if (line.Contains("\"IMAPD\""))
            return ImapBrush;
         if (line.Contains("\"POP3D\""))
            return Pop3Brush;
         if (line.Contains("\"APPLICATION\""))
            return AppBrush;
         return DefaultBrush;
      }

      private void Pause_Click(object sender, RoutedEventArgs e)
      {
         paused_ = !paused_;
         PauseButton.Content = paused_ ? L("_Resume") : L("_Pause");
         PauseButtonIcon.Symbol = paused_ ? Wpf.Ui.Controls.SymbolRegular.Play24 : Wpf.Ui.Controls.SymbolRegular.Pause24;
      }

      private void Clear_Click(object sender, RoutedEventArgs e) => lines_.Clear();

      /// <summary>
      /// The selected lines as the log has them: in the log's order, whatever order
      /// they were picked in, each exactly as it was read (the viewer drops only the
      /// line ending), joined by CR LF - the log file's own line ending, and what
      /// every Windows editor pastes as one. No line ending after the last.
      /// </summary>
      internal static string TextOf(IEnumerable<LogLine> lines, IEnumerable selected)
      {
         // By reference: two lines with the same words are two lines.
         var chosen = new HashSet<object>(selected.Cast<object>(), ReferenceEqualityComparer.Instance);
         return string.Join("\r\n", lines.Where(line => chosen.Contains(line)).Select(line => line.Text));
      }

      private void CopySelection_() => SelectableText.SetClipboard(TextOf(lines_, LogList.SelectedItems));

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

      /// <summary>The list's own scroller, looked up each time: a template applied
      /// again (a theme change) makes a new one.</summary>
      private ScrollViewer Scroller_() => FirstScroller_(LogList);

      private static ScrollViewer FirstScroller_(DependencyObject node)
      {
         for (int i = 0; i < VisualTreeHelper.GetChildrenCount(node); i++)
         {
            DependencyObject child = VisualTreeHelper.GetChild(node, i);
            if (child is ScrollViewer scroller)
               return scroller;
            ScrollViewer inner = FirstScroller_(child);
            if (inner != null)
               return inner;
         }

         return null;
      }
   }
}
