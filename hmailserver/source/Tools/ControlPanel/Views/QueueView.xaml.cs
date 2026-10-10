// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using hMailServer.ControlPanel.Services;
using System.Linq;
using MessageBox = hMailServer.ControlPanel.Views.Dialogs;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Views
{
   public partial class QueueView : UserControl, IPageLifecycle
   {
      public class QueueRow
      {
         public string Id { get; set; }

         // The two times as ServerTimeText shows them (review finding F321): how long
         // ago and how soon in a session on this computer, the reader's short date and
         // time under a "(server time)" header in one from another, the whole time
         // marked as the server's in each cell's tool tip, and the time itself to sort
         // by, where the cells were the server's raw "2026-10-08 02:36:53".
         public string Created { get; set; }
         public string CreatedTip { get; set; }
         public DateTime? CreatedAt { get; set; }
         public string From { get; set; }
         public string Recipients { get; set; }
         public string Tries { get; set; }
         public string File { get; set; }

         /// <summary>
         /// Where the message stands - Delivering now, Next try in 12 min - its next try's
         /// time as the State cell's tool tip, and the order the column sorts in
         /// (Services/QueueWords).
         /// </summary>
         public string State { get; set; }
         public string StateTip { get; set; }
         public DateTime StateOrder { get; set; }

         // What a screen reader says for the row: its cells, not the name of
         // this class, which is what a grid row is announced as otherwise.
         public override string ToString() => Id + ", " + From + ", " + Recipients + ", " + State;
      }

      public QueueView()
      {
         InitializeComponent();
         // Deliver now and Remove act on every row chosen; enabled while one is. View
         // source and Trace message are one message's, enabled while exactly one is
         // (QueueGrid_SelectionChanged).
         hMailServer.ControlPanel.Services.SelectionGate.Bind(QueueGrid, RetryButton, RemoveButton);
         QueueGrid_SelectionChanged(null, null);

         // Enter and F2 show the selected message's source and Delete removes it
         // after asking; a double-click on a message shows its source (Views/GridKeys).
         GridKeys.Wire(QueueGrid, ViewButton, RemoveButton, null);
      }

      public void OnEnter() => Reload();

      public void OnLeave()
      {
      }

      private void Refresh_Click(object sender, RoutedEventArgs e) => Reload();

      /// <summary>
      /// View source and Trace message while exactly one row is chosen, which each
      /// opens one message for; Deliver all now while the queue holds any.
      /// </summary>
      private void QueueGrid_SelectionChanged(object sender, SelectionChangedEventArgs e)
      {
         bool one = QueueGrid.SelectedItems.Count == 1;
         ViewButton.IsEnabled = one;
         TraceButton.IsEnabled = one;
      }

      /// <summary>The rows chosen, in the grid's order.</summary>
      private List<QueueRow> Chosen_() => QueueGrid.SelectedItems.OfType<QueueRow>().ToList();

      private void View_Click(object sender, RoutedEventArgs e)
      {
         if (QueueGrid.SelectedItems.Count != 1 || QueueGrid.SelectedItem is not QueueRow row)
            return;

         new MessageViewerDialog(Window.GetWindow(this), row.File).ShowDialog();
      }

      /// <summary>
      /// Trace message: every event of the chosen message, oldest first, on the Message
      /// trace page, which follows it by its queue id as its own Follow this message
      /// does (review finding F248): nothing led from a queued message to its trace.
      /// </summary>
      private void Trace_Click(object sender, RoutedEventArgs e)
      {
         if (QueueGrid.SelectedItems.Count == 1 && QueueGrid.SelectedItem is QueueRow row)
            Trace_(row);
      }

      /// <summary>The row's own Trace message icon, for the row it is on whatever is chosen.</summary>
      private void TraceRow_Click(object sender, RoutedEventArgs e)
      {
         if ((sender as FrameworkElement)?.DataContext is QueueRow row)
            Trace_(row);
      }

      /// <summary>
      /// The row icon's name and tool tip, "Trace message 1042", for the row it is on
      /// (the owner's decision 15) - set here, as the icon is loaded and whenever the
      /// grid gives it another row, rather than bound to a property of the row, which
      /// the page's search would read: "trace" and "message" matched every row.
      /// </summary>
      private void TraceRow_Named(object sender, RoutedEventArgs e) => NameTraceRow_(sender as FrameworkElement);

      private void TraceRow_DataContextChanged(object sender, DependencyPropertyChangedEventArgs e) => NameTraceRow_(sender as FrameworkElement);

      private static void NameTraceRow_(FrameworkElement button)
      {
         if (button?.DataContext is not QueueRow row)
            return;

         string name = TraceName(row.Id);
         button.ToolTip = name;
         AutomationProperties.SetName(button, name);
      }

      /// <summary>The name of a row's Trace message icon.</summary>
      internal static string TraceName(string id) => F("Trace message {0}", id);

      private static void Trace_(QueueRow row)
      {
         if (!long.TryParse(row.Id, out long id) || Application.Current?.MainWindow is not MainWindow main)
            return;

         main.NavigateTo("messagetrace"); // no-loc: a page key
         if (main.ContentHost.Content is MessageTraceView trace)
            trace.FollowQueuedMessage(id);
      }

      private void Retry_Click(object sender, RoutedEventArgs e) => Retry_(Chosen_());

      /// <summary>Deliver all now: every message in the queue, as Deliver now does for the rows chosen.</summary>
      private void RetryAll_Click(object sender, RoutedEventArgs e)
         => Retry_((QueueGrid.ItemsSource as IEnumerable<QueueRow>)?.ToList() ?? new List<QueueRow>());

      /// <summary>
      /// Deliver now for each message given, then one start of the delivery, and the
      /// list read again: it acted on the first of the rows chosen alone.
      /// </summary>
      private void Retry_(List<QueueRow> rows)
      {
         if (rows.Count == 0)
            return;

         try
         {
            dynamic queue = ServerSession.Current.Application.GlobalObjects.DeliveryQueue;
            try
            {
               // Message ids are 64-bit on the server; narrowing to Int32 threw
               // OverflowException on long-lived installations (Remove already used Int64).
               foreach (QueueRow row in rows)
                  queue.ResetDeliveryTime(Convert.ToInt64(row.Id));
               queue.StartDelivery();
            }
            finally
            {
               ServerSession.Release(queue);
            }
            Reload();
            ShowNotice_(StatusLevel.Good, QueueWords.Retried(rows.Select(row => row.Id).ToList()));
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            Reload();
            ShowNotice_(StatusLevel.Critical, F("Could not retrigger delivery: {0}", ServerSession.DescribeComError(ex)));
         }
      }

      /// <summary>
      /// Remove for every row chosen, after one question that counts them - or names the
      /// one - and says that none bounces: it asked about the first of five chosen rows
      /// and removed it alone.
      /// </summary>
      private void Remove_Click(object sender, RoutedEventArgs e)
      {
         List<QueueRow> rows = Chosen_();
         if (rows.Count == 0)
            return;

         // Each question with its own act, so that ConfirmationPatternTests reads both.
         bool confirmed = rows.Count == 1
            ? MessageBox.Confirm(Confirmations.RemoveQueuedMessage(rows[0].Id, rows[0].From, rows[0].Recipients), L("Control Panel"), L("_Remove message"), destructive: true)
            : MessageBox.Confirm(Confirmations.RemoveQueuedMessages(rows.Count), L("Control Panel"), L("_Remove messages"), destructive: true);
         if (!confirmed)
            return;

         var removed = new List<string>();
         string trying = rows[0].Id;
         try
         {
            dynamic queue = ServerSession.Current.Application.GlobalObjects.DeliveryQueue;
            try
            {
               foreach (QueueRow row in rows)
               {
                  trying = row.Id;
                  queue.Remove(Convert.ToInt64(row.Id));
                  removed.Add(row.Id);
               }
            }
            finally
            {
               ServerSession.Release(queue);
            }
            Reload();
            ShowNotice_(StatusLevel.Good, QueueWords.Removed(removed));
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            // What was removed before the refusal is said and is gone from the list
            // read again; the one refused is named with the server's reason, and it
            // and the rest are still in the list.
            Reload();
            ShowNotice_(StatusLevel.Critical, QueueWords.RemoveRefused(removed, trying, ServerSession.DescribeComError(ex)));
         }
      }

      private void Reload()
      {
         try
         {
            var snap = ServerSession.Current.ReadStatus(includeQueueRows: true);
            bool sameClock = ServerSession.IsLocalSession;
            DateTime now = DateTime.Now;

            var rows = new List<QueueRow>();

            // How long ago only where the server's clock is this computer's; the
            // header says whose clock the dates are otherwise (review finding F321).
            // The State column says the wait to the next try in its own words.
            DateTime? localNow = sameClock ? now : null;
            CreatedColumn.Header = localNow == null ? L("Created (server time)") : L("Created");
            CreatedColumn.MinWidth = Math.Max(110, GridStyles.HeaderWidth((string)CreatedColumn.Header));
            // No column narrower than its header, measured at the Windows text size: ID and
            // Tries keep their pixels as numbers, but at 150 per cent German cut "Versuche"
            // to "Vers…" (review finding F317).
            foreach (DataGridColumn column in QueueGrid.Columns)
               column.MinWidth = Math.Max(column.MinWidth, GridStyles.UpToDevicePixels(GridStyles.HeaderWidth(column.Header as string)));

            // Tab-separated: id, created, from, recipients, next try, file, locked, tries
            foreach (string[] columns in snap.QueueRows
                                            .Select(line => line.Split('\t'))
                                            .Where(c => c.Length >= 8))
            {
               // The server's locked flag, a message being delivered now, which the
               // page read and dropped (review finding F248).
               bool locked = columns[6] == "1";
               rows.Add(new QueueRow
               {
                  Id = columns[0],
                  Created = ServerTimeText.GridCell(columns[1], localNow),
                  CreatedTip = ServerTimeText.GridTip(columns[1]),
                  CreatedAt = ServerTimeText.Parse(columns[1]),
                  From = string.IsNullOrWhiteSpace(columns[2]) ? "<>" : columns[2],
                  Recipients = columns[3],
                  File = columns[5],
                  Tries = columns[7],
                  State = QueueWords.State(locked, columns[4], now, sameClock),
                  StateTip = QueueWords.StateTip(locked, columns[4]),
                  StateOrder = QueueWords.Order(locked, columns[4])
               });
            }

            QueueGrid.ItemsSource = rows;
            ReadFailure.Clear(Notice);
            StatusText.Show(EmptyStatus, Notice, rows.Count, null, EmptyText_);
            CountPill.Visibility = Visibility.Visible;
            ShowListState_();
            RetryAllButton.IsEnabled = rows.Count > 0;
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            // Neither the rows of the last read nor "The delivery queue is empty."
            // is a fact this page has now: both go, and the notice says what
            // happened, with Try again (Services/ReadFailure).
            ReadFailure.Show(Notice, EmptyStatus, QueueGrid, L("Could not read the queue: "), ex, Reload, RetryAllButton);
            CountPill.Visibility = Visibility.Collapsed;
         }
      }

      /// <summary>What the last action said, at its level: a confirmation in
      /// green, a failure in red, each with its shape and word.</summary>
      private void ShowNotice_(StatusLevel level, string text)
      {
         ReadFailure.Clear(Notice);
         Notice.Level = level;
         Notice.Text = text;
         Notice.Visibility = Visibility.Visible;
      }

      // What an empty queue means, said by a read that finds no message and by a
      // search cleared over none.
      private static string EmptyText_ => L("The delivery queue is empty.");

      private void Search_TextChanged(object sender, EventArgs e) => ShowListState_();

      /// <summary>
      /// The search over the messages read, the count beside the title and the empty
      /// state: "Shown: 1 of 4" while a search is typed in, and where it hides every
      /// row, the sentence that says so in place of the empty list's
      /// (Services/ListSearchState).
      /// </summary>
      private void ShowListState_() => ListSearchState.Show(QueueGrid, SearchBar.SearchText, CountPill,
         n => Plural(n, "{0} message", "{0} messages"), EmptyStatus, EmptyText_);
   }
}
