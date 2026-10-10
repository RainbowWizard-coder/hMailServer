// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using hMailServer.ControlPanel.Services;
using hMailServer.ControlPanel.Views.Scaffold;
using MessageBox = hMailServer.ControlPanel.Views.Dialogs;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Views
{
   /// <summary>
   /// The audit trail: who changed what, when, from where and with what
   /// authority, newest first, a page at a time, over Utilities.GetAuditTrail -
   /// the same rows the Deck's page reads over GET /api/v1/audit. The chain is
   /// verified from here, and a setting's change is put back to the value its
   /// row says it had, through Utilities.RevertAuditedChange, which records the
   /// put-back as one more row rather than editing history.
   /// </summary>
   public class AuditTrailView : UserControl, IPageLifecycle
   {
      // Every column a row shows is searched (AuditTrailReport.Matches), where the
      // search asked the server for the administrator alone (review finding F254).
      private readonly Toolbar toolbar_ = new() { SearchPlaceholder = L("Search who, what, interface or detail") };

      private readonly DataGrid rows_ = new()
      {
         AutoGenerateColumns = false,
         IsReadOnly = true,
         CanUserAddRows = false,
         CanUserDeleteRows = false,
         SelectionMode = DataGridSelectionMode.Single,
         MinHeight = 200
      };

      private readonly InlineNotice status_ = new() { Visibility = Visibility.Collapsed };
      private readonly EmptyState empty_ = new() { Icon = Wpf.Ui.Controls.SymbolRegular.DocumentBulletList24, Visibility = Visibility.Collapsed };

      // While a search reads the trail, in the list's place.
      private readonly LoadingState walking_ = new() { Visibility = Visibility.Collapsed };

      // Which rows of how many the page shows, "1 to 100 of 1,284", beside the
      // title, where an Information notice said it (review finding F288).
      private readonly StatusPill count_ = new() { Level = StatusLevel.Normal, Visibility = Visibility.Collapsed };

      private const int PageSize = 100;

      // The time as the grid shows it, and as its column is measured (WidestTime_).
      private const string WhenFormat = "yyyy-MM-dd HH:mm:ss"; // no-loc: a format

      // The columns of words, each with the floor its header sets and its value on a
      // row, floored at the widest of their rows on the page shown (FloorShared_), and
      // the most such a floor is.
      private readonly List<(DataGridTextColumn Column, double Header, Func<RowView, string> Value)> shared_ = new();
      private const double SharedFloorCap = 180;

      private int offset_;
      private int total_;

      // The pages of rows, under the list, each step pressable only where there is a
      // page that way, and where the page is (review finding F254): Newer and Older
      // were in the toolbar, never disabled, and wrapped onto different rows at 760.
      private readonly StackPanel pager_ = new() { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Right, Visibility = Visibility.Collapsed };
      private readonly TextBlock pageText_ = new() { VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(12, 0, 4, 0) };
      private Button newer_;
      private Button older_;

      // Put back, pressable only on a row it can put back: a setting's change that
      // records the value it had. It was the page's Primary and pressable on every
      // row, where on a domain's or an account's it answered with a notice.
      private Button putBack_;

      // The search asks the server once typing pauses, from the newest rows
      // (review finding F210).
      private readonly SearchDelay searchDelay_;

      // The whole trail as a search read it, which every word typed in after is
      // looked for in; read again by Refresh, a put back and the page opened again.
      private List<AuditRow> trail_;
      private bool trailCapped_;
      private int trailTotal_;
      private int generation_;
      private bool readingTrail_;

      // A search reads the trail a thousand rows at a time, the most the server
      // answers, and no more than this many of the newest, which it then says.
      private const int WalkPageSize = 1000;
      private const int MaxSearched = 100000;

      public AuditTrailView()
      {
         searchDelay_ = new SearchDelay(() => { offset_ = 0; Reload(); });
         Build();
      }

      // Read on entry rather than in the constructor: a change made since the
      // page was first opened must be here when somebody comes back to it.
      public void OnEnter()
      {
         generation_++;
         trail_ = null;
         Reload();
      }

      public void OnLeave() => searchDelay_.Cancel();

      private void Status_(StatusLevel level, string text)
      {
         ReadFailure.Clear(status_);
         status_.Level = level;
         status_.Text = text;
         status_.Visibility = Visibility.Visible;
      }

      private void Reload()
      {
         ReadFailure.Clear(status_);
         status_.Visibility = Visibility.Collapsed;
         count_.Visibility = Visibility.Collapsed;

         // A search looks in every column of the whole trail, which the page reads
         // for it (Search_); with none, the server's own pages, newest first.
         string query = toolbar_.SearchText?.Trim() ?? "";
         if (query.Length > 0)
         {
            Search_();
            return;
         }

         try
         {
            dynamic utilities = ServerSession.Current.Application.Utilities;

            AuditTrailReport report;
            try
            {
               report = AuditTrailReport.Parse((string)utilities.GetAuditTrail("", PageSize, offset_));
            }
            finally
            {
               ServerSession.Release((object)utilities);
            }

            if (report.Error != null)
            {
               ReadFailure.Show(status_, empty_, rows_, null, F("The server's answer could not be read: {0}", report.Error), Reload);
               ShowPager_();
               return;
            }

            total_ = report.Total;
            Show_(report.Entries);
            GatePutBack_();

            StatusText.Show(empty_, null, report.Total, null,
               L("Nothing has been recorded yet. Every administrative change appears here."));

            if (report.Total > 0)
            {
               count_.Text = F("{0} to {1} of {2}", offset_ + 1, offset_ + report.Entries.Count, report.Total);
               count_.Visibility = Visibility.Visible;
            }
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            // Neither the rows of the last read nor "Nothing has been recorded
            // yet." is a fact this page has now: both go, and the notice says what
            // happened, with Try again (Services/ReadFailure).
            ReadFailure.Show(status_, empty_, rows_, L("Could not read the audit trail: "), ex, Reload);
         }

         ShowPager_();
      }

      /// <summary>
      /// The rows the search finds, a page at a time: the trail read whole the first
      /// time - off the window's thread, a thousand rows a call, the list's place
      /// saying so - and every word typed in after looked for in it, so that a
      /// search finds an object, a name or a detail as well as who made the change.
      /// </summary>
      private async void Search_()
      {
         if (trail_ == null)
         {
            // One read at a time; it shows what is typed in when it ends, and a read
            // the page asked for again meanwhile (generation_) starts once it ends.
            if (readingTrail_)
               return;

            readingTrail_ = true;
            int generation = generation_;
            rows_.ItemsSource = null;
            empty_.Visibility = Visibility.Collapsed;
            pager_.Visibility = Visibility.Collapsed;
            walking_.Text = L("Searching the audit trail…");
            walking_.Visibility = Visibility.Visible;

            (List<AuditRow> Rows, int Total, bool Capped, string Error) read = default;
            Exception failure = null;
            try
            {
               read = await Task.Run(Walk_);
            }
            catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
            {
               failure = ex;
            }
            finally
            {
               readingTrail_ = false;
               walking_.Visibility = Visibility.Collapsed;
            }

            // The page was opened again, or refreshed, while the trail was read: that
            // Reload found this read running and left the trail to it, so it reads
            // again now, for what is typed in then - where it returned, and the list
            // stayed empty, with no rows, no empty or loading state and no pager.
            if (generation != generation_)
            {
               Reload();
               return;
            }

            if (failure != null || read.Error != null)
            {
               if (failure != null)
                  ReadFailure.Show(status_, empty_, rows_, L("Could not read the audit trail: "), failure, Reload);
               else
                  ReadFailure.Show(status_, empty_, rows_, null, F("The server's answer could not be read: {0}", read.Error), Reload);
               return;
            }

            trail_ = read.Rows;
            trailTotal_ = read.Total;
            trailCapped_ = read.Capped;

            // Typed away while it read: the trail's own pages.
            if ((toolbar_.SearchText?.Trim() ?? "").Length == 0)
            {
               Reload();
               return;
            }
         }

         string query = toolbar_.SearchText?.Trim() ?? "";
         List<AuditRow> matches = trail_.Where(row => AuditTrailReport.Matches(row, query)).ToList();
         total_ = matches.Count;
         if (offset_ >= total_)
            offset_ = 0;

         List<AuditRow> page = matches.Skip(offset_).Take(PageSize).ToList();
         Show_(page);
         GatePutBack_();

         StatusText.Show(empty_, null, total_, null,
            F("No change in the audit trail has \"{0}\" in who made it, what it was made to, its interface or its detail.", query));

         if (total_ > 0)
         {
            count_.Text = F("{0} to {1} of {2}", offset_ + 1, offset_ + page.Count, total_);
            count_.Visibility = Visibility.Visible;
         }

         // A trail longer than a search reads is a state with a consequence.
         if (trailCapped_)
            Status_(StatusLevel.Information, F("The search looks at the {0} most recent changes of the {1} in the trail.", trail_.Count, trailTotal_));

         ShowPager_();
      }

      /// <summary>The whole trail, newest first, as many of its rows as a search reads; on a worker thread.</summary>
      private static (List<AuditRow> Rows, int Total, bool Capped, string Error) Walk_()
      {
         var rows = new List<AuditRow>();
         int total = 0;
         dynamic utilities = ServerSession.Current.Application.Utilities;
         try
         {
            for (int offset = 0; rows.Count < MaxSearched; offset += WalkPageSize)
            {
               AuditTrailReport report = AuditTrailReport.Parse((string)utilities.GetAuditTrail("", WalkPageSize, offset));
               if (report.Error != null)
                  return (null, 0, false, report.Error);

               total = Math.Max(total, report.Total);
               rows.AddRange(report.Entries);
               if (report.Entries.Count < WalkPageSize || rows.Count >= report.Total)
                  break;
            }
         }
         finally
         {
            ServerSession.Release((object)utilities);
         }

         // Stopped at the most a search reads, with more rows in the trail.
         bool capped = rows.Count >= MaxSearched && total > rows.Count;
         return (rows, Math.Max(total, rows.Count), capped, null);
      }

      /// <summary>Newer and Older each pressable while there is a page that way, and where the page is between them.</summary>
      private void ShowPager_()
      {
         bool pages = total_ > PageSize && !ReadFailure.IsShown(status_) && !readingTrail_;
         KeepPagerKeyboard_(pages && offset_ > 0, pages && offset_ + PageSize < total_);
         pager_.Visibility = pages ? Visibility.Visible : Visibility.Collapsed;
         newer_.IsEnabled = offset_ > 0;
         older_.IsEnabled = offset_ + PageSize < total_;
         pageText_.Text = F("Page {0} of {1}", offset_ / PageSize + 1, (total_ + PageSize - 1) / PageSize);
      }

      /// <summary>
      /// The keyboard off a step of the pager that is about to be disabled or hidden:
      /// onto the other step where that stays pressable, otherwise onto the list. A
      /// button disabled or hidden while it holds the keyboard drops it to the window,
      /// so that Older pressed from the keyboard onto the last page, or Newer onto the
      /// first, left a keyboard user to start again from the top of the window.
      /// </summary>
      private void KeepPagerKeyboard_(bool newerStays, bool olderStays)
      {
         Button leaving = newer_.IsKeyboardFocused && !newerStays ? newer_
            : older_.IsKeyboardFocused && !olderStays ? older_
            : null;
         if (leaving == null)
            return;

         Button other = ReferenceEquals(leaving, newer_) ? older_ : newer_;
         if (ReferenceEquals(leaving, newer_) ? olderStays : newerStays)
         {
            other.IsEnabled = true;
            if (ReferenceEquals(System.Windows.Input.Keyboard.Focus(other), other))
               return;
         }

         GridKeys.FocusSelectedRow(rows_);
         if (leaving.IsKeyboardFocused)
            toolbar_.FocusSearch();
      }

      /// <summary>Put back is pressable on a setting's change it can put back, and says why not on any other row.</summary>
      private void GatePutBack_()
      {
         bool can = rows_.SelectedItem is RowView { CanPutBack: true };
         putBack_.IsEnabled = can;
         putBack_.ToolTip = can || rows_.SelectedItem == null
            ? null
            : L("Only a setting's change can be put back: the row has to carry the value the setting had.");
         ToolTipService.SetShowOnDisabled(putBack_, true);
      }

      /// <summary>A row as the grid shows it: the server's words turned into the reader's.</summary>
      private sealed class RowView
      {
         public AuditRow Row { get; init; }
         public long Id { get; init; }
         public string When { get; init; }
         public string Who { get; init; }
         public string Interface { get; init; }
         public string From { get; init; }
         public string Object { get; init; }
         public string Name { get; init; }
         public string Action { get; init; }
         public string Detail { get; init; }
         public bool CanPutBack { get; init; }

         // What a screen reader says for the row: when, who, and what was done to
         // what - not the name of this class, which is what a grid row is
         // announced as otherwise.
         public override string ToString() => RowSpeech.Of(When, Who, Action, Object, Name);
      }

      /// <summary>The rows into the grid, each column of words floored at them first (FloorShared_).</summary>
      private void Show_(IReadOnlyList<AuditRow> rows)
      {
         List<RowView> shown = Present_(rows);
         FloorShared_(shown);
         rows_.ItemsSource = shown;
      }

      private static List<RowView> Present_(IReadOnlyList<AuditRow> rows)
      {
         var result = new List<RowView>(rows.Count);
         foreach (AuditRow row in rows)
         {
            result.Add(new RowView
            {
               Row = row,
               Id = row.Id,
               When = row.Time > 0 ? DateTimeOffset.FromUnixTimeSeconds(row.Time).ToLocalTime().ToString(WhenFormat) : "",
               Who = row.Actor,
               // The server's tokens in the reader's words - "REST API", "Role
               // assignment", "Password reset" - where the page showed "rest",
               // "role_assignment" and "password_reset" in every language.
               Interface = AuditTrailReport.InterfaceWord(row.Interface),
               From = row.Address.Length == 0 ? "—" : row.Address, // no-loc
               Object = AuditTrailReport.ObjectWord(row.ObjectType),
               Name = row.ObjectName,
               Action = AuditTrailReport.ActionWord(row.Action),
               Detail = AuditTrailReport.ShownDetail(row.Detail),
               CanPutBack = row.CanPutBack
            });
         }
         return result;
      }

      private void VerifyChain()
      {
         status_.Visibility = Visibility.Collapsed;

         try
         {
            dynamic utilities = ServerSession.Current.Application.Utilities;
            AuditTrailReport.Verification verification;
            try
            {
               verification = AuditTrailReport.ParseVerification((string)utilities.VerifyAuditTrail());
            }
            finally
            {
               ServerSession.Release((object)utilities);
            }

            if (verification.Error != null)
               Status_(StatusLevel.Critical, F("The server's answer could not be read: {0}", verification.Error));
            else if (verification.Intact)
               Status_(StatusLevel.Good, Plural(verification.RowsChecked, "The chain is intact. {0} row checked.", "The chain is intact. {0} rows checked."));
            else
               Status_(StatusLevel.Critical, F("The chain is broken at row {0}. {1}", verification.FirstBrokenId, verification.Reason));
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            Status_(StatusLevel.Critical, ServerSession.DescribeComError(ex));
         }
      }

      private void PutBackSelected()
      {
         if (rows_.SelectedItem is not RowView row)
         {
            Status_(StatusLevel.Information, L("Select a setting's change first."));
            return;
         }

         if (!row.CanPutBack)
         {
            Status_(StatusLevel.Information, L("Only a setting's change can be put back: the row has to carry the value the setting had."));
            return;
         }

         // Cancel on Enter, like every other confirmation that changes what the
         // server does. The question names the change, both values, where the row
         // says them.
         string question = AuditTrailReport.TrySplitChange(row.Row, out string from, out string to)
            ? F("Put {0} back from \"{1}\" to \"{2}\"? The put-back is recorded as a change of its own.", row.Name, to, from)
            : F("Put {0} back to the value it had before this change? The put-back is recorded as a change of its own.", row.Name);
         if (!MessageBox.Confirm(question, L("Put a setting back"), L("_Put back"), cancelByDefault: true))
         {
            return;
         }

         try
         {
            dynamic utilities = ServerSession.Current.Application.Utilities;
            string restored;
            try
            {
               restored = (string)utilities.RevertAuditedChange((int)row.Id);
            }
            finally
            {
               ServerSession.Release((object)utilities);
            }

            // The put-back is a row of its own: a search reads the trail again.
            trail_ = null;
            Reload();
            Status_(StatusLevel.Good, F("{0} is {1} again.", row.Name, restored));
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
         AutomationProperties.SetLiveSetting(count_, AutomationLiveSetting.Polite);
         root.Children.Add(new PageHeader
         {
            Status = count_,
            Actions = MakeButton(L("_Refresh"), Wpf.Ui.Controls.ControlAppearance.Secondary, (_, _) => { generation_++; trail_ = null; Reload(); }),
            PageKey = "audit",
            Subtitle = L("Every administrative change as one row in a hash chain: who made it, when, over which interface, from where, and with what authority. A setting's change can be put back from its row.")
         });

         // A query to the server, run once typing pauses, where it waited for
         // Enter, which nothing on the page said (review finding F210); Enter
         // runs it at once.
         toolbar_.SearchTextChanged += (_, _) => searchDelay_.Restart();
         toolbar_.PreviewKeyDown += (_, e) => { if (e.Key == System.Windows.Input.Key.Enter) searchDelay_.Now(); };

         // The selected row's verb, then the one that acts on the whole trail, each
         // a secondary button: Put back was the page's Primary and pressable on
         // every row; Refresh is the page's and is in the header
         // (docs/ControlPanelDesign.md, List pages; review findings F254, F288, F289).
         var actions = new WrapPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Right };
         putBack_ = MakeButton(L("_Put back"), Wpf.Ui.Controls.ControlAppearance.Secondary, (_, _) => PutBackSelected());
         AutomationProperties.SetAutomationId(putBack_, "audit-put-back");
         actions.Children.Add(putBack_);
         actions.Children.Add(MakeButton(L("_Verify the chain"), Wpf.Ui.Controls.ControlAppearance.Secondary, (_, _) => VerifyChain()));
         rows_.SelectionChanged += (_, _) => GatePutBack_();
         foreach (FrameworkElement button in actions.Children)
            button.Margin = new Thickness(button.Margin.Left, 3, button.Margin.Right, 3);
         toolbar_.Actions = actions;

         // The suppression list's shape: the toolbar, the notice, the list and the
         // selected row's detail under the header, the list taking the height the
         // others leave, down to three rows, and below that the page scrolls - so
         // that the detail never takes the list's height in a short window.
         ScrollViewer scroll = FillPanel.ScrollingPage(out FillPanel parts);
         Grid.SetRow(scroll, 1);
         root.Children.Add(scroll);
         parts.Children.Add(toolbar_);
         parts.Children.Add(status_);

         // The time at the width of its words, which are always as long; the
         // columns of words share what is left, each floored at the widest of its
         // words on the page (FloorShared_), and what changed - the column a reader
         // comes for - takes twice a share and never under 200 pixels, where the
         // columns sized to their words left it about 60 ("60 -> 1…") (review
         // finding F254). Where the floors do not fit, the grid scrolls sideways
         // rather than cut who made a change and what it was made to.
         DataGridTextColumn when = Text_(L("When"), nameof(RowView.When), DataGridLength.Auto);
         // Floored where they are fixed as well: below the floors of the shared
         // columns a grid takes the shortfall from its fixed columns, down to 20
         // pixels, and in the narrowest window the time and the address were gone.
         when.Width = Math.Ceiling(Math.Max(GridStyles.HeaderWidth(L("When")), GridStyles.CellWidth(WidestTime_())));
         when.MinWidth = when.Width.Value;
         rows_.Columns.Add(when);
         rows_.Columns.Add(Shared_(L("Who"), nameof(RowView.Who), row => row.Who));
         rows_.Columns.Add(Shared_(L("Interface"), nameof(RowView.Interface), row => row.Interface));
         rows_.Columns.Add(Shared_(L("From"), nameof(RowView.From), row => row.From, DataGridLength.Auto));
         rows_.Columns.Add(Shared_(L("Object"), nameof(RowView.Object), row => row.Object));
         rows_.Columns.Add(Shared_(L("Name"), nameof(RowView.Name), row => row.Name));
         rows_.Columns.Add(Shared_(L("Action"), nameof(RowView.Action), row => row.Action));

         DataGridTextColumn detail = Text_(L("Detail"), nameof(RowView.Detail), new DataGridLength(2, DataGridLengthUnitType.Star));
         detail.MinWidth = 200;
         rows_.Columns.Add(detail);

         // A grid whose columns keep their widths and scroll sideways keeps its upright
         // bar, so that the two bars cannot take turns for ever where the rows fit only
         // without the sideways one (docs/ControlPanelDesign.md, Grids).
         ScrollViewer.SetVerticalScrollBarVisibility(rows_, ScrollBarVisibility.Visible);

         // The whole detail on the row's tip: a long value is cut in the column.
         var rowStyle = new Style(typeof(DataGridRow));
         if (Application.Current?.TryFindResource(typeof(DataGridRow)) is Style appRowStyle)
            rowStyle.BasedOn = appRowStyle;
         rowStyle.Setters.Add(new Setter(ToolTipProperty, new System.Windows.Data.Binding(nameof(RowView.Detail))));
         rows_.RowStyle = rowStyle;

         AutomationProperties.SetName(rows_, L("Audit trail"));

         var host = new Grid();
         host.Children.Add(rows_);
         host.Children.Add(empty_);
         host.Children.Add(walking_);
         var card = new Card { Padding = new Thickness(8), Content = host };
         parts.Children.Add(FillPanel.Filling(card, FillPanel.ListCardFloor));

         // The pages, under the list: Newer, where the page is, Older.
         newer_ = MakeButton(L("_Newer"), Wpf.Ui.Controls.ControlAppearance.Secondary, (_, _) => { offset_ = Math.Max(0, offset_ - PageSize); Reload(); });
         older_ = MakeButton(L("_Older"), Wpf.Ui.Controls.ControlAppearance.Secondary, (_, _) => { if (offset_ + PageSize < total_) { offset_ += PageSize; Reload(); } });
         newer_.Margin = new Thickness(0);
         pageText_.SetResourceReference(StyleProperty, "TextSecondary");
         AutomationProperties.SetAutomationId(pager_, "audit-pager");
         pager_.Margin = new Thickness(0, DesignTokens.Space.Sm, 0, 0);
         pager_.Children.Add(newer_);
         pager_.Children.Add(pageText_);
         pager_.Children.Add(older_);
         parts.Children.Add(pager_);

         // The selected row's whole detail, which a long value's column cuts and
         // only the row's tool tip held (Views/RowDetailStrip).
         parts.Children.Add(RowDetailStrip.For(rows_, _ => L("Detail"), row => ((RowView)row).Detail));

         GatePutBack_();
         Content = root;
      }

      private static DataGridTextColumn Text_(string header, string path, DataGridLength width)
         => GridStyles.Trimmed(new DataGridTextColumn { Header = header, Binding = new System.Windows.Data.Binding(path), Width = width });

      /// <summary>
      /// A column of words that shares the width - a star, or the address's Auto - floored
      /// at its header until the page's rows floor it at their widest (FloorShared_).
      /// </summary>
      private DataGridTextColumn Shared_(string header, string path, Func<RowView, string> value, DataGridLength? width = null)
      {
         DataGridTextColumn column = Text_(header, path, width ?? new DataGridLength(1, DataGridLengthUnitType.Star));
         double floor = Math.Ceiling(GridStyles.HeaderWidth(header));
         column.MinWidth = floor;
         shared_.Add((column, floor, value));
         return column;
      }

      /// <summary>
      /// Each column of words floored at the widest of its words on the page shown, and
      /// no further than <see cref="SharedFloorCap"/>, where a longer one ends in an
      /// ellipsis and says itself in a tool tip: floored at 90 pixels, the five stars
      /// shared what was left at 1280 by 800 and cut what the page itself shows in every
      /// language - "Administr…", "Control Pa…", "AutoBan…", "user03@e…", the name of
      /// the setting a put-back decision needs - where sized to their words they had
      /// read whole (review finding F254).
      /// </summary>
      private void FloorShared_(IReadOnlyList<RowView> rows)
      {
         foreach ((DataGridTextColumn column, double header, Func<RowView, string> value) in shared_)
            column.MinWidth = AuditTrailReport.WordsFloor(header, rows.Select(row => GridStyles.CellWidth(value(row))), SharedFloorCap);
      }

      /// <summary>
      /// The widest time the column can show: the format's, each digit the widest digit of
      /// the cell's font, whose digits are not all one width (AuditTrailReport.WidestDigits).
      /// </summary>
      private static string WidestTime_()
         => AuditTrailReport.WidestDigits(new DateTime(2026, 10, 10, 10, 10, 10).ToString(WhenFormat), text => GridStyles.CellWidth(text));

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
