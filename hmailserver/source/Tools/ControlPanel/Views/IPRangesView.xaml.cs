// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Linq;
using System.Windows;
using System.Windows.Controls;
using hMailServer.ControlPanel.Services;
using MessageBox = hMailServer.ControlPanel.Views.Dialogs;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Views
{
   public partial class IPRangesView : UserControl, IPageLifecycle
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

         // What a screen reader says for the row: its cells, not the name of
         // this class, which is what a grid row is announced as otherwise.
         public override string ToString() => Name + ", " + LowerIP + " - " + UpperIP;
      }

      public IPRangesView()
      {
         InitializeComponent();

         // Properties and Delete act on the selected range: live with nothing
         // selected, one answered with a message box and the other with nothing.
         SelectionGate.Bind(RangeGrid, PropertiesButton, DeleteRangeButton);

         // Every box follows the card's pill: Unsaved changes from the first change.
         foreach (CheckBox box in PermissionBoxes())
         {
            box.Checked += (s, e) => ShowPermsState_();
            box.Unchecked += (s, e) => ShowPermsState_();
         }

         ShowNoRange();

         // Enter and F2 open the selected range, Delete deletes it after asking,
         // Insert goes to the add form, and Enter in the form adds the range;
         // Escape empties its three boxes and leaves the priority (Views/GridKeys, FormSubmit).
         GridKeys.Wire(RangeGrid, GridKeys.Pressing(PropertiesButton), GridKeys.Pressing(DeleteRangeButton), GridKeys.Focusing(NewName));
         FormSubmit.Bind((UIElement)AddRangeButton.Parent, AddRangeButton, NewName, NewLower, NewUpper);
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

      /// <summary>
      /// The page as it is opened: read from the server, the card's boxes included.
      /// Changes left on the card when the page was left were discarded by leaving it
      /// (MainWindow asked), so they are not kept.
      /// </summary>
      public void OnEnter()
      {
         ForgetPermissionChanges_();
         Reload();
      }

      public void OnLeave()
      {
      }

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
            dynamic ranges = ServerSession.Current.Application.Settings.SecurityRanges;
            try
            {
               int count = (int)ranges.Count;
               for (int i = 0; i < count; i++)
               {
                  dynamic range = ranges.Item[i];
                  rows.Add(new RangeRow
                  {
                     Id = (int)range.ID,
                     Name = (string)range.Name,
                     LowerIP = (string)range.LowerIP,
                     UpperIP = (string)range.UpperIP,
                     Priority = (int)range.Priority,
                     Smtp = (bool)range.AllowSMTPConnections ? L("Yes") : L("No"),
                     Imap = (bool)range.AllowIMAPConnections ? L("Yes") : L("No"),
                     Pop3 = (bool)range.AllowPOP3Connections ? L("Yes") : L("No")
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
            RangeGrid.ItemsSource = rows;
            ListSearch.Apply(RangeGrid, SearchBar.SearchText);
            StatusText.Show(EmptyStatus, null, rows.Count, null, L("No IP ranges defined yet."));

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
      }

      private IEnumerable<CheckBox> PermissionBoxes()
         => new[] { PermSmtp, PermImap, PermPop3, PermTlsAuth, PermLL, PermLR, PermRL, PermRR };

      private void PermGrid_SizeChanged(object sender, SizeChangedEventArgs e)
         => PermGrid.Columns = e.NewSize.Width >= 4 * PermColumnMinWidth ? 4 : 2;

      private void Search_TextChanged(object sender, EventArgs e)
         => ListSearch.Apply(RangeGrid, SearchBar.SearchText);

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
            PermGrid.IsEnabled = true;
            SavePermsButton.IsEnabled = true;
            ServerSession.Release(range);
            permsRow_ = row;
            perms_.Take(PermissionValues_());
            permsSettled_ = SettingsPageState.Clean;
            ShowPermsState_();
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
         // permissions back: the pill says Saved until the next change.
         Reload();
         if (saved && permsRow_ != null && permsRow_.Id == row.Id)
         {
            permsSettled_ = SettingsPageState.Saved;
            ShowPermsState_();
         }
      }

      /// <summary>
      /// Writes the card's boxes to <paramref name="row"/>'s range; true when the
      /// server took them, and the boxes are then what the range holds.
      /// </summary>
      private bool SavePermissions_(RangeRow row)
      {
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
            perms_.Take(PermissionValues_());
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

         new IPRangeDialog(Window.GetWindow(this), row.Id).ShowDialog();
         Reload();
      }

      private void Add_Click(object sender, RoutedEventArgs e)
      {
         string name = NewName.Text.Trim();
         string lower = NewLower.Text.Trim();
         string upper = NewUpper.Text.Trim();

         if (name.Length == 0 || lower.Length == 0 || upper.Length == 0)
         {
            MessageBox.Show(L("Name, lower IP and upper IP are required."), L("Control Panel"));
            return;
         }

         if (!int.TryParse(NewPriority.Text.Trim(), out int priority))
            priority = 15;

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
                L("Replace ALL IP ranges with the defaults?\n\nEvery custom range - including any auto-ban entries - will be removed and replaced with 'My computer' (127.0.0.1) and 'Internet' (everything else). This changes who may connect and relay, immediately."),
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
