// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Globalization;
using System.Linq;
using System.Text.Json;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Automation.Peers;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Threading;
using hMailServer.ControlPanel.Services;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Views
{
   /// <summary>
   /// Live server statistics: a "needs attention" setup summary, five KPI cards
   /// and two charts.
   ///
   /// The charts are <see cref="AccessibleChartCard"/> instances built from
   /// <see cref="ChartCatalog"/> rather than LiveCharts controls declared in the
   /// XAML. That is what gives them a High Contrast palette, a table view of the
   /// samples, and a legend that identifies each series by dash pattern and shape;
   /// this page's remaining job is to read the status snapshot and push one sample
   /// per series into each card.
   ///
   /// The setup summary runs <see cref="ExternalSetupChecks"/> - the same checks
   /// the Setup checklist page shows in full - because the administrator who most
   /// needs that page is the one who does not know it exists. The dashboard is
   /// where they land, so it is where "3 things need attention" has to appear.
   /// </summary>
   public partial class DashboardView : UserControl, IPageLifecycle, IRefreshablePage
   {
      private const int HistoryLength = 90;

      private readonly DispatcherTimer timer_;
      private readonly AccessibleChartCard throughputCard_;
      private readonly AccessibleChartCard sessionsCard_;

      private long lastProcessed_ = -1;
      private DateTime lastSampleUtc_ = DateTime.MinValue;

      // The range selector. "live" shows the two cards above, fed every two
      // seconds from Status and holding three minutes; the other ranges show two
      // cards of the same shape loaded from the history the server keeps
      // (MetricsHistoryDays), through Utilities.GetMetricHistory. The note on a
      // range is under the choice, the page's width: beside it, in a row of
      // unbounded width, it ran past the window's edge and was cut mid-sentence.
      private readonly StackPanel rangeBar_ = new() { Margin = new Thickness(0, 0, 0, 8) };
      private readonly TextBlock rangeNote_ = new()
      {
         FontSize = Typography.Caption,
         Opacity = 0.75,
         TextWrapping = TextWrapping.Wrap,
         Margin = new Thickness(0, 6, 0, 0)
      };
      private string range_ = "live";
      private AccessibleChartCard historyThroughput_;
      private AccessibleChartCard historySessions_;

      public DashboardView()
      {
         InitializeComponent();

         throughputCard_ = new AccessibleChartCard(ChartCatalog.DashboardThroughput, HistoryLength)
         {
            EmptyText = L("No delivery activity yet"),
            // Taller than the 300 the bare chart used: the card now carries a
            // legend and a summary line under the plot, and squeezing the plot to
            // keep the old height is how you end up with a chart too short to read.
            Height = 340,
            Margin = new Thickness(0, 0, 12, 0)
         };

         sessionsCard_ = new AccessibleChartCard(ChartCatalog.DashboardSessions, HistoryLength)
         {
            EmptyText = L("No active sessions"),
            Height = 340
         };

         Grid.SetColumn(throughputCard_, 0);
         Grid.SetColumn(sessionsCard_, 1);
         ChartsGrid.Children.Add(throughputCard_);
         ChartsGrid.Children.Add(sessionsCard_);

         BuildRangeSelector_();

         timer_ = new DispatcherTimer { Interval = TimeSpan.FromSeconds(2) };
         timer_.Tick += (s, e) => Refresh();
      }

      // The chart range is one choice of four: a radio group named Chart range,
      // its chosen range drawn as the primary button was and the others as the
      // secondary ones (WPF-UI's toggle button, whose checked state is the accent
      // fill). Four plain buttons marked the chosen range by their colour alone,
      // so a screen reader heard "Show Live" and three more with none of them
      // chosen, and pressing the chosen range read the history again (review
      // finding F244). The group is one tab stop, and the arrow keys move the
      // choice as in any radio group; a chosen range pressed again does nothing.
      private void BuildRangeSelector_()
      {
         var caption = new TextBlock
         {
            Text = L("Chart range"),
            FontSize = Typography.Body,
            VerticalAlignment = VerticalAlignment.Center,
            Margin = new Thickness(0, 0, 8, 0)
         };
         var row = new StackPanel { Orientation = Orientation.Horizontal };
         row.Children.Add(caption);

         var group = new ChartRangeGroup { Orientation = Orientation.Horizontal };
         AutomationProperties.SetName(group, L("Chart range"));
         AutomationProperties.SetAutomationId(group, "dashboard-chart-range");
         KeyboardNavigation.SetTabNavigation(group, KeyboardNavigationMode.Once);

         var segment = new Style(typeof(RadioButton), TryFindResource("DefaultToggleButtonStyle") as Style);
         segment.Setters.Add(new Setter(MarginProperty, new Thickness(0, 0, 6, 0)));

         var ranges = new[] { ("live", L("Live")), ("24h", L("24 hours")), ("7d", L("7 days")), ("30d", L("30 days")) };
         foreach ((string key, string label) in ranges)
         {
            var choice = new RadioButton
            {
               Content = label,
               GroupName = "dashboard-chart-range", // no-loc
               IsChecked = key == "live",
               Style = segment,
               Tag = key
            };
            AutomationProperties.SetAutomationId(choice, "dashboard-range-" + key);
            // "2 of 4": a group of radio buttons in a panel has no items control
            // to count them for the reader.
            AutomationProperties.SetPositionInSet(choice, group.Children.Count + 1);
            AutomationProperties.SetSizeOfSet(choice, ranges.Length);
            choice.Checked += (s, e) => ShowRange_(key);
            group.Children.Add(choice);
         }

         group.PreviewKeyDown += (s, e) => MoveRangeChoice_(group, e);
         row.Children.Add(group);
         rangeBar_.Children.Add(row);

         // Shown while it says something: live has no note.
         var whenEmpty = new Style(typeof(TextBlock), TryFindResource(typeof(TextBlock)) as Style);
         whenEmpty.Triggers.Add(new Trigger { Property = TextBlock.TextProperty, Value = "", Setters = { new Setter(VisibilityProperty, Visibility.Collapsed) } });
         rangeNote_.Style = whenEmpty;
         rangeBar_.Children.Add(rangeNote_);

         var parent = (Panel)ChartsGrid.Parent;
         parent.Children.Insert(parent.Children.IndexOf(ChartsGrid), rangeBar_);
      }

      /// <summary>
      /// The arrow keys in the chart range: the next or the previous range, round
      /// from the last to the first, chosen and with the keyboard on it.
      /// </summary>
      private static void MoveRangeChoice_(Panel group, KeyEventArgs e)
      {
         int step = e.Key is Key.Right or Key.Down ? 1 : e.Key is Key.Left or Key.Up ? -1 : 0;
         var choices = group.Children.OfType<RadioButton>().ToList();
         int at = choices.FindIndex(c => c.IsKeyboardFocusWithin);
         if (step == 0 || at < 0 || Keyboard.Modifiers != ModifierKeys.None)
            return;

         RadioButton next = choices[(at + step + choices.Count) % choices.Count];
         next.IsChecked = true;
         next.Focus();
         e.Handled = true;
      }

      /// <summary>The chart range's radio buttons, a group a screen reader names.</summary>
      private sealed class ChartRangeGroup : StackPanel
      {
         protected override AutomationPeer OnCreateAutomationPeer() => new GroupPeer(this);

         private sealed class GroupPeer : FrameworkElementAutomationPeer
         {
            public GroupPeer(ChartRangeGroup owner) : base(owner) { }

            protected override AutomationControlType GetAutomationControlTypeCore() => AutomationControlType.Group;

            protected override string GetClassNameCore() => nameof(ChartRangeGroup);

            protected override bool IsControlElementCore() => true;
         }
      }

      private void ShowRange_(string range)
      {
         range_ = range;

         ChartsGrid.Children.Clear();

         if (range == "live")
         {
            historyThroughput_ = null;
            historySessions_ = null;
            rangeNote_.Text = "";
            ChartsGrid.Children.Add(throughputCard_);
            ChartsGrid.Children.Add(sessionsCard_);
            return;
         }

         int minutes = range == "30d" ? 30 * 24 * 60 : range == "7d" ? 7 * 24 * 60 : 24 * 60;
         int bucket = range == "30d" ? 60 : range == "7d" ? 10 : 1;

         try
         {
            dynamic utilities = ServerSession.Current.Application.Utilities;

            // Throughput is a total; the chart wants a rate. The difference between
            // consecutive buckets over the bucket length is messages per minute, and
            // a drop (a restart reset the counter) is shown as zero rather than as a
            // negative rate.
            (bool enabled, int retention, var processed) = Samples_((string)utilities.GetMetricHistory("processed_messages_total", minutes, bucket));
            var smtp = Samples_((string)utilities.GetMetricHistory("sessions_smtp", minutes, bucket)).samples;
            var imap = Samples_((string)utilities.GetMetricHistory("sessions_imap", minutes, bucket)).samples;
            var pop3 = Samples_((string)utilities.GetMetricHistory("sessions_pop3", minutes, bucket)).samples;

            if (!enabled)
            {
               rangeNote_.Text = L("The server keeps no history: MetricsHistoryDays is 0 in hMailServer.ini. Set it to the days to keep.");
               ChartsGrid.Children.Add(throughputCard_);
               ChartsGrid.Children.Add(sessionsCard_);
               return;
            }

            historyThroughput_ = new AccessibleChartCard(ChartCatalog.DashboardThroughput, Math.Max(2, processed.Count))
            {
               EmptyText = L("No samples in this range yet"),
               Height = 340,
               Margin = new Thickness(0, 0, 12, 0)
            };

            for (int i = 1; i < processed.Count; i++)
            {
               double rate = Math.Max(0, processed[i].value - processed[i - 1].value) / bucket;
               historyThroughput_.Push(processed[i].time, rate);
            }

            var times = smtp.Select(x => x.time).Union(imap.Select(x => x.time)).Union(pop3.Select(x => x.time)).OrderBy(t => t).ToList();
            var smtpByTime = smtp.ToDictionary(x => x.time, x => x.value);
            var imapByTime = imap.ToDictionary(x => x.time, x => x.value);
            var pop3ByTime = pop3.ToDictionary(x => x.time, x => x.value);

            historySessions_ = new AccessibleChartCard(ChartCatalog.DashboardSessions, Math.Max(2, times.Count))
            {
               EmptyText = L("No samples in this range yet"),
               Height = 340
            };

            foreach (DateTime time in times)
            {
               historySessions_.Push(time,
                  smtpByTime.TryGetValue(time, out double a) ? a : (double?)null,
                  imapByTime.TryGetValue(time, out double b) ? b : (double?)null,
                  pop3ByTime.TryGetValue(time, out double c) ? c : (double?)null);
            }

            Grid.SetColumn(historyThroughput_, 0);
            Grid.SetColumn(historySessions_, 1);
            ChartsGrid.Children.Add(historyThroughput_);
            ChartsGrid.Children.Add(historySessions_);

            string per = bucket == 1 ? L("minute") : bucket == 10 ? L("ten minutes") : L("hour");
            rangeNote_.Text = processed.Count == 0
               ? Plural(retention, "No samples in this range yet: the server records one a minute and keeps {0} day.", "No samples in this range yet: the server records one a minute and keeps {0} days.")
               : F("One point per {0}, from the {1}-day history the server keeps. Throughput is the change in the processed-messages total per minute.", per, retention);
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            rangeNote_.Text = F("Could not load the history: {0}", ServerSession.DescribeComError(ex));
            ChartsGrid.Children.Add(throughputCard_);
            ChartsGrid.Children.Add(sessionsCard_);
         }
      }

      // The JSON Utilities.GetMetricHistory returns, as points. Times are as the
      // server's database stored them: its own clock, shown as it is.
      private static (bool enabled, int retention, System.Collections.Generic.List<(DateTime time, double value)> samples) Samples_(string json)
      {
         var samples = new System.Collections.Generic.List<(DateTime time, double value)>();
         bool enabled = false;
         int retention = 0;

         using (JsonDocument document = JsonDocument.Parse(json))
         {
            JsonElement root = document.RootElement;
            enabled = root.TryGetProperty("enabled", out JsonElement enabledElement) && enabledElement.GetBoolean();
            retention = root.TryGetProperty("retention_days", out JsonElement retentionElement) ? retentionElement.GetInt32() : 0;

            if (root.TryGetProperty("samples", out JsonElement array))
            {
               foreach (JsonElement sample in array.EnumerateArray())
               {
                  string time = sample.GetProperty("time").GetString() ?? "";
                  double value = sample.GetProperty("value").GetDouble();

                  if (DateTime.TryParseExact(time, "yyyy-MM-dd HH:mm:ss", CultureInfo.InvariantCulture, DateTimeStyles.None, out DateTime parsed))
                     samples.Add((parsed, value));
               }
            }
         }

         return (enabled, retention, samples);
      }

      public void OnEnter()
      {
         // The timer runs only while the statistics can be read: a read that fails
         // stops it and says so, and Try again - or the shell, when the session
         // heals and it enters the page again - starts it.
         if (Refresh())
            timer_.Start();
         BeginSetupCheck_();
      }

      /// <summary>F5: the numbers and the setup check now, rather than at the next tick.</summary>
      void IRefreshablePage.Refresh()
      {
         Refresh();
         BeginSetupCheck_();
      }

      public void OnLeave()
      {
         timer_.Stop();

         // Orphan any setup check still in flight: a DNS pass that completes
         // after the user has left must not repaint a page they are no longer
         // on, and re-entering starts a fresh check anyway.
         setupGeneration_++;
      }

      /// <summary>
      /// Reads the status snapshot into the KPI cards and the charts. False when the
      /// read failed, which the notice under the header then says - once, at the
      /// level its cause has (ReadFailure: Critical, the level the shell's pill gives
      /// a lost link), with Try again - in place of the grey "Connection to the
      /// server lost." the subtitle used to carry with no mark, word or colour.
      /// </summary>
      private bool Refresh()
      {
         var session = ServerSession.Current;
         if (session == null || !session.IsConnected)
            return false;

         try
         {
            var snap = session.ReadStatus();

            SetKpi_(KpiProcessed, L("Messages processed"), snap.ProcessedMessages.ToString("N0"));
            SetKpi_(KpiSpam, L("Spam detected"), snap.SpamBlocked.ToString("N0"));
            SetKpi_(KpiViruses, L("Viruses removed"), snap.VirusesRemoved.ToString("N0"));
            SetKpi_(KpiUptime, L("Uptime"), ServerUptimeText.Uptime(snap.StartTime, DateTime.Now, ServerSession.IsLocalSession, compact: true));
            ShowQueue_(snap.QueueLength);

            // Two clocks on purpose. The table's Time column has to be local time
            // to mean anything to the reader, but the throughput rate is a division
            // by an elapsed interval, and a daylight-saving change or a clock
            // correction on local time would turn one sample into a spike of
            // thousands of messages a minute.
            DateTime nowLocal = DateTime.Now;
            DateTime nowUtc = DateTime.UtcNow;

            sessionsCard_.Push(nowLocal, snap.SmtpSessions, snap.ImapSessions, snap.Pop3Sessions);

            if (lastProcessed_ >= 0 && lastSampleUtc_ != DateTime.MinValue)
            {
               double seconds = (nowUtc - lastSampleUtc_).TotalSeconds;
               if (seconds > 0.5)
               {
                  throughputCard_.Push(nowLocal,
                     Math.Max(0, snap.ProcessedMessages - lastProcessed_) * 60.0 / seconds);
               }
            }

            lastProcessed_ = snap.ProcessedMessages;
            lastSampleUtc_ = nowUtc;

            Header.Subtitle = F("Live server statistics - last update {0}", nowLocal.ToLongTimeString());
            lastRead_ = nowLocal;
            ReadFailure.Clear(ReadNotice);
            return true;
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            timer_.Stop();
            ShowKpisUnread_();
            ReadFailure.Show(ReadNotice, null, null, L("Could not read the server's statistics: "), ex, TryAgain_);
            return false;
         }
      }

      /// <summary>When the statistics were last read, or null before the first read that worked.</summary>
      private DateTime? lastRead_;

      /// <summary>
      /// What the KPI cards say while the statistics cannot be read. Never read: a
      /// dash in every card, named "not available" - a 0 is a count, and "In queue
      /// 0" is a statement the page cannot make. Read before: the figures stay, as
      /// the last the server gave, dimmed and each named with the time it was read,
      /// and the subtitle says when that was - figures that froze with nothing to
      /// flag them would read as current. Dimmed to the secondary text colour rather
      /// than by opacity, which would take the 11-pixel labels under 4.5:1.
      /// </summary>
      private void ShowKpisUnread_()
      {
         var cards = new (TextBlock value, string label)[]
         {
            (KpiUptime, L("Uptime")),
            (KpiProcessed, L("Messages processed")),
            (KpiQueue, L("In queue")),
            (KpiSpam, L("Spam detected")),
            (KpiViruses, L("Viruses removed"))
         };

         if (lastRead_ == null)
         {
            foreach ((TextBlock value, string label) in cards)
            {
               value.Text = NotRead;
               value.ClearValue(TextBlock.ForegroundProperty);
               AutomationProperties.SetName(value, F("{0}, not available", label));
            }

            KpiQueueBadge.Visibility = Visibility.Hidden;
            return;
         }

         string time = lastRead_.Value.ToLongTimeString();
         foreach ((TextBlock value, string label) in cards)
         {
            value.SetResourceReference(TextBlock.ForegroundProperty, "TextFillColorSecondaryBrush");
            AutomationProperties.SetName(value, F("{0}, {1}, as read at {2}", label, value.Text, time));
         }

         Header.Subtitle = F("Figures from {0}, the last time the server could be read.", time);
      }

      /// <summary>What a KPI card shows for a figure that has not been read.</summary>
      private const string NotRead = "—"; // no-loc: the dash a card shows for no figure

      /// <summary>
      /// Try again on the notice: reads once more and, when that works, starts the
      /// timer again and checks the setup again, since the summary card could not
      /// be built over the same lost link either.
      /// </summary>
      private void TryAgain_()
      {
         if (!Refresh())
            return;

         timer_.Start();
         BeginSetupCheck_();
      }

      /// <summary>
      /// Sets a KPI value and the accessible name that goes with it. The name has
      /// to carry the label as well as the number, because the five cards are five
      /// unlabelled numbers to anything reading the automation tree - "312" on its
      /// own could be the queue, the spam count or the uptime in minutes.
      /// </summary>
      private static void SetKpi_(TextBlock value, string label, string text)
      {
         value.Text = text;
         value.ClearValue(TextBlock.ForegroundProperty);   // a current figure, no longer dimmed
         AutomationProperties.SetName(value, label + ", " + text);
      }

      /// <summary>
      /// Shows the queue length, and says whether it is a backlog in three
      /// independent channels: the colour of the number, a shape plus the word
      /// "Backlog" underneath it, and the value's accessible name.
      ///
      /// Colour used to be the only channel. That made the one piece of judgement
      /// on the dashboard - "this queue is not draining" - invisible to anyone who
      /// cannot distinguish amber from the body text, which includes every user of
      /// a High Contrast theme, where the amber is replaced by a system colour that
      /// may well be the same one the number would have had anyway.
      /// </summary>
      private void ShowQueue_(int queueLength)
      {
         string text = queueLength.ToString("N0");
         StatusLevel level = StatusSemantics.ForQueueLength(queueLength);
         StatusPresentation status = StatusSemantics.For(level);

         KpiQueue.Text = text;
         KpiQueue.SetResourceReference(TextBlock.ForegroundProperty, status.BrushKey);

         if (level == StatusLevel.Normal)
         {
            // Hidden, not Collapsed. A collapsed badge gives its space back, so the card
            // grew by the badge's height the moment the queue crossed the backlog
            // threshold - and because the five KPI cards share a UniformGrid, the whole
            // row jumped taller at precisely the moment an operator was most likely to be
            // staring at it. Reserving the space costs nineteen pixels of empty card in
            // the normal case and buys a dashboard that does not move under the reader.
            KpiQueueBadge.Visibility = Visibility.Hidden;
            AutomationProperties.SetName(KpiQueue, F("In queue, {0}", text));
            return;
         }

         // The badge's word and its mark in the level's fill-text shade, as every
         // status word on a card is: the status colours are measured against the
         // page and lose contrast on a card. The number above keeps the status
         // colour - at 28 pixels it is large text, whose floor is 3:1.
         ShapeMarkVisuals.ApplyMark(KpiQueueShape, status.Shape, status.FillTextBrushKey);
         KpiQueueBadgeText.Text = L("Backlog");
         KpiQueueBadgeText.SetResourceReference(TextBlock.ForegroundProperty, status.FillTextBrushKey);
         KpiQueueBadge.Visibility = Visibility.Visible;
         KpiQueueBadge.ToolTip = F("More than {0} messages are waiting to be delivered.", StatusSemantics.QueueBacklogThreshold.ToString("N0"));

         AutomationProperties.SetName(KpiQueue,
            F("In queue, {0}. {1}: backlog.", text, status.SeverityWord));
      }

      // ---- the "needs attention" setup summary ---------------------------------

      /// <summary>
      /// Bumped whenever a new check starts (or the page is left). A check
      /// carries the generation it was started for and applies nothing if the
      /// counter has moved on - the same pattern ExternalSetupView uses - so a
      /// slow DNS pass can never overwrite a newer summary with stale verdicts.
      /// </summary>
      private int setupGeneration_;

      /// <summary>
      /// Starts the setup check without holding up the dashboard. The COM/INI/
      /// file half runs on the UI thread because the COM objects are apartment-
      /// bound there (every page reads them on that thread), but it is deferred
      /// to Background priority so the page paints first; the DNS half - the
      /// part that can block for seconds - runs on a background task, exactly as
      /// ExternalSetupView runs it.
      /// </summary>
      private void BeginSetupCheck_()
      {
         int generation = ++setupGeneration_;

         ShowSetupChecking_();

         Dispatcher.BeginInvoke(new Action(() =>
         {
            if (generation != setupGeneration_)
               return;

            var session = ServerSession.Current;
            if (session == null || !session.IsConnected)
            {
               ShowSetupUncheckable_(L("The external setup could not be checked - not connected to the server."));
               return;
            }

            ExternalSetupChecks checks;
            try
            {
               checks = ExternalSetupChecks.Run();
            }
            catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
            {
               // Run() degrades per-item rather than throwing, so this is belt
               // and braces - but a dashboard that dies building a summary card
               // would be strictly worse than a dashboard without one.
               ShowSetupUncheckable_(F("The external setup could not be checked: {0}", ServerSession.DescribeComError(ex)));
               return;
            }

            // A link lost while the checks ran is one event, and the notice under the
            // header says it at Critical: the card says it cannot tell, and draws no
            // count of things needing attention from values that were never read.
            if (checks.LinkLost)
            {
               ShowSetupUncheckable_(L("The external setup could not be checked - not connected to the server."));
               return;
            }

            RenderAttention_(checks, checks.DnsLookupCount);

            if (checks.DnsLookupCount == 0)
               return;

            Task.Run(() =>
            {
               checks.ResolveDnsLookups();

               Dispatcher.BeginInvoke(new Action(() =>
               {
                  if (generation != setupGeneration_)
                     return;

                  checks.ApplyDnsResults();
                  RenderAttention_(checks, 0);
               }));
            });
         }), DispatcherPriority.Background);
      }

      /// <summary>The transient state before the first verdict: plain text, no
      /// pill, because "checking" is not a status and must not borrow one.</summary>
      private void ShowSetupChecking_()
      {
         AttentionPill.Visibility = Visibility.Collapsed;
         AttentionSummary.Text = L("Checking what this server still needs done outside it…");
         AutomationProperties.SetName(AttentionSummary, AttentionSummary.Text);
         AttentionItems.Children.Clear();
         AttentionNote.Visibility = Visibility.Collapsed;
         AttentionReadNotice.Visibility = Visibility.Collapsed;
      }

      /// <summary>Honest degradation: the summary itself could not be built, which
      /// is presented as "Cannot tell", never as all-clear.</summary>
      private void ShowSetupUncheckable_(string message)
      {
         SetAttentionHeader_(SetupItemState.CannotTell);
         AttentionSummary.Text = message;
         AutomationProperties.SetName(AttentionSummary,
            ExternalSetupChecks.StateWord(SetupItemState.CannotTell) + ": " + message);
         AttentionItems.Children.Clear();
         AttentionNote.Visibility = Visibility.Collapsed;
         AttentionReadNotice.Visibility = Visibility.Collapsed;
      }

      /// <summary>
      /// Renders the aggregate and the items that need action. The full page
      /// (the setup checklist) shows all four states per item; this card shows the
      /// counts, the action items as one-line links to the page that fixes each,
      /// and a positive all-clear when there is nothing to do - an empty box
      /// would read as broken, and silence would read as "never checked".
      /// </summary>
      private void RenderAttention_(ExternalSetupChecks checks, int pendingDnsLookups)
      {
         int action = checks.Items.Count(i => i.State == SetupItemState.ActionNeeded);
         int unknown = checks.Items.Count(i => i.State == SetupItemState.CannotTell);

         SetupItemState overall = action > 0 ? SetupItemState.ActionNeeded
            : unknown > 0 ? SetupItemState.CannotTell
            : SetupItemState.Done;

         SetAttentionHeader_(overall);

         AttentionSummary.Text = SummaryText_(action, unknown, checks.Items.Count);

         // One utterance for a screen reader: the state word first, then the
         // counts, so the severity is heard even though the visible word sits in
         // a separate element.
         AutomationProperties.SetName(AttentionSummary,
            ExternalSetupChecks.StateWord(overall) + ": " + AttentionSummary.Text);

         AttentionItems.Children.Clear();
         int index = 0;
         foreach (SetupItem item in checks.Items.Where(i => i.State == SetupItemState.ActionNeeded))
            AttentionItems.Children.Add(AttentionLine_(item, index++));

         string note = pendingDnsLookups > 0
            ? Plural(pendingDnsLookups,
               "{0} DNS record is still being checked in the background; this summary will update.",
               "{0} DNS records are still being checked in the background; this summary will update.")
            : "";

         AttentionNote.Text = note;
         AttentionNote.Visibility = note.Length > 0 ? Visibility.Visible : Visibility.Collapsed;

         // Values the server answered without are a Warning, with the mark and the
         // word, as the setup checklist says them; a lost link never gets here.
         if (checks.FailedReads > 0)
            AttentionReadNotice.Show(StatusLevel.Warning,
               Plural(checks.FailedReads,
                  "{0} value could not be read from the server, so this summary may be incomplete.",
                  "{0} values could not be read from the server, so this summary may be incomplete."));
         else
            AttentionReadNotice.Hide();
      }

      /// <summary>Colour, shape AND word for the card's overall state - the same
      /// three channels every status badge in this application carries, so the
      /// meaning survives greyscale, colour blindness and High Contrast. The pill
      /// draws all three from the level; the word is the checklist's own.</summary>
      private void SetAttentionHeader_(SetupItemState overall)
      {
         AttentionPill.Level = ExternalSetupChecks.LevelFor(overall);
         AttentionPill.Text = ExternalSetupChecks.StateWord(overall);
         AttentionPill.Visibility = Visibility.Visible;
      }

      // Each count is a whole sentence in the forms its language has; two counts
      // are two sentences, the first placed whole in the second.
      private static string SummaryText_(int action, int unknown, int total)
      {
         if (action > 0)
         {
            string text = Plural(action, "{0} thing needs attention.", "{0} things need attention.");
            return unknown > 0
               ? PluralF(unknown, "{0} {1} item cannot be checked from here.", "{0} {1} items cannot be checked from here.", text, unknown)
               : text;
         }

         if (unknown > 0)
            return Plural(unknown, "Nothing needs action, but {0} item cannot be checked from here.", "Nothing needs action, but {0} items cannot be checked from here.");

         return Plural(total,
            "Nothing needs attention - the {0} external setup check is done or not needed.",
            "Nothing needs attention - all {0} external setup checks are done or not needed.");
      }

      /// <summary>
      /// One action item as one line: the warning mark plus the item's title as a
      /// link that opens the page owning its settings - see it AND do it, without
      /// hunting. The state is carried by shape, colour and the words "Action
      /// needed" in the accessible name, never by colour alone.
      /// </summary>
      private FrameworkElement AttentionLine_(SetupItem item, int index)
      {
         var row = new Grid { Margin = new Thickness(0, 2, 0, 0) };
         row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
         row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
         row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });

         // Numbered, in the checklist's own order - the order a new
         // installation meets them - so the list reads as steps and the first
         // one is where to start; it carries a "Next step" box as well.
         var number = new TextBlock
         {
            Text = (index + 1).ToString(System.Globalization.CultureInfo.CurrentCulture) + ".",
            MinWidth = 22,
            VerticalAlignment = VerticalAlignment.Center
         };
         number.SetResourceReference(StyleProperty, "TextBodyStrong");
         row.Children.Add(number);

         StatusPresentation presentation = StatusSemantics.For(ExternalSetupChecks.LevelFor(item.State));

         var mark = new System.Windows.Shapes.Path
         {
            Width = 9,
            Height = 9,
            Stretch = System.Windows.Media.Stretch.Fill,
            Margin = new Thickness(1, 0, 8, 0),
            VerticalAlignment = VerticalAlignment.Center
         };
         ShapeMarkVisuals.ApplyMark(mark, presentation.Shape, presentation.BrushKey);
         Grid.SetColumn(mark, 1);
         row.Children.Add(mark);

         // Every item on the setup checklist records the navigation key of the
         // page that owns its settings; fall back to the checklist itself if
         // one ever does not. The title is translated: it was passed through in
         // English, so a translated dashboard named the page in English.
         string page = item.Page ?? "externalsetup";
         string pageTitle = L(NavigationMap.TitleOf(page));

         // The link says where it goes as well as what it is for: the item,
         // then an arrow and the page it opens, in the secondary colour.
         var caption = new TextBlock { TextWrapping = TextWrapping.Wrap };
         caption.Inlines.Add(new System.Windows.Documents.Run(item.Title));
         var destination = new System.Windows.Documents.Run("  →  " + pageTitle); // no-loc: the arrow, then the translated page title
         destination.SetResourceReference(System.Windows.Documents.TextElement.ForegroundProperty, "TextFillColorSecondaryBrush");
         caption.Inlines.Add(destination);

         var link = new Wpf.Ui.Controls.Button
         {
            Appearance = Wpf.Ui.Controls.ControlAppearance.Transparent,
            FontSize = Typography.Label,
            Padding = new Thickness(6, 2, 8, 2),
            HorizontalAlignment = HorizontalAlignment.Left,
            Cursor = System.Windows.Input.Cursors.Hand,
            Content = caption,
            ToolTip = F("Open {0}, which owns the settings for: {1}", pageTitle, item.Title)
         };
         AutomationProperties.SetName(link, (index == 0 ? L("Next step") + ". " : "") + F("Action needed: {0}. Opens {1}.", item.Title, pageTitle));
         AutomationProperties.SetAutomationId(link, "dashboard-attention-" + index);
         link.Click += (s, e) => (Application.Current?.MainWindow as MainWindow)?.NavigateTo(page);

         var linkRow = new WrapPanel { Orientation = Orientation.Horizontal };
         linkRow.Children.Add(link);
         if (index == 0)
            linkRow.Children.Add(new Scaffold.StatusPill { Level = StatusLevel.Information, Text = L("Next step"), Margin = new Thickness(6, 0, 0, 0) });
         Grid.SetColumn(linkRow, 2);
         row.Children.Add(linkRow);

         return row;
      }

      private void OpenExternalSetup_Click(object sender, RoutedEventArgs e)
         => (Application.Current?.MainWindow as MainWindow)?.NavigateTo("externalsetup");
   }
}
