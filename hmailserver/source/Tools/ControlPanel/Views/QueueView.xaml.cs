// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Windows;
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
         public string Created { get; set; }
         public string From { get; set; }
         public string Recipients { get; set; }
         public string NextTry { get; set; }
         public string Tries { get; set; }
         public string File { get; set; }

         // What a screen reader says for the row: its cells, not the name of
         // this class, which is what a grid row is announced as otherwise.
         public override string ToString() => Id + ", " + From + ", " + Recipients;
      }

      public QueueView()
      {
         InitializeComponent();
         // The three act on the selected message; enabled only while one is selected.
         hMailServer.ControlPanel.Services.SelectionGate.Bind(QueueGrid, ViewButton, RetryButton, RemoveButton);

         // Enter and F2 show the selected message's source and Delete removes it
         // after asking; a double-click on a message shows its source (Views/GridKeys).
         GridKeys.Wire(QueueGrid, ViewButton, RemoveButton, null);
      }

      public void OnEnter() => Reload();

      public void OnLeave()
      {
      }

      private void Refresh_Click(object sender, RoutedEventArgs e) => Reload();

      private void View_Click(object sender, RoutedEventArgs e)
      {
         if (QueueGrid.SelectedItem is not QueueRow row)
         {
            ShowNotice_(StatusLevel.Information, L("Select a message first."));
            return;
         }

         new MessageViewerDialog(Window.GetWindow(this), row.File).ShowDialog();
      }

      private void Retry_Click(object sender, RoutedEventArgs e)
      {
         if (QueueGrid.SelectedItem is not QueueRow row)
         {
            ShowNotice_(StatusLevel.Information, L("Select a message first."));
            return;
         }

         try
         {
            dynamic queue = ServerSession.Current.Application.GlobalObjects.DeliveryQueue;
            // Message ids are 64-bit on the server; narrowing to Int32 threw
            // OverflowException on long-lived installations (Remove already used Int64).
            queue.ResetDeliveryTime(Convert.ToInt64(row.Id));
            queue.StartDelivery();
            ServerSession.Release(queue);
            Reload();
            ShowNotice_(StatusLevel.Good, F("Delivery retriggered for message {0}.", row.Id));
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            ShowNotice_(StatusLevel.Critical, F("Could not retrigger delivery: {0}", ServerSession.DescribeComError(ex)));
         }
      }

      private void Remove_Click(object sender, RoutedEventArgs e)
      {
         if (QueueGrid.SelectedItem is not QueueRow row)
         {
            ShowNotice_(StatusLevel.Information, L("Select a message first."));
            return;
         }

         if (!MessageBox.Confirm(Confirmations.RemoveQueuedMessage(row.Id, row.From, row.Recipients), L("Control Panel"), L("_Remove message"), destructive: true))
            return;

         try
         {
            dynamic queue = ServerSession.Current.Application.GlobalObjects.DeliveryQueue;
            queue.Remove(Convert.ToInt64(row.Id));
            ServerSession.Release(queue);
            Reload();
            ShowNotice_(StatusLevel.Good, F("Message {0} removed.", row.Id));
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            ShowNotice_(StatusLevel.Critical, F("Could not remove the message: {0}", ServerSession.DescribeComError(ex)));
         }
      }

      private void Reload()
      {
         try
         {
            var snap = ServerSession.Current.ReadStatus(includeQueueRows: true);

            var rows = new List<QueueRow>();
            // Tab-separated: id, created, from, recipients, next try, file, locked, tries
            foreach (string[] columns in snap.QueueRows
                                            .Select(line => line.Split('\t'))
                                            .Where(c => c.Length >= 8))
            {

               rows.Add(new QueueRow
               {
                  Id = columns[0],
                  Created = columns[1],
                  From = string.IsNullOrWhiteSpace(columns[2]) ? "<>" : columns[2],
                  Recipients = columns[3],
                  NextTry = columns[4],
                  File = columns[5],
                  Tries = columns[7]
               });
            }

            QueueGrid.ItemsSource = rows;
            ReadFailure.Clear(Notice);
            StatusText.Show(EmptyStatus, Notice, rows.Count, null, EmptyText_);
            CountPill.Visibility = Visibility.Visible;
            ShowListState_();
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            // Neither the rows of the last read nor "The delivery queue is empty."
            // is a fact this page has now: both go, and the notice says what
            // happened, with Try again (Services/ReadFailure).
            ReadFailure.Show(Notice, EmptyStatus, QueueGrid, L("Could not read the queue: "), ex, Reload);
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
