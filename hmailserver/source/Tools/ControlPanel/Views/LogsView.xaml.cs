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
   public partial class LogsView : UserControl, IPageLifecycle, IRefreshablePage
   {
      public class LogLine
      {
         /// <summary>The line as the file has it: what a copy takes and what the line's colour is read from.</summary>
         public string Text { get; set; }
         public Brush Brush { get; set; }

         // The parts the line is shown in (Services/LiveLogFiles), the file's quotes
         // and tabs taken off.
         public string Time { get; set; } = "";
         public string Type { get; set; } = "";
         public string Thread { get; set; } = "";
         public string Session { get; set; } = "";
         public string Message { get; set; } = "";

         // The log is a ListBox with an ItemTemplate, so a ListBoxItem's accessible
         // name falls back to ToString() on the bound object rather than the text
         // shown. Without this a screen reader announces the type name for every
         // line of the live log, which is the page an administrator is most likely
         // to be reading aloud when something is wrong. The parts as shown, not the
         // file's quotes and tabs.
         public override string ToString()
            => string.Join(" ", new[] { Time, Type, Thread, Session, Message }.Where(part => !string.IsNullOrEmpty(part)));
      }

      private readonly ObservableCollection<LogLine> lines_ = new();
      private readonly DispatcherTimer timer_;
      private string logFile_;
      private LiveLogTail tail_ = new();
      private bool paused_;

      // A file there when the page opened it is read from its last 64 KB, which the
      // first read that opens it sets; one that appears later is read from its start.
      private bool startTail_;

      // The log folder, and which of its logs the picker has chosen.
      private string folder_;
      private LiveLogKind kind_ = LiveLogKind.Everything;
      private bool filling_;

      // Read while paused, so that Paused can say how many are waiting; shown on Resume.
      private readonly List<string> waiting_ = new();
      private DateTime pausedAt_;

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

         // hMailServer.INI and the log folder are the administrators' alone from
         // 6.3.6 (hMailServer.exe /ProtectFolders): a Control Panel not run as
         // administrator says it could not read them, not that there is no log
         // folder on this machine.
         string denied = store.ReadDenied();
         string folder = denied == null ? store.GetLogFolder() : null;

         // The protocols' own logs are offered while the server writes them apart.
         FillPicker_(denied == null && store.ReadBool("SepSvcLogs", false));
         Open_(denied, folder);
      }

      public void OnLeave() => timer_.Stop();

      /// <summary>The logs the picker offers, the chosen one kept where it is still offered.</summary>
      private void FillPicker_(bool separate)
      {
         var kinds = new List<(LiveLogKind Kind, string Caption)>
         {
            (LiveLogKind.Everything, separate ? L("Everything else") : L("Everything")),
            (LiveLogKind.Errors, L("Errors")),
            (LiveLogKind.Events, L("Script events")),
            (LiveLogKind.Backup, L("Backup and restore"))
         };
         if (separate)
         {
            kinds.Add((LiveLogKind.Smtp, L("SMTP conversations")));
            kinds.Add((LiveLogKind.Imap, L("IMAP conversations")));
            kinds.Add((LiveLogKind.Pop3, L("POP3 conversations")));
         }

         if (!kinds.Any(k => k.Kind == kind_))
            kind_ = LiveLogKind.Everything;

         filling_ = true;
         LogPicker.Items.Clear();
         foreach ((LiveLogKind kind, string caption) in kinds)
         {
            var item = new ComboBoxItem { Content = caption, Tag = kind };
            LogPicker.Items.Add(item);
            if (kind == kind_)
               LogPicker.SelectedItem = item;
         }
         filling_ = false;
      }

      private void LogPicker_SelectionChanged(object sender, SelectionChangedEventArgs e)
      {
         if (filling_ || LogPicker.SelectedItem is not ComboBoxItem { Tag: LiveLogKind kind } || kind == kind_)
            return;

         kind_ = kind;
         Open_(null, folder_);
      }

      /// <summary>
      /// Starts following the chosen log: today's file of it, by the server's own name,
      /// from its last 64 KB. What keeps it from being shown is said in the list's
      /// place, each with what to do, where it was the grey subtitle under the title,
      /// over an empty card (review finding F249). A file not there yet is waited for:
      /// the page starts showing it with the server's first line.
      /// </summary>
      private void Open_(string denied, string folder)
      {
         timer_.Stop();
         lines_.Clear();
         waiting_.Clear();
         logFile_ = null;
         folder_ = folder;
         Header.Subtitle = L("The server log as it is written, read from the log folder on this computer.");

         if (denied != null)
         {
            LogPicker.IsEnabled = false;
            Problem_(L("hMailServer.ini and the log folder belong to the administrators from 6.3.6, so this Control Panel may not read them. Close it and start it with Run as administrator to see the live log."), false);
            return;
         }

         // The folder is listed and the file opened, not looked for with Directory.Exists
         // and File.Exists: in a log folder that belongs to the administrators those find
         // no file, without an error, for a Control Panel not run as administrator, which
         // then said that nothing had been logged today (Services/LiveLogFiles.TryOpen).
         LiveLogAccess listing = LiveLogFiles.FolderAccess(folder);
         if (listing == LiveLogAccess.NotThere)
         {
            LogPicker.IsEnabled = false;
            Problem_(L("The log folder was not found on this computer. Live logs read the log files where the server runs: start the Control Panel on that computer to see them."), false);
            return;
         }

         LogPicker.IsEnabled = true;
         logFile_ = Path.Join(folder, LiveLogFiles.FileName(kind_, DateTime.Today));
         tail_ = new LiveLogTail();
         startTail_ = false;

         switch (LiveLogFiles.Access(logFile_))
         {
            case LiveLogAccess.Readable:
            case LiveLogAccess.Busy:
               startTail_ = true;   // the last 64 KB, from the first read
               Streaming_();
               break;
            case LiveLogAccess.NotThere when listing != LiveLogAccess.Denied:
               NotYet_();
               break;
            default:
               // The file may not be read, or the folder may not be listed and whether
               // the file is there cannot be told: either way they are the administrators'.
               Denied_();
               return;
         }

         Poll();
         timer_.Start();
      }

      /// <summary>What the page says while the chosen log has no file yet today, or ever.</summary>
      private void NotYet_()
      {
         string name = Path.GetFileName(logFile_);
         switch (kind_)
         {
            case LiveLogKind.Errors:
               Problem_(F("No errors have been logged today: there is no {0} in {1}.", name, folder_), false);
               break;
            case LiveLogKind.Events:
            case LiveLogKind.Backup:
               Problem_(F("Nothing has been written to {0} in {1} yet. Its lines appear here as soon as the server writes them.", name, folder_), false);
               break;
            default:
               Problem_(F("Nothing has been logged today: there is no {0} in {1} yet. Logging may be switched off; its lines appear here as soon as the server writes them.", name, folder_), true);
               break;
         }
      }

      /// <summary>A log file this Control Panel may not read, said once; it is not retried.</summary>
      private void Denied_()
      {
         timer_.Stop();
         lines_.Clear();
         Problem_(L("The log files belong to the administrators from 6.3.6, so this Control Panel may not read them. Close it and start it with Run as administrator to see the live log."), false);
      }

      /// <summary>The list's place says what keeps the log from being shown, with Logging's page where that is the cure.</summary>
      private void Problem_(string text, bool offerLogging)
      {
         // The subtitle says what the page is, not Streaming over a log it has stopped
         // reading (a folder made the administrators' while it was streamed).
         Header.Subtitle = L("The server log as it is written, read from the log folder on this computer.");
         Problem.Text = text;
         Problem.Action = offerLogging ? OpenLogging_() : null;
         Problem.Visibility = Visibility.Visible;
         LogList.Visibility = Visibility.Collapsed;
      }

      private void Streaming_()
      {
         Problem.Visibility = Visibility.Collapsed;
         Problem.Action = null;
         LogList.Visibility = Visibility.Visible;
         SayState_();
      }

      private static Wpf.Ui.Controls.Button OpenLogging_()
      {
         var button = new Wpf.Ui.Controls.Button { Content = L("_Open Logging") };
         System.Windows.Automation.AutomationProperties.SetAutomationId(button, "logs-open-logging");
         button.Click += (_, _) => (Application.Current?.MainWindow as MainWindow)?.NavigateTo("logging");
         return button;
      }

      /// <summary>
      /// The subtitle: the file followed, or, while paused, since when and how many
      /// lines are waiting - where Pause left "Streaming" standing.
      /// </summary>
      private void SayState_()
      {
         if (logFile_ == null || Problem.Visibility == Visibility.Visible)
            return;

         string name = Path.GetFileName(logFile_);
         if (!paused_)
            Header.Subtitle = F("Streaming {0}", name);
         else if (waiting_.Count == 0)
            Header.Subtitle = F("Paused at {0}: no new lines in {1} yet.", pausedAt_.ToShortTimeString(), name);
         else if (waiting_.Count == 1)
            Header.Subtitle = F("Paused at {0}: 1 new line of {1} is waiting.", pausedAt_.ToShortTimeString(), name);
         else
            Header.Subtitle = F("Paused at {0}: {1} new lines of {2} are waiting.", pausedAt_.ToShortTimeString(), waiting_.Count, name);
      }

      /// <summary>
      /// F5: the newest log from its last 64 KB, as opening the page reads it -
      /// the file of a new day once the server has moved to it - in place of the
      /// lines shown, which opening the page again would have shown twice.
      /// </summary>
      void IRefreshablePage.Refresh()
      {
         timer_.Stop();
         lines_.Clear();
         OnEnter();
      }

      private void Poll()
      {
         if (logFile_ == null)
            return;

         try
         {
            // The day turned over: the server opens the next day's file with its first
            // line after midnight. The rest of the old one is read first, then a line
            // says where the log goes on, and the new file is read from its start. A
            // next file there but not to be read is there: opening it says so below.
            string next = LiveLogFiles.NextDay(logFile_, kind_, DateTime.Today, path => LiveLogFiles.Access(path) != LiveLogAccess.NotThere);
            if (next != null)
            {
               if (Problem.Visibility != Visibility.Visible && LiveLogFiles.TryOpen(logFile_, out FileStream old) == LiveLogAccess.Readable)
               {
                  using (old)
                     Show_(tail_.Read(old));
                  Show_(new[] { F("… continued in {0}", Path.GetFileName(next)) });
               }

               logFile_ = next;
               tail_ = new LiveLogTail();
               startTail_ = false;
               Streaming_();
            }

            // Opened, not looked for with File.Exists, which in a folder made the
            // administrators' alone while it was streamed (hMailServer.exe
            // /ProtectFolders) finds no file and no error, so that the page said
            // Streaming over a log it could no longer read.
            switch (LiveLogFiles.TryOpen(logFile_, out FileStream stream))
            {
               case LiveLogAccess.NotThere:
               case LiveLogAccess.Busy:
                  return;   // not written yet, or held for a moment: looked at again next tick
               case LiveLogAccess.Denied:
                  Denied_();   // said once, not retried
                  return;
            }

            using (stream)
            {
               // The file the page was waiting for has been written: shown from its start.
               if (Problem.Visibility == Visibility.Visible)
               {
                  tail_ = new LiveLogTail();
                  startTail_ = false;
                  Streaming_();
               }
               else if (startTail_)
               {
                  tail_.StartAt(stream.Length);   // the last 64 KB
                  startTail_ = false;
               }

               // Whole lines only, and the end followed only while it is in view: what
               // is read and where the view goes are LiveLogTail's, which says why.
               Show_(tail_.Read(stream));
            }
         }
         catch (IOException)
         {
            // A read that failed part-way: tried again on the next tick.
         }
         catch (UnauthorizedAccessException)
         {
            Denied_();
         }
      }

      /// <summary>
      /// Lines into the view, or, while paused, into the lines waiting, which the
      /// subtitle counts; the end followed only while it is in view (LiveLogTail).
      /// </summary>
      private void Show_(IReadOnlyList<string> read)
      {
         if (read.Count == 0)
            return;

         if (paused_)
         {
            waiting_.AddRange(read);
            LiveLogTail.Trim(waiting_, LiveLogTail.MaxLines);
            SayState_();
            return;
         }

         ScrollViewer scroller = Scroller_();
         bool atEnd = scroller == null || LiveLogTail.IsAtEnd(scroller.VerticalOffset, scroller.ScrollableHeight);
         double offset = scroller?.VerticalOffset ?? 0;

         foreach (string line in read)
            lines_.Add(Line_(line));

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

      private LogLine Line_(string line)
      {
         LiveLogFiles.Parts parts = LiveLogFiles.Split(line, kind_);
         return new LogLine
         {
            Text = line,
            Brush = Classify(line),
            Time = parts.Time,
            Type = parts.Type,
            Thread = parts.Thread,
            Session = parts.Session,
            Message = parts.Text
         };
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
         PauseButton.Content = paused_ ? L("R_esume view") : L("_Pause view");
         PauseButtonIcon.Symbol = paused_ ? Wpf.Ui.Controls.SymbolRegular.PlayCircle24 : Wpf.Ui.Controls.SymbolRegular.PauseCircle24;

         if (paused_)
         {
            pausedAt_ = DateTime.Now;
         }
         else if (waiting_.Count > 0)
         {
            string[] held = waiting_.ToArray();
            waiting_.Clear();
            Show_(held);
         }

         SayState_();
      }

      // The view only: the file is the server's and is not changed (the button's
      // tool tip says so).
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
