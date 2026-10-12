// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Windows;
using System.Windows.Controls;
using hMailServer.ControlPanel.Services;
using hMailServer.ControlPanel.Views.Scaffold;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Views
{
   /// <summary>
   /// "What happened to the message Jane sent at 14:20."
   ///
   /// That question used to be answered by grepping several log files and hoping the
   /// relevant one had not rotated. The events were never missing - the server has
   /// called its delivery journal from every interesting site for years - they just
   /// went to a text file shaped for a web-statistics tool.
   ///
   /// So this page is a search box, because a search is the shape of the question.
   /// One address in, matched against sender or recipient, newest first; then a
   /// second click on any row to pull together every event for that one message.
   ///
   /// It deliberately does not try to be a dashboard. There is no count of everything
   /// and no time-series, because nobody comes here to browse - they come with an
   /// address and a complaint.
   /// </summary>
   public class MessageTraceView : UserControl, IPageLifecycle, IRefreshablePage
   {
      // The last search, an address or a followed message, which Refresh and F5
      // run again.
      private Action rerun_;

      // The header's Refresh, as on every list page (docs/ControlPanelDesign.md,
      // List pages): it can be pressed once there is a search to run again.
      private Button refresh_;

      /// <summary>
      /// The toolbar's search box is the address: the search runs on Enter or the
      /// Search button, not on every keystroke, because it is a query to the
      /// server rather than a filter over rows already here.
      /// </summary>
      private readonly Toolbar toolbar_ = new() { SearchPlaceholder = L("Address") };

      /// <summary>
      /// A real grid, because the rows are a table and were being drawn as one.
      ///
      /// This was a ListBox of monospaced strings padded to fixed widths - the
      /// addresses to 34 characters each. Every address longer than its column
      /// pushed the rest of that row out of line, and mail addresses longer than 34
      /// characters are not an edge case. It also had no headers, so the reader had
      /// to work out what the columns were; nothing could be sorted; a single field
      /// could not be copied without the padding around it; and a screen reader read
      /// each row as one run-on line with the alignment spaces in it.
      ///
      /// The column styling is the application's, from the implicit DataGrid styles
      /// in App.xaml, so this page did not need any appearance code of its own.
      /// </summary>
      private readonly DataGrid list_ = new()
      {
         AutoGenerateColumns = false,
         IsReadOnly = true,
         CanUserAddRows = false,
         CanUserDeleteRows = false,
         SelectionMode = DataGridSelectionMode.Single
      };

      private readonly InlineNotice status_ = new() { Visibility = Visibility.Collapsed };

      // How many events the search or the followed message has, beside the title
      // (review finding F288); none until a query has worked.
      private readonly StatusPill count_ = new() { Level = StatusLevel.Normal, Visibility = Visibility.Collapsed };

      // MessageTraceRetentionDays, read over COM when there is a session and from
      // hMailServer.ini otherwise, for Remove expired: 0 keeps every event.
      private readonly IniFeatureStore iniStore_ = new();
      private Button removeExpired_;

      // What the page shows before the first search: what to do, in the grid's
      // place, where an Information notice said it over a blank grid of headers
      // whatever the setting was (review finding F253).
      private readonly EmptyState empty_ = new() { Icon = Wpf.Ui.Controls.SymbolRegular.DocumentSearch24 };

      // Whether the trace records anything: a Warning with the page that turns it
      // on while MessageTraceEnabled is off, and how long events are kept while it
      // is on, read as the page opens (ShowTracingState_).
      private readonly InlineNotice tracingOff_ = new() { Level = StatusLevel.Warning, Visibility = Visibility.Collapsed };
      private readonly TextBlock keptFor_ = new() { Visibility = Visibility.Collapsed, TextWrapping = TextWrapping.Wrap };

      // The results a followed message was opened from, which Back to results puts
      // back as they were, the selected event with them: following one replaced the
      // address's results with no way back but searching again.
      private Button back_;
      private object backCaption_;
      private System.Collections.Generic.List<TraceRow> resultRows_;
      private int resultSelected_ = -1;
      private string resultsFor_;
      private bool following_;

      public MessageTraceView()
      {
         Build();
         empty_.Text = L("Enter an address and press Search to see every event of the mail it sent or received.");
      }

      /// <summary>
      /// Whether message tracing is on and how long its events are kept, read as the
      /// page opens: a setting changed on the Logging page since is said here when the
      /// page is next opened.
      /// </summary>
      public void OnEnter()
      {
         ShowTracingState_();
         ShowSweepState_(RetentionDays_());
      }

      public void OnLeave()
      {
      }

      /// <summary>
      /// Off - nothing is recorded - says so as a Warning with the Logging page that
      /// turns it on; on, a caption says how long events are kept. Neither while the
      /// setting could not be read at all, which is no fact about the server.
      /// </summary>
      private void ShowTracingState_()
      {
         iniStore_.ForgetFallback();
         string enabled = iniStore_.Read("MessageTraceEnabled", "0").Trim();
         bool unknown = iniStore_.FallbackCause != null && !iniStore_.IsAvailable;

         tracingOff_.Visibility = !unknown && enabled != "1" ? Visibility.Visible : Visibility.Collapsed;
         keptFor_.Text = MessageTraceStatus.KeptFor(RetentionDays_());
         keptFor_.Visibility = !unknown && enabled == "1" ? Visibility.Visible : Visibility.Collapsed;
      }

      private static Wpf.Ui.Controls.Button OpenLogging_()
      {
         var button = new Wpf.Ui.Controls.Button { Content = L("_Open Logging") };
         System.Windows.Automation.AutomationProperties.SetAutomationId(button, "messagetrace-open-logging");
         button.Click += (_, _) => (Application.Current?.MainWindow as MainWindow)?.NavigateTo("logging");
         return button;
      }

      private dynamic OpenTrace()
      {
         dynamic globalObjects = ServerSession.Current.Application.GlobalObjects;
         dynamic trace = globalObjects.MessageTrace;

         ServerSession.Release(globalObjects);

         return trace;
      }

      private void Fill(dynamic trace, string emptyMessage)
      {
         var rows = new System.Collections.Generic.List<TraceRow>();

         int count = (int)trace.Count;

         for (int i = 0; i < count; i++)
         {
            dynamic item = trace[i];

            try
            {
               rows.Add(new TraceRow
               {
                  OccurredTime = (string)item.OccurredTime,
                  EventName = (string)item.EventName,
                  Sender = (string)item.Sender,
                  Recipient = (string)item.Recipient,
                  StatusCode = (int)item.StatusCode,
                  QueueId = (int)item.QueueID
               });
            }
            finally
            {
               ServerSession.Release(item);
            }
         }

         list_.ItemsSource = rows;
         StatusText.Show(empty_, null, count, null, emptyMessage);

         // How many events the search found, beside the title, where an
         // Information notice said it (review finding F288).
         status_.Visibility = Visibility.Collapsed;
         count_.Text = Plural(count, "{0} event", "{0} events");
         count_.Visibility = Visibility.Visible;
      }

      /// <summary>
      /// One row of the grid.
      ///
      /// The COM objects behind these are released as soon as they are read - the
      /// trace collection is a snapshot and holding a few hundred live references
      /// across a user's browsing is how a page keeps a server object alive for an
      /// afternoon - so the values are copied out rather than bound through.
      /// </summary>
      private sealed class TraceRow
      {
         // As the server wrote it, its own local time.
         public string OccurredTime { get; init; }

         // The reader's short date and the time to the second, under a header that
         // names the server's clock, where the cell was the server's "2026-10-10
         // 20:26:26" under "Time" (review finding F321); sorted by the time itself, and
         // whole, marked as the server's, in the cell's tool tip.
         public string Time => ServerTimeText.CellWithSeconds(OccurredTime);
         public DateTime? OccurredAt => ServerTimeText.Parse(OccurredTime);
         public string TimeTip => ServerTimeText.GridTip(OccurredTime);
         public string EventName { get; init; }
         public string Sender { get; init; }
         public string Recipient { get; init; }
         public int StatusCode { get; init; }

         /// <summary>The status as its word and its code, "Deferred 451", and the level its mark is drawn at (Services/MessageTraceStatus).</summary>
         public string StatusText => MessageTraceStatus.TextOf(StatusCode);
         public StatusLevel StatusLevel => MessageTraceStatus.LevelOf(StatusCode);
         public Visibility StatusShown => StatusText.Length > 0 ? Visibility.Visible : Visibility.Collapsed;

         /// <summary>
         /// 0 means the event happened before a queue entry existed - a refusal at
         /// RCPT - so there is nothing to follow and no story to open.
         /// </summary>
         public int QueueId { get; init; }

         public string FollowHint => QueueId == 0
            ? L("Refused before the message was queued, so there is no message to follow.")
            : F("Queue ID {0} - select and choose \"Follow this message\".", QueueId);

         // The time and the event, then the sender, the recipient and the status
         // with their columns' names.
         public override string ToString() => RowSpeech.Of(Time, EventName,
            RowSpeech.Labelled(L("Sender"), Sender), RowSpeech.Labelled(L("Recipient"), Recipient),
            RowSpeech.Labelled(L("Status"), StatusText));
      }

      /// <summary>What the last search said, at its level.</summary>
      private void Status_(StatusLevel level, string text)
      {
         status_.Level = level;
         status_.Text = text;
         status_.Visibility = Visibility.Visible;
      }

      /// <summary>F5: the last search again, for what has happened since; nothing before the first.</summary>
      void IRefreshablePage.Refresh() => rerun_?.Invoke();

      /// <summary>The search or the followed message that Refresh and F5 run again, which Refresh waits for.</summary>
      private void Remember_(Action rerun)
      {
         rerun_ = rerun;
         if (refresh_ != null)
            refresh_.IsEnabled = true;
      }

      private void Search() => Search_(toolbar_.SearchText.Trim());

      private void Search_(string address)
      {
         Remember_(() => Search_(address));
         count_.Visibility = Visibility.Collapsed;
         following_ = false;
         resultRows_ = null;
         resultsFor_ = address;
         back_.Visibility = Visibility.Collapsed;
         try
         {
            dynamic trace = OpenTrace();
            try
            {
               trace.Search(address);
               Fill(trace, L("No events for that address. Either nothing has happened to it, or the trace was switched off at the time - it records only while message tracing is on (Logging page)."));
            }
            finally
            {
               ServerSession.Release(trace);
            }
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            Status_(StatusLevel.Critical, ServerSession.DescribeComError(ex));
         }
      }

      private void FollowSelected()
      {
         if (list_.SelectedItem is not TraceRow selected)
         {
            Status_(StatusLevel.Information, L("Select an event first."));
            return;
         }

         int queueId = selected.QueueId;

         if (queueId == 0)
         {
            Status_(StatusLevel.Information, L("That event happened before the message was queued - a refusal during the SMTP conversation - so there is no message to follow."));
            return;
         }

         Follow_(queueId);
      }

      /// <summary>
      /// Every event of one queued message, oldest first, as Follow this message shows
      /// them: the delivery queue's Trace message (review finding F248). The trace keeps
      /// queue ids the server's COM interface takes as 32-bit; a larger one is not
      /// followed, and the page says so and what to search for instead, with the last
      /// search's events taken away so that they are not read as this message's - where
      /// it was left showing whatever it showed before, without a word. Refresh says it
      /// again.
      /// </summary>
      public void FollowQueuedMessage(long queueId)
      {
         if (QueueWords.CanFollow(queueId))
         {
            Follow_((int)queueId);
            return;
         }

         if (queueId <= int.MaxValue)
            return;

         Remember_(() => FollowQueuedMessage(queueId));
         list_.ItemsSource = null;
         empty_.Visibility = Visibility.Collapsed;
         count_.Visibility = Visibility.Collapsed;
         Status_(StatusLevel.Information, QueueWords.CannotFollow(queueId));
      }

      private void Follow_(int queueId)
      {
         Remember_(() => Follow_(queueId));
         count_.Visibility = Visibility.Collapsed;

         // The results the message is followed from, kept for Back to results; a
         // message followed from a followed message goes back to the same results.
         if (!following_ && list_.ItemsSource is System.Collections.Generic.List<TraceRow> results)
         {
            resultRows_ = results;
            resultSelected_ = list_.SelectedIndex;
         }

         try
         {
            dynamic trace = OpenTrace();
            try
            {
               trace.SearchByQueueID(queueId);
               Fill(trace, L("No events for that message."));
               Status_(StatusLevel.Information, F("Every event for queue id {0}, oldest first.", queueId));
               following_ = true;
               ShowBack_();
            }
            finally
            {
               ServerSession.Release(trace);
            }
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            Status_(StatusLevel.Critical, ServerSession.DescribeComError(ex));
         }
      }

      /// <summary>Back to results shows whose results it goes back to, while a message is followed from them.</summary>
      private void ShowBack_()
      {
         if (resultRows_ == null)
         {
            back_.Visibility = Visibility.Collapsed;
            return;
         }

         // The address goes into an access-key caption: its own underscores are doubled,
         // so that "j_doe@example.com" is shown as it is and B stays the key.
         back_.Content = string.IsNullOrEmpty(resultsFor_)
            ? backCaption_
            : F("_Back to results for {0}", resultsFor_.Replace("_", "__"));
         back_.Visibility = Visibility.Visible;
      }

      /// <summary>
      /// The address's results again, as they were when a message was followed from
      /// them, the event it was followed from selected; Refresh then runs the search
      /// again.
      /// </summary>
      private void BackToResults_()
      {
         if (resultRows_ == null)
            return;

         string address = resultsFor_;
         list_.ItemsSource = resultRows_;
         StatusText.Show(empty_, null, resultRows_.Count, null, L("No events for that address. Either nothing has happened to it, or the trace was switched off at the time - it records only while message tracing is on (Logging page)."));
         count_.Text = Plural(resultRows_.Count, "{0} event", "{0} events");
         count_.Visibility = Visibility.Visible;
         status_.Visibility = Visibility.Collapsed;
         Remember_(() => Search_(address));
         following_ = false;
         back_.Visibility = Visibility.Collapsed;

         if (resultSelected_ >= 0 && resultSelected_ < resultRows_.Count)
         {
            list_.SelectedIndex = resultSelected_;
            list_.Dispatcher.BeginInvoke(System.Windows.Threading.DispatcherPriority.Loaded, new Action(() => GridKeys.FocusSelectedRow(list_)));
         }

         resultRows_ = null;
      }

      /// <summary>MessageTraceRetentionDays; 30, the server's default, when it cannot be read or parsed.</summary>
      private int RetentionDays_()
      {
         try
         {
            return int.TryParse(iniStore_.Read("MessageTraceRetentionDays", "30").Trim(), out int days) ? days : 30;
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            return 30;
         }
      }

      /// <summary>Remove expired can be pressed while there is a retention window, and says why not otherwise.</summary>
      private void ShowSweepState_(int days)
      {
         if (removeExpired_ == null)
            return;

         removeExpired_.IsEnabled = days > 0;
         removeExpired_.ToolTip = days > 0 ? null : Confirmations.NothingExpires;
         ToolTipService.SetShowOnDisabled(removeExpired_, true);
      }

      /// <summary>
      /// Deletes every event older than the retention window, after asking, with the
      /// window named and Cancel the answer Enter gives, where it ran at the press
      /// (review finding F184); with a window of 0, which keeps everything, it asks
      /// nothing and says so.
      /// </summary>
      private void SweepExpired()
      {
         int days = RetentionDays_();
         ShowSweepState_(days);
         if (days <= 0)
         {
            Status_(StatusLevel.Information, Confirmations.NothingExpires);
            return;
         }

         // A Cancel answers the press: the line kept the last sweep's outcome, so that a
         // cancelled second press read as a second removal.
         if (!Dialogs.Confirm(Confirmations.DeleteExpiredTrace(days), L("Message trace"), L("_Delete"), destructive: true))
         {
            Status_(StatusLevel.Information, L("Nothing was changed."));
            return;
         }

         try
         {
            dynamic trace = OpenTrace();
            try
            {
               int removed = (int)trace.DeleteExpired();

               if (removed == 0)
                  Status_(StatusLevel.Information, L("Nothing was old enough to remove. The window is set on the Logging page (Delete trace events after), and 0 means never."));
               else
                  Status_(StatusLevel.Good, Plural(removed, "{0} event past the retention window was removed.", "{0} events past the retention window were removed."));
            }
            finally
            {
               ServerSession.Release(trace);
            }
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            Status_(StatusLevel.Critical, ServerSession.DescribeComError(ex));
         }
      }

      private void Build()
      {
         var root = new Grid();
         root.SetResourceReference(MarginProperty, "AppPagePadding");
         root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
         root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });

         // The count is a polite live region, so that a screen reader hears it as
         // it changes, as the notice it replaced was heard (Scaffold/StatusPill).
         System.Windows.Automation.AutomationProperties.SetLiveSetting(count_, System.Windows.Automation.AutomationLiveSetting.Polite);

         // Refresh in the header, as on every list page, where the page had F5
         // alone: the last search again, for what has happened since. Before the
         // first there is nothing to run again, and it cannot be pressed.
         refresh_ = MakeButton(L("_Refresh"), Wpf.Ui.Controls.ControlAppearance.Secondary, (_, _) => rerun_?.Invoke());
         refresh_.IsEnabled = rerun_ != null;
         root.Children.Add(new PageHeader
         {
            Status = count_,
            Actions = refresh_,
            PageKey = "messagetrace",
            Subtitle = L("What happened to a particular message. Search by any address - it matches senders and recipients - then follow one row to see every event for that message in order.")
         });

         // Enter in the address box searches, as it always did.
         toolbar_.PreviewKeyDown += (_, e) => { if (e.Key == System.Windows.Input.Key.Enter) Search(); };

         var filters = new StackPanel { Orientation = Orientation.Horizontal };
         filters.Children.Add(MakeButton(L("_Search"), Wpf.Ui.Controls.ControlAppearance.Primary, (_, _) => Search()));

         // While a message is followed: back to the results it was followed from,
         // in words, as every toolbar button is (docs/ControlPanelDesign.md, List pages).
         back_ = MakeButton(L("_Back to the results"), Wpf.Ui.Controls.ControlAppearance.Secondary, (_, _) => BackToResults_());
         backCaption_ = back_.Content;
         back_.Visibility = Visibility.Collapsed;
         System.Windows.Automation.AutomationProperties.SetAutomationId(back_, "messagetrace-back");
         filters.Children.Add(back_);
         toolbar_.Filters = filters;

         var actions = new StackPanel { Orientation = Orientation.Horizontal };
         var follow = MakeButton(L("_Follow this message"), Wpf.Ui.Controls.ControlAppearance.Secondary, (_, _) => FollowSelected());
         actions.Children.Add(follow);
         hMailServer.ControlPanel.Services.SelectionGate.Bind(list_, follow);

         // Enter, F2 and a double-click on an event follow its message, as its
         // button does (Views/GridKeys), where they did nothing.
         GridKeys.Wire(list_, follow, null, null);
         removeExpired_ = MakeButton(L("Remove _expired"), Wpf.Ui.Controls.ControlAppearance.Secondary, (_, _) => SweepExpired());
         ButtonStyles.DangerText(removeExpired_);
         actions.Children.Add(removeExpired_);
         toolbar_.Actions = actions;

         // The suppression list's shape: the toolbar, the notice, the list and the
         // selected event's detail under the header, the list taking the height the
         // others leave, down to three rows, and below that the page scrolls - so
         // that the detail never takes the list's height in a short window.
         ScrollViewer scroll = FillPanel.ScrollingPage(out FillPanel parts);
         Grid.SetRow(scroll, 1);
         root.Children.Add(scroll);
         parts.Children.Add(toolbar_);

         // Whether anything is recorded, under the toolbar: a Warning while tracing
         // is off, the retention while it is on.
         tracingOff_.Text = L("Message tracing is off - nothing is being recorded.");
         tracingOff_.Content = OpenLogging_();
         tracingOff_.SetResourceReference(MarginProperty, "AppCardGap");
         System.Windows.Automation.AutomationProperties.SetAutomationId(tracingOff_, "messagetrace-off");
         parts.Children.Add(tracingOff_);
         keptFor_.SetResourceReference(StyleProperty, "TextCaption");
         keptFor_.Margin = new Thickness(0, 0, 0, 8);
         System.Windows.Automation.AutomationProperties.SetAutomationId(keptFor_, "messagetrace-kept");
         parts.Children.Add(keptFor_);
         parts.Children.Add(status_);

         BuildColumns();
         System.Windows.Automation.AutomationProperties.SetName(list_, L("Message trace"));

         var host = new Grid();
         host.Children.Add(list_);
         host.Children.Add(empty_);
         var card = new Card { Padding = new Thickness(DesignTokens.ListCardPadding), Content = host };
         parts.Children.Add(FillPanel.Filling(card, FillPanel.ListCardFloor));

         // Whether the selected event can be followed - a refusal at RCPT has no
         // message to follow - which only the row's tool tip said (Views/RowDetailStrip).
         parts.Children.Add(RowDetailStrip.For(list_, null, row => ((TraceRow)row).FollowHint));

         Content = root;
      }

      /// <summary>
      /// The columns, in the order the question is asked: when, what happened, who
      /// to whom, and what the server said.
      ///
      /// The two address columns are the ones that used to be padded to a fixed 34
      /// characters and are now star-sized, so a long address takes the room it
      /// needs and the reader can drag the divider if it needs more. Time, event and
      /// status size to their contents, because none of them varies much and giving
      /// them a share of the width takes it from the addresses, which is where the
      /// variation actually is.
      /// </summary>
      private void BuildColumns()
      {
         // When each event happened by the server's clock, as the audit trail's When
         // says it; the whole time is the cell's tool tip, which the row's own tip
         // stands behind.
         var timeTip = new Style(typeof(TextBlock), DataGridTextColumn.DefaultElementStyle);
         timeTip.Setters.Add(new Setter(ToolTipProperty, new System.Windows.Data.Binding(nameof(TraceRow.TimeTip))));
         list_.Columns.Add(new DataGridTextColumn
         {
            Header = L("When (server time)"),
            Binding = new System.Windows.Data.Binding(nameof(TraceRow.Time)),
            SortMemberPath = nameof(TraceRow.OccurredAt),
            ElementStyle = timeTip,
            // Auto, not SizeToCells. SizeToCells measures the CELLS only, so on an
            // empty grid the column collapses to zero and its header disappears
            // with it - which is how Quarantine shipped a table showing Sender,
            // Recipients and Subject while Held and Score were simply absent until
            // the first row arrived. Auto is max(header, cells), so the column is
            // never narrower than the word naming it.
            Width = DataGridLength.Auto
         });

         list_.Columns.Add(new DataGridTextColumn
         {
            Header = L("Event"),
            Binding = new System.Windows.Data.Binding(nameof(TraceRow.EventName)),
            Width = DataGridLength.Auto
         });

         // Each address that is wider than its share ends in an ellipsis and is whole in
         // its cell's tool tip: at 150 per cent text "jane.doe@partner.example.net" was
         // cut to "jane.doe@partner.example.r" with nothing to say so (review finding F317).
         list_.Columns.Add(GridStyles.Trimmed(new DataGridTextColumn
         {
            Header = L("Sender"),
            Binding = new System.Windows.Data.Binding(nameof(TraceRow.Sender)),
            Width = new DataGridLength(1, DataGridLengthUnitType.Star)
         }));

         list_.Columns.Add(GridStyles.Trimmed(new DataGridTextColumn
         {
            Header = L("Recipient"),
            Binding = new System.Windows.Data.Binding(nameof(TraceRow.Recipient)),
            Width = new DataGridLength(1, DataGridLengthUnitType.Star)
         }));

         // The status as a word with the mark of its level - "OK 250", "Deferred
         // 451", "Refused 550" - where it was the bare code, right-aligned like a
         // count (review finding F253). Sorted by the code.
         var pill = new FrameworkElementFactory(typeof(StatusPill));
         pill.SetBinding(StatusPill.LevelProperty, new System.Windows.Data.Binding(nameof(TraceRow.StatusLevel)));
         pill.SetBinding(StatusPill.TextProperty, new System.Windows.Data.Binding(nameof(TraceRow.StatusText)));
         pill.SetBinding(VisibilityProperty, new System.Windows.Data.Binding(nameof(TraceRow.StatusShown)));
         pill.SetValue(HorizontalAlignmentProperty, HorizontalAlignment.Left);
         // Copied as its words, "Deferred 451": a template column copies nothing of its
         // own, and Ctrl+C on the results copied an empty status where the text column
         // it replaced copied the code.
         list_.Columns.Add(new DataGridTemplateColumn
         {
            Header = L("Status"),
            CellTemplate = new DataTemplate { VisualTree = pill },
            SortMemberPath = nameof(TraceRow.StatusCode),
            ClipboardContentBinding = new System.Windows.Data.Binding(nameof(TraceRow.StatusText)),
            Width = DataGridLength.Auto,
            MinWidth = StatusFloor_()
         });

         // The tip that used to live on each ListBoxItem. It says whether a row can
         // be followed at all, which is not otherwise visible - a refusal at RCPT
         // looks exactly like any other event until you try to follow it.
         // BasedOn the application's implicit DataGridRow style, not instead of it.
         // Assigning RowStyle REPLACES the implicit style rather than adding to it,
         // so building this from scratch would have quietly cost this grid alone the
         // hover and selection fills every other grid in the app has.
         var rowStyle = new Style(typeof(DataGridRow));

         if (Application.Current?.TryFindResource(typeof(DataGridRow)) is Style appRowStyle)
            rowStyle.BasedOn = appRowStyle;

         rowStyle.Setters.Add(new Setter(ToolTipProperty,
            new System.Windows.Data.Binding(nameof(TraceRow.FollowHint))));
         list_.RowStyle = rowStyle;
      }

      /// <summary>
      /// The Status column's floor: its header, and the widest of the three classes'
      /// pills with a three-digit code in the reader's language. Width Auto alone kept
      /// the header's width when the delivery queue's Trace message filled the grid
      /// before the page's first layout, and every pill read as its mark and a dot.
      /// </summary>
      private static double StatusFloor_()
      {
         double floor = GridStyles.HeaderWidth(L("Status"));
         foreach (int code in new[] { 250, 451, 550 })
            floor = Math.Max(floor, GridStyles.PillCellWidth(MessageTraceStatus.TextOf(code)));
         return floor;
      }

      private static Button MakeButton(string text, Wpf.Ui.Controls.ControlAppearance appearance, RoutedEventHandler onClick)
      {
         var button = new Wpf.Ui.Controls.Button
         {
            Content = text,
            Appearance = appearance,
            Margin = new Thickness(8, 0, 0, 0),
            MinWidth = 110
         };
         button.Click += onClick;
         return button;
      }
   }
}
