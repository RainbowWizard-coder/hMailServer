// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.IO;
using System.Linq;
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
      private LiveLogTail tail_ = new();
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
         tail_ = new LiveLogTail();
         tail_.StartAt(newest.Length);   // the last 64 KB
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
            // Whole lines only, and the end followed only while it is in view: what
            // is read and where the view goes are LiveLogTail's, which says why.
            using var stream = new FileStream(logFile_, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            IReadOnlyList<string> read = tail_.Read(stream);
            if (read.Count == 0)
               return;

            ScrollViewer scroller = Scroller_();
            bool atEnd = scroller == null || LiveLogTail.IsAtEnd(scroller.VerticalOffset, scroller.ScrollableHeight);
            double offset = scroller?.VerticalOffset ?? 0;

            foreach (string line in read)
               lines_.Add(new LogLine { Text = line, Brush = Classify(line) });

            int dropped = LiveLogTail.Trim(lines_, LiveLogTail.MaxLines);

            if (LiveLogTail.Follows(atEnd, read.Count))
            {
               if (LogList.Items.Count > 0)
                  LogList.ScrollIntoView(LogList.Items[LogList.Items.Count - 1]);
            }
            else if (scroller != null && LiveLogTail.OffsetAfterDrop(atEnd, offset, dropped) is double back)
            {
               scroller.ScrollToVerticalOffset(back);
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
