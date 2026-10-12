// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Linq;
using System.Windows;
using System.Windows.Controls;
using hMailServer.ControlPanel.Services;
using MessageBox = hMailServer.ControlPanel.Views.Dialogs;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Views
{
   public partial class IPRangesView : UserControl, IPageLifecycle, IRefreshablePage
   {
      // The narrowest a column of permissions may be before the card takes two
      // columns instead of four: at a quarter of a narrow card, the Portuguese
      // for "Require TLS for auth" broke in the middle of its last word.
      private const double PermColumnMinWidth = 180;

      public class RangeRow
      {
         public int Id { get; set; }
         public string Name { get; set; }
         public string LowerIP { get; set; }
         public string UpperIP { get; set; }
         public int Priority { get; set; }
         public string Smtp { get; set; }
         public string Imap { get; set; }
         public string Pop3 { get; set; }

         /// <summary>When the range ends, and whether and how it relays (Services/IpRangeCells).</summary>
         public string Expires { get; set; }
         public string Relay { get; set; }
         public StatusLevel RelayLevel { get; set; }
         public bool RelayMarked => RelayLevel != StatusLevel.Normal;

         /// <summary>Whether the range is one of the server's bans (Services/AutoBanList), for Show auto-bans only.</summary>
         public bool IsBan { get; set; }

         // What a screen reader says for the row: its cells, not the name of
         // this class, which is what a grid row is announced as otherwise.
         public override string ToString() => Name + ", " + LowerIP + " - " + UpperIP;
      }

      public IPRangesView()
      {
         InitializeComponent();

         // The captions name the fields and the boxes show an example, marked as
         // one, where the captions said "Lower IP (e.g. 10.0.0.0)" (review
         // finding F303).
         NewLower.PlaceholderText = F("e.g. {0}", "10.0.0.0");
         NewUpper.PlaceholderText = F("e.g. {0}", "10.0.0.255");

         // Properties and Delete act on the selected range: live with nothing
         // selected, one answered with a message box and the other with nothing.
         SelectionGate.Bind(RangeGrid, PropertiesButton, DeleteRangeButton);

         // Every box follows the card's pill: Unsaved changes from the first change,
         // and the relay notice, which follows Local to external and External to
         // external and their authentication.
         foreach (CheckBox box in PermissionBoxes())
         {
            box.Checked += (s, e) => { ShowPermsState_(); ShowRelayExposure_(); };
            box.Unchecked += (s, e) => { ShowPermsState_(); ShowRelayExposure_(); };
         }

         // Each delivery direction's authentication box counts only while the
         // direction is allowed, and follows it; both are named for their row and
         // their column, as a screen reader cannot see the grid (review finding F229).
         foreach ((CheckBox allowed, CheckBox authentication) in DeliveryRows_())
            DeliveryRow.Wire(allowed, authentication);

         ShowNoRange();
         FloorColumns_(Array.Empty<RangeRow>());

         // Enter and F2 open the selected range, Delete deletes it after asking,
         // Insert goes to the add form, and Enter in the form adds the range;
         // Escape empties its three boxes and leaves the priority (Views/GridKeys, FormSubmit).
         GridKeys.Wire(RangeGrid, GridKeys.Pressing(PropertiesButton), GridKeys.Pressing(DeleteRangeButton), GridKeys.Focusing(NewName));
         FormSubmit.Bind(AddRangeForm, AddRangeButton, NewName, NewLower, NewUpper);

         // Ctrl+S saves the permissions the card holds, the page's one unsaved
         // change (Views/PageCommands); F5 reads the ranges again (Refresh).
         PageCommands.SetCommand(SavePermsButton, PageCommand.Save);

         // The empty state under the grid's column headers, centred where the
         // rows would be, never drawn across the headers' line.
         Scaffold.EmptyStateUnderHeaders.Keep(EmptyStatus, RangeGrid);
      }

      // What the card settled in after the range was read or saved: Clean, or Saved
      // until the next change.
      private SettingsPageState permsSettled_ = SettingsPageState.Clean;

      /// <summary>
      /// The permissions card's pill, by the settings pages' one save-state model
      /// (SettingsPageStatus): none while the boxes hold what the range has, Unsaved
      /// changes from the first change, and Saved after a save until the next - where
      /// a save put "(saved)" after the range's name in the card's title, and a change
      /// said nothing.
      /// </summary>
      private void ShowPermsState_()
      {
         SettingsPageState state = SettingsPageStatus.Follow(permsSettled_, HasUnsavedChanges);
         PermsStatePill.Level = SettingsPageStatus.LevelFor(state);
         PermsStatePill.Text = SettingsPageStatus.WordFor(state);
         PermsStatePill.Visibility = SettingsPageStatus.ShowsPill(state) ? Visibility.Visible : Visibility.Collapsed;
      }

      // The range whose permissions the card shows, and what its boxes held when
      // they were filled from the server or saved to it: a box ticked or cleared
      // since is an unsaved change.
      private RangeRow permsRow_;
      private readonly EditorBaseline perms_ = new();

      // Set while the rows are replaced, and while the selection is put back on the
      // range whose changes are on the card, so that neither is taken for a choice
      // of another range.
      private bool reloading_;
      private bool restoringRow_;

      // The range Add made and what the card says of it, while the card shows it
      // and until its permissions are saved.
      private int addedRangeId_;
      private string addedText_;

      /// <summary>
      /// The page as it is opened: read from the server, the card's boxes included.
      /// Changes left on the card when the page was left were discarded by leaving it
      /// (MainWindow asked), so they are not kept.
      /// </summary>
      public void OnEnter()
      {
         ForgetPermissionChanges_();
         addedText_ = null;
         Reload();
      }

      public void OnLeave()
      {
      }

      /// <summary>
      /// F5: the ranges again, as a save or an add reads them - the selected range
      /// kept, and the permission boxes' unsaved changes with it - where opening
      /// the page forgets them.
      /// </summary>
      void IRefreshablePage.Refresh() => Reload();

      /// <summary>Whether a permission box holds something other than what the range has on the server.</summary>
      public bool HasUnsavedChanges => permsRow_ != null && PermGrid.IsEnabled && perms_.Differs(PermissionValues_());

      /// <summary>Save permissions, for the range the card shows; true when it was saved.</summary>
      public bool SaveChanges() => permsRow_ == null || SavePermissions_(permsRow_);

      private IEnumerable<string> PermissionValues_() => PermissionBoxes().Select(box => box.IsChecked is true ? "1" : "0");

      private void ForgetPermissionChanges_()
      {
         permsRow_ = null;
         perms_.Forget();
      }

      private void Reload()
      {
         // The range selected before stays selected after: a reload follows every
         // save, edit and add, and replacing the rows used to drop the selection,
         // which emptied the permissions card's title and left its boxes showing
         // the range that had just been saved. A range whose permissions have
         // unsaved changes on the card is the one selected again, and keeps them:
         // adding a range is no reason to lose them.
         bool keepChanges = HasUnsavedChanges;
         int? selectedId = keepChanges ? permsRow_.Id : (RangeGrid.SelectedItem as RangeRow)?.Id;

         var rows = new List<RangeRow>();
         try
         {
            bool sameClock = ServerSession.IsLocalSession;
            DateTime now = DateTime.Now;
            dynamic ranges = ServerSession.Current.Application.Settings.SecurityRanges;
            try
            {
               int count = (int)ranges.Count;
               for (int i = 0; i < count; i++)
               {
                  dynamic range = ranges.Item[i];
                  string name = (string)range.Name, lower = (string)range.LowerIP, upper = (string)range.UpperIP;
                  bool expires = (bool)range.Expires;
                  (string relay, StatusLevel relayLevel) = IpRangeCells.Relay(lower, upper,
                     (bool)range.AllowDeliveryFromLocalToRemote, (bool)range.RequireSMTPAuthLocalToExternal,
                     (bool)range.AllowDeliveryFromRemoteToRemote, (bool)range.RequireSMTPAuthExternalToExternal);
                  rows.Add(new RangeRow
                  {
                     Id = (int)range.ID,
                     Name = name,
                     LowerIP = lower,
                     UpperIP = upper,
                     Priority = (int)range.Priority,
                     Smtp = (bool)range.AllowSMTPConnections ? L("Yes") : L("No"),
                     Imap = (bool)range.AllowIMAPConnections ? L("Yes") : L("No"),
                     Pop3 = (bool)range.AllowPOP3Connections ? L("Yes") : L("No"),
                     Expires = IpRangeCells.Expires(expires, expires ? ExpiresAt_(range) : DateTime.MinValue, now, sameClock),
                     Relay = relay,
                     RelayLevel = relayLevel,
                     IsBan = AutoBanList.IsBan(name, expires)
                  });
                  ServerSession.Release(range);
               }
            }
            finally
            {
               ServerSession.Release(ranges);
            }
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            // Neither the rows of the last read nor "No IP ranges defined yet." is
            // a fact this page has now: both go, the notice says what happened,
            // with Try again, and Add and Restore defaults wait for a read that
            // works - Restore defaults replaces every range, which is the last
            // thing to offer over a list that could not be seen (Services/ReadFailure).
            ReadFailure.Show(Notice, EmptyStatus, RangeGrid, L("Could not read the IP ranges: "), ex, Reload,
               AddRangeButton, RestoreDefaultsButton);
            CountPill.Visibility = Visibility.Collapsed;
            // The permissions card keeps the edits it holds: a read that failed
            // is no more a reason to lose them than an added range is, and the
            // next read that works selects their range again. With none, the
            // card shows no range, since the list shows none.
            if (!keepChanges)
               ShowNoRange();
            return;
         }

         reloading_ = true;
         try
         {
            ReadFailure.Clear(Notice, AddRangeButton, RestoreDefaultsButton);
            // The order the list is in, which replacing its rows clears.
            SortDescription[] order = RangeGrid.Items.SortDescriptions.ToArray();
            ListSortDirection?[] directions = RangeGrid.Columns.Select(column => column.SortDirection).ToArray();
            RangeGrid.ItemsSource = rows;
            SortRows_(order, directions);
            FloorColumns_(rows);
            CountPill.Visibility = Visibility.Visible;
            ShowListState_();

            // Selected, not scrolled into view: the list keeps its own offset when its
            // rows are replaced, and ScrollIntoView would bring the row into the
            // page's view as well, scrolling the page away from the Save permissions
            // button or the Add button just pressed and back up to the list.
            RangeRow again = selectedId == null ? null : rows.FirstOrDefault(r => r.Id == selectedId.Value);
            if (again != null)
               RangeGrid.SelectedItem = again;
         }
         finally
         {
            reloading_ = false;
         }

         RangeRow kept = keepChanges ? rows.FirstOrDefault(r => r.Id == permsRow_.Id) : null;
         if (kept != null)
            permsRow_ = kept;
         else
            ShowPermissions_();
      }

      /// <summary>
      /// The rows in the order the server decides by - the highest priority first,
      /// the ranges of one priority by name (review finding F230) - where they were
      /// in the order the server listed them, though where ranges overlap the
      /// higher priority wins. The Priority column's sort direction says so, so
      /// that a click on its header turns the order round. A header clicked orders
      /// the list by its column instead, and that order is kept when the rows are
      /// read again, as every save, edit and add reads them.
      /// </summary>
      private void SortRows_(SortDescription[] order, ListSortDirection?[] directions)
      {
         bool chosen = order.Length > 0;
         using (RangeGrid.Items.DeferRefresh())
         {
            RangeGrid.Items.SortDescriptions.Clear();
            foreach (SortDescription by in chosen ? order : PriorityOrder)
               RangeGrid.Items.SortDescriptions.Add(by);
         }

         for (int i = 0; i < RangeGrid.Columns.Count; i++)
            RangeGrid.Columns[i].SortDirection = chosen && i < directions.Length ? directions[i] : null;
         if (!chosen)
            PriorityColumn.SortDirection = ListSortDirection.Descending;
      }

      private static readonly SortDescription[] PriorityOrder =
      {
         new(nameof(RangeRow.Priority), ListSortDirection.Descending),
         new(nameof(RangeRow.Name), ListSortDirection.Ascending)
      };

      /// <summary>
      /// The card's word on a range just added, while the card shows it; once the
      /// card shows another range, or none, the range is no longer the one added.
      /// </summary>
      private void ShowAdded_()
      {
         if (permsRow_ == null || permsRow_.Id != addedRangeId_)
         {
            addedRangeId_ = 0;
            addedText_ = null;
         }

         AddedNotice.Text = addedText_;
         AddedNotice.Visibility = addedText_ == null ? Visibility.Collapsed : Visibility.Visible;
      }

      /// <summary>
      /// The permissions card with no range selected: the title asks for one, and
      /// the boxes are clear and disabled, so they show no range's permissions and
      /// cannot be changed with nothing to save them to.
      /// </summary>
      private void ShowNoRange()
      {
         ForgetPermissionChanges_();
         PermCard.Title = L("Permissions (select a range)");
         foreach (CheckBox box in PermissionBoxes())
            box.IsChecked = false;
         PermGrid.IsEnabled = false;
         SavePermsButton.IsEnabled = false;
         permsSettled_ = SettingsPageState.Clean;
         ShowPermsState_();
         ShowRelayExposure_();
         ShowAdded_();
      }

      private IEnumerable<CheckBox> PermissionBoxes()
         => new[] { PermSmtp, PermImap, PermPop3, PermTlsAuth, PermLL, PermLR, PermRL, PermRR, PermAuthLL, PermAuthLR, PermAuthRL, PermAuthRR };

      /// <summary>Each delivery direction's row: the direction's own box, and Only after SMTP authentication.</summary>
      private IEnumerable<(CheckBox, CheckBox)> DeliveryRows_()
         => new[] { (PermLL, PermAuthLL), (PermLR, PermAuthLR), (PermRL, PermAuthRL), (PermRR, PermAuthRR) };

      private void ConnectionsGrid_SizeChanged(object sender, SizeChangedEventArgs e)
         => ConnectionsGrid.Columns = e.NewSize.Width >= 4 * PermColumnMinWidth ? 4 : 2;

      /// <summary>
      /// The card's word on relaying (Services/RelayExposure): a range that lets the
      /// addresses in it send mail to an external address without signing in - External
      /// to external, or Local to external, which anyone can use by giving one of this
      /// server's domains as the sender - is an open relay where it reaches the
      /// Internet, at Critical, and a warning inside a network of its own; none for one
      /// address, or while both directions ask for authentication or are not allowed.
      /// </summary>
      private void ShowRelayExposure_()
      {
         RelayExposure.Reach reach = permsRow_ == null || !PermGrid.IsEnabled
            ? RelayExposure.Reach.None
            : RelayExposure.Of(permsRow_.LowerIP, permsRow_.UpperIP, PermLR.IsChecked is true, PermAuthLR.IsChecked is true, PermRR.IsChecked is true, PermAuthRR.IsChecked is true);
         string notice = RelayExposure.Notice(reach, OpenOnCard_());
         RelayNotice.Level = RelayExposure.LevelOf(reach);
         RelayNotice.Text = notice;
         RelayNotice.Visibility = notice == null ? Visibility.Collapsed : Visibility.Visible;
      }

      /// <summary>The directions to an external address the card's boxes let through without signing in.</summary>
      private RelayExposure.Open OpenOnCard_()
         => RelayExposure.OpenDirections(PermLR.IsChecked is true, PermAuthLR.IsChecked is true, PermRR.IsChecked is true, PermAuthRR.IsChecked is true);

      /// <summary>
      /// No column is narrower than its header, and no column of addresses or permissions
      /// than the widest of its words - the widest IPv4 address and the ranges' own, Yes and No in
      /// the current language - measured in the grid's font at the Windows text size, so
      /// that a narrow window scrolls the grid sideways rather than cutting a word: fixed
      /// at 150 and 70 pixels, larger text cut them short (review finding F317).
      /// </summary>
      private void FloorColumns_(IEnumerable<RangeRow> rows)
      {
         // Every column at its header, Priority's too: a column of numbers keeps its
         // pixels, but its header is words, and at 150 per cent German cut it to "Priori…".
         foreach (DataGridColumn column in RangeGrid.Columns)
            Floor_(column, GridStyles.HeaderWidth(column.Header as string));

         foreach (DataGridColumn column in new DataGridColumn[] { LowerColumn, UpperColumn })
            Floor_(column, GridStyles.CellWidth("255.255.255.255")); // no-loc: the widest IPv4 address
         foreach (RangeRow row in rows)
         {
            Floor_(LowerColumn, GridStyles.CellWidth(row.LowerIP));
            Floor_(UpperColumn, GridStyles.CellWidth(row.UpperIP));
         }

         foreach (DataGridColumn column in new DataGridColumn[] { SmtpColumn, ImapColumn, Pop3Column })
         {
            Floor_(column, GridStyles.CellWidth(L("Yes")));
            Floor_(column, GridStyles.CellWidth(L("No")));
         }
      }

      /// <summary>Raises a column's floor to <paramref name="width"/>, a whole pixel, where that is more.</summary>
      private static void Floor_(DataGridColumn column, double width) => column.MinWidth = Math.Max(column.MinWidth, GridStyles.WholeDevicePixels(width));

      private void Search_TextChanged(object sender, EventArgs e) => ShowListState_();

      private void BansOnly_Changed(object sender, RoutedEventArgs e) => ShowListState_();

      // A VT_DATE in the server's local time, cast as the range's dialog casts it; a
      // value that is not one is taken for a time that has passed, as the server takes it.
      private static DateTime ExpiresAt_(dynamic range)
      {
         try
         {
            return (DateTime)range.ExpiresTime;
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            return DateTime.MinValue;
         }
      }

      /// <summary>
      /// The search over the IP ranges read, the count beside the title and the empty
      /// state: "Shown: 1 of 4" while a search is typed in, and where it hides every
      /// row, the sentence that says so in place of the empty list's
      /// (Services/ListSearchState). Show auto-bans only narrows the rows to the
      /// server's bans, which the search then filters within, and the pill counts what
      /// is shown of the whole list, as a search's does.
      /// </summary>
      private void ShowListState_()
      {
         string empty = L("No IP ranges, so this server refuses every SMTP, POP3 and IMAP connection until one exists.");
         if (BansOnlyBox.IsChecked == true)
         {
            ListSearchState.ShowNarrowed(RangeGrid, SearchBar.SearchText, CountPill,
               n => Plural(n, "{0} IP range", "{0} IP ranges"), EmptyStatus, empty, item => item is RangeRow { IsBan: true }, L("No address is banned now."));
            return;
         }

         ListSearchState.Show(RangeGrid, SearchBar.SearchText, CountPill,
            n => Plural(n, "{0} IP range", "{0} IP ranges"), EmptyStatus, empty);
      }

      /// <summary>
      /// The header's Refresh: the ranges read again as F5 reads them, the selected
      /// range kept and the permission boxes' unsaved changes with it.
      /// </summary>
      private void Refresh_Click(object sender, RoutedEventArgs e) => Reload();

      private void RangeGrid_SelectionChanged(object sender, SelectionChangedEventArgs e)
      {
         if (reloading_ || restoringRow_)
            return;

         // Changed permissions are not put out of the way by another range's: the
         // question comes first, once the click that chose the range has finished.
         // A selection that goes - a search that hides the range - leaves the card
         // on the range it shows, with its changes.
         if (HasUnsavedChanges)
         {
            if (RangeGrid.SelectedItem is RangeRow chosen && chosen.Id != permsRow_.Id)
            {
               RangeRow edited = permsRow_;
               Dispatcher.BeginInvoke(new Action(() => AskAboutPermissions_(edited)), System.Windows.Threading.DispatcherPriority.Input);
            }

            return;
         }

         ShowPermissions_();
      }

      /// <summary>
      /// The unsaved permissions of <paramref name="edited"/>, asked about after
      /// another range was chosen: Save writes them and shows the chosen range's,
      /// Discard shows the chosen range's, and Stay - or a save that failed - puts
      /// the selection back on the range whose changes are on the card.
      /// </summary>
      private void AskAboutPermissions_(RangeRow edited)
      {
         if (!HasUnsavedChanges || !ReferenceEquals(permsRow_, edited))
            return;

         if (RangeGrid.SelectedItem is not RangeRow chosen || chosen.Id == edited.Id)
            return;

         UnsavedAnswer answer = MessageBox.AskUnsaved(UnsavedChanges.PermissionsQuestion(edited.Name), L("Control Panel"));
         if (answer == UnsavedAnswer.Discard)
         {
            ForgetPermissionChanges_();
            ShowPermissions_();
            return;
         }

         if (answer == UnsavedAnswer.Save && SavePermissions_(edited))
         {
            // The list's Yes and No follow the save; the chosen range stays chosen.
            Reload();
            return;
         }

         restoringRow_ = true;
         try
         {
            RangeGrid.SelectedItem = (RangeGrid.ItemsSource as IEnumerable<RangeRow>)?.FirstOrDefault(r => r.Id == edited.Id);
         }
         finally
         {
            restoringRow_ = false;
         }
      }

      /// <summary>
      /// Fills the card with the selected range's permissions as the server holds
      /// them, or empties it when no range is selected.
      /// </summary>
      private void ShowPermissions_()
      {
         if (RangeGrid.SelectedItem is not RangeRow row)
         {
            ShowNoRange();
            return;
         }

         dynamic ranges = ServerSession.Current.Application.Settings.SecurityRanges;
         try
         {
            dynamic range = FindRange(ranges, row);
            if (range == null)
            {
               ShowNoRange();
               return;
            }

            PermCard.Title = F("Permissions - {0}", row.Name);
            PermSmtp.IsChecked = (bool)range.AllowSMTPConnections;
            PermImap.IsChecked = (bool)range.AllowIMAPConnections;
            PermPop3.IsChecked = (bool)range.AllowPOP3Connections;
            PermTlsAuth.IsChecked = (bool)range.RequireSSLTLSForAuth;
            PermLL.IsChecked = (bool)range.AllowDeliveryFromLocalToLocal;
            PermLR.IsChecked = (bool)range.AllowDeliveryFromLocalToRemote;
            PermRL.IsChecked = (bool)range.AllowDeliveryFromRemoteToLocal;
            PermRR.IsChecked = (bool)range.AllowDeliveryFromRemoteToRemote;
            PermAuthLL.IsChecked = (bool)range.RequireSMTPAuthLocalToLocal;
            PermAuthLR.IsChecked = (bool)range.RequireSMTPAuthLocalToExternal;
            PermAuthRL.IsChecked = (bool)range.RequireSMTPAuthExternalToLocal;
            PermAuthRR.IsChecked = (bool)range.RequireSMTPAuthExternalToExternal;
            PermGrid.IsEnabled = true;
            SavePermsButton.IsEnabled = true;
            ServerSession.Release(range);
            permsRow_ = row;
            perms_.Take(PermissionValues_());
            permsSettled_ = SettingsPageState.Clean;
            ShowPermsState_();
            ShowRelayExposure_();
            ShowAdded_();
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            ShowNoRange();
         }
         finally
         {
            ServerSession.Release(ranges);
         }
      }

      // By the range's ID, as the properties dialog finds it: two ranges may
      // share a name and a lower address, and matching on those acted on the
      // first of them whichever was selected.
      private static dynamic FindRange(dynamic ranges, RangeRow row)
      {
         int count = (int)ranges.Count;
         for (int i = 0; i < count; i++)
         {
            dynamic range = ranges.Item[i];
            if ((int)range.ID == row.Id)
               return range;
            ServerSession.Release(range);
         }
         return null;
      }

      private void SavePerms_Click(object sender, RoutedEventArgs e)
      {
         // The range the card shows, which is the selected one unless a search has
         // hidden it while its permissions were being changed.
         RangeRow row = permsRow_;
         if (row == null)
            return;

         bool saved = SavePermissions_(row);

         // After the reload, which selects the range again and so reads its
         // permissions back: the pill says Saved until the next change - to a screen
         // reader as well, which heard nothing of the save: the keyboard stays on Save
         // permissions, and the pill is the one place the card says it.
         Reload();
         if (saved && permsRow_ != null && permsRow_.Id == row.Id)
         {
            permsSettled_ = SettingsPageState.Saved;
            ShowPermsState_();
            Announce.Request(PermsStatePill);
         }
      }

      /// <summary>
      /// Writes the card's boxes to <paramref name="row"/>'s range; true when the
      /// server took them, and the boxes are then what the range holds.
      /// </summary>
      private bool SavePermissions_(RangeRow row)
      {
         // Permissions that make an open relay are asked about first, whichever way
         // they are saved - Save permissions, the question before another range is
         // shown, the one before the range's dialog opens (review finding F229).
         if (RelayExposure.Of(row.LowerIP, row.UpperIP, PermLR.IsChecked is true, PermAuthLR.IsChecked is true, PermRR.IsChecked is true, PermAuthRR.IsChecked is true) == RelayExposure.Reach.Internet
             && !MessageBox.Confirm(RelayExposure.SaveQuestion(row.Name, OpenOnCard_()), L("Control Panel"), L("Sa_ve permissions"), destructive: true))
            return false;

         bool saved = false;
         dynamic ranges = ServerSession.Current.Application.Settings.SecurityRanges;
         try
         {
            dynamic range = FindRange(ranges, row);
            if (range == null)
            {
               // Deleted from elsewhere since the list was read: said, rather than a
               // Save that does nothing.
               MessageBox.Show(F("Could not save the permissions: {0}", L("the range is no longer on the server.")), L("Control Panel"));
               return false;
            }

            range.AllowSMTPConnections = PermSmtp.IsChecked is true;
            range.AllowIMAPConnections = PermImap.IsChecked is true;
            range.AllowPOP3Connections = PermPop3.IsChecked is true;
            range.RequireSSLTLSForAuth = PermTlsAuth.IsChecked is true;
            range.AllowDeliveryFromLocalToLocal = PermLL.IsChecked is true;
            range.AllowDeliveryFromLocalToRemote = PermLR.IsChecked is true;
            range.AllowDeliveryFromRemoteToLocal = PermRL.IsChecked is true;
            range.AllowDeliveryFromRemoteToRemote = PermRR.IsChecked is true;
            range.RequireSMTPAuthLocalToLocal = PermAuthLL.IsChecked is true;
            range.RequireSMTPAuthLocalToExternal = PermAuthLR.IsChecked is true;
            range.RequireSMTPAuthExternalToLocal = PermAuthRL.IsChecked is true;
            range.RequireSMTPAuthExternalToExternal = PermAuthRR.IsChecked is true;
            range.Save();
            ServerSession.Release(range);
            saved = true;
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            MessageBox.Show(F("Could not save the permissions: {0}", ServerSession.DescribeComError(ex)), L("Control Panel"));
         }
         finally
         {
            ServerSession.Release(ranges);
         }

         if (saved)
         {
            perms_.Take(PermissionValues_());
            // Its permissions chosen, a range just added needs the notice no more.
            if (row.Id == addedRangeId_)
               addedText_ = null;
         }
         return saved;
      }

      private void Edit_Click(object sender, RoutedEventArgs e)
      {
         if (RangeGrid.SelectedItem is not RangeRow row)
         {
            MessageBox.Show(L("Select a range first."), L("Control Panel"));
            return;
         }

         // The dialog edits the same permissions as the card, from what the server
         // holds: changes on the card are saved or let go first, or the dialog is
         // not opened.
         if (HasUnsavedChanges)
         {
            RangeRow edited = permsRow_;
            if (!UnsavedChanges.MayLeave(true,
                   () => MessageBox.AskUnsaved(UnsavedChanges.PermissionsQuestion(edited.Name), L("Control Panel")),
                   () => SavePermissions_(edited)))
               return;

            ForgetPermissionChanges_();
         }

         var dialog = new IPRangeDialog(Window.GetWindow(this), row.Id);
         dialog.ShowDialog();
         // Saved there, a range just added has had its permissions chosen.
         if (dialog.Saved && row.Id == addedRangeId_)
            addedText_ = null;
         Reload();
      }

      private void Add_Click(object sender, RoutedEventArgs e)
      {
         string name = NewName.Text.Trim();
         string lower = NewLower.Text.Trim();
         string upper = NewUpper.Text.Trim();

         // Each value refused is said on its box, all on one press, with the
         // keyboard in the first, where one message box named three boxes and a
         // priority that was no number became 15 without a word (review findings
         // F129 and F212). The ends of the range are addresses the server reads,
         // of one family, the lower not above the upper.
         DialogFields.ClearErrors(NewNameRow, NewLowerRow, NewUpperRow, NewPriorityRow);
         if (name.Length == 0)
            NewNameRow.Error = L("Enter a name for the range.");
         if (!InlineFormChecks.IsAddress(lower))
            NewLowerRow.Error = L("Enter the lower address of the range, such as 10.0.0.0.");
         if (!InlineFormChecks.IsAddress(upper))
            NewUpperRow.Error = L("Enter the upper address of the range, such as 10.0.0.255.");
         else if (InlineFormChecks.OrderOf(lower, upper) is InlineFormChecks.RangeOrder order && order != InlineFormChecks.RangeOrder.InOrder)
            NewUpperRow.Error = order == InlineFormChecks.RangeOrder.MixedFamilies
               ? L("The upper address must be of the same kind as the lower: both IPv4, or both IPv6.")
               : L("The upper address must not be below the lower.");
         // The range dialog's sentence for its priority: that the higher wins is
         // the page's description, just above, and in the form's narrow column at
         // 760 pixels "...; the higher number wins." ran to seven lines in German.
         if (!InlineFormChecks.TryWholeNumber(NewPriority.Text, int.MinValue, int.MaxValue, out int priority))
            NewPriorityRow.Error = L("Enter a whole number.");
         if (DialogFields.ShowFirstError(NewNameRow, NewLowerRow, NewUpperRow, NewPriorityRow))
            return;

         int addedId;
         dynamic ranges = ServerSession.Current.Application.Settings.SecurityRanges;
         try
         {
            dynamic range = ranges.Add();
            range.Name = name;
            range.LowerIP = lower;
            range.UpperIP = upper;
            range.Priority = priority;
            range.AllowSMTPConnections = true;
            range.AllowIMAPConnections = true;
            range.AllowPOP3Connections = true;
            range.Save();
            addedId = (int)range.ID;
            ServerSession.Release(range);
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            MessageBox.Show(F("Could not add the range: {0}", ServerSession.DescribeComError(ex)), L("Control Panel"));
            return;
         }
         finally
         {
            ServerSession.Release(ranges);
         }

         NewName.Text = NewLower.Text = NewUpper.Text = "";
         Reload();
         if (ReadFailure.IsShown(Notice))
            return;

         // The new range selected, its card in view with what to do next and the
         // keyboard on its first box: it may connect but send no mail until it is
         // given a direction, where nothing was selected and the card went on
         // showing another range (review finding F230) - the palette's "Let a device
         // send mail" leads here. A search typed in before that would leave it out
         // of the list is cleared. With changes to another range on the card, the
         // question about them comes first, and the notice once the card shows it.
         if (RangeGrid.ItemsSource is List<RangeRow> rows && rows.Find(r => r.Id == addedId) is RangeRow added)
         {
            ListSearchState.Reveal(SearchBar, RangeGrid, added);
            addedRangeId_ = addedId;
            addedText_ = F("{0} added. Addresses in the range may connect, but send no mail until you choose what they may deliver and Save permissions.", name);
            RangeGrid.SelectedItem = added;
            if (permsRow_ != null && permsRow_.Id == addedId)
            {
               Announce.Request(AddedNotice);
               RangeGrid.ScrollIntoView(added);
               // After the list has brought the row into its own view - and the
               // page's, which follows - the card's top, its title and the notice,
               // at the top of the page's view: BringIntoView showed a tall card's
               // foot in a short window, the notice scrolled out above. The keyboard
               // goes to the first box, under the notice, which a box further down
               // would scroll out of a short window again.
               Dispatcher.BeginInvoke(System.Windows.Threading.DispatcherPriority.Loaded, new Action(() =>
               {
                  Point top = PermCard.TranslatePoint(new Point(0, 0), PageScroll);
                  PageScroll.ScrollToVerticalOffset(PageScroll.VerticalOffset + top.Y);
                  PermSmtp.Focus();
               }));
            }
         }
      }

      /// <summary>
      /// Settings.SecurityRanges.SetDefault() over COM: every range is replaced with
      /// the standard two - "My computer" (127.0.0.1, priority 30) and "Internet"
      /// (0.0.0.0-255.255.255.255, priority 10). Another classic-Administrator action
      /// that lost its button when that tool was retired. The server's failure text
      /// is shown verbatim after a prefix that says what failed; it explains that the
      /// old ranges are gone even when writing the new ones failed, which is the
      /// state an administrator most needs to know about.
      /// </summary>
      private void RestoreDefaults_Click(object sender, RoutedEventArgs e)
      {
         if (!MessageBox.Confirm(
                L("Replace all IP ranges with the defaults?\n\nEvery custom range - including any auto-ban entries - will be removed and replaced with 'My computer' (127.0.0.1) and 'Internet' (everything else). This changes who may connect and relay, immediately."),
                L("Control Panel"), L("_Replace all ranges"), destructive: true))
            return;

         dynamic ranges = ServerSession.Current.Application.Settings.SecurityRanges;
         try
         {
            ranges.SetDefault();
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            MessageBox.Show(F("Could not restore the default IP ranges: {0}", ServerSession.DescribeComError(ex)), L("Control Panel"), MessageBoxButton.OK, MessageBoxImage.Error);
         }
         finally
         {
            ServerSession.Release(ranges);
         }

         Reload();
      }

      private void Delete_Click(object sender, RoutedEventArgs e)
      {
         if (RangeGrid.SelectedItem is not RangeRow row)
            return;

         if (!MessageBox.Confirm(Confirmations.DeleteIpRange(row.Name, row.LowerIP, row.UpperIP), L("Control Panel"), L("_Delete range"), destructive: true))
            return;

         dynamic ranges = ServerSession.Current.Application.Settings.SecurityRanges;
         try
         {
            dynamic range = FindRange(ranges, row);
            if (range != null)
            {
               range.Delete();
               ServerSession.Release(range);
            }
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            MessageBox.Show(F("Could not delete the range: {0}", ServerSession.DescribeComError(ex)), L("Control Panel"));
         }
         finally
         {
            ServerSession.Release(ranges);
         }

         Reload();
      }
   }
}
