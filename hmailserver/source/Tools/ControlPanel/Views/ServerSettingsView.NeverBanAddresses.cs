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
   public partial class ServerSettingsView
   {
      /// <summary>
      /// The never-ban list by address, AutoBanNeverBan, as a table like the list by
      /// reverse name below it (review finding F231): each address or CIDR block on its
      /// row with what it spares - one address, the run a block covers, or that the
      /// server ignores it - added and removed one at a time with its own buttons,
      /// which write at once. It was one comma-separated box saved with Save beside a
      /// table whose buttons wrote at once, an entry the server could not read was
      /// said only in its error log, and what a block spared was left to arithmetic.
      ///
      /// A row of the page that writes on its own buttons, not on Save: the list is a
      /// setting of the server's store (IniFeatureStore.Write, over COM), in force for
      /// the next ban the moment it is written (applies now). Each button reads the
      /// list again before it writes, so that an entry written meanwhile elsewhere is
      /// kept.
      /// </summary>
      private sealed class NeverBanAddressRows : ComSetting, INotPersisted
      {
         public IniFeatureStore IniStore;

         // The grid, its empty state, the status notice and the buttons of the editor
         // last built: made anew by every BuildEditor, as the never-ban names' are,
         // since BuildUi builds the editors again on every visit and every Reload.
         private DataGrid grid_;
         private Scaffold.EmptyState empty_;
         private Scaffold.InlineNotice ignored_;
         private Scaffold.InlineNotice status_;
         private Wpf.Ui.Controls.Button add_;
         private Wpf.Ui.Controls.Button remove_;

         public override bool WantsInitialValue => false;

         protected override FrameworkElement BuildEditor(object value)
         {
            grid_ = new DataGrid
            {
               AutoGenerateColumns = false,
               IsReadOnly = true,
               CanUserAddRows = false,
               CanUserDeleteRows = false,
               SelectionMode = DataGridSelectionMode.Single,
               MinHeight = 120,
               MaxHeight = 280
            };
            ScrollViewer.SetHorizontalScrollBarVisibility(grid_, ScrollBarVisibility.Disabled);
            grid_.Columns.Add(Column_(L("Address or block"), nameof(NeverBanAddressRow.Entry), 2));
            grid_.Columns.Add(Column_(L("Covers"), nameof(NeverBanAddressRow.Covers), 3));
            Describe(grid_, "never-ban-addresses");

            // The entries the server ignores, said for as long as they are listed, apart
            // from what the last press did.
            ignored_ = new Scaffold.InlineNotice
            {
               Level = StatusLevel.Warning,
               Margin = new Thickness(0, DesignTokens.Space.Sm, 0, 0),
               Visibility = Visibility.Collapsed
            };
            status_ = new Scaffold.InlineNotice
            {
               Margin = new Thickness(0, DesignTokens.Space.Sm, 0, 0),
               Visibility = Visibility.Collapsed
            };

            add_ = MakeButton_(L("A_dd address…"), "never-ban-address-add");
            add_.Click += (_, _) => Add_();

            remove_ = MakeButton_(L("Remo_ve address"), "never-ban-address-remove");
            ButtonStyles.DangerText(remove_);
            remove_.Click += (_, _) => Remove_();
            SelectionGate.Bind(grid_, remove_);
            // Delete on a row presses Remove address, Insert Add address..., as on
            // every list page (Views/GridKeys).
            GridKeys.Wire(grid_, null, GridKeys.Pressing(remove_), GridKeys.Pressing(add_));

            var buttons = new WrapPanel { Margin = new Thickness(0, DesignTokens.Space.Sm, 0, 0) };
            buttons.Children.Add(add_);
            buttons.Children.Add(remove_);

            // What an empty list means, under the column headers, with no glyph: the
            // grid is 120 pixels at its floor, as the never-ban names' is.
            empty_ = new Scaffold.EmptyState { Padding = new Thickness(12), Margin = new Thickness(0, 33, 0, 0), Visibility = Visibility.Collapsed };
            var gridHost = new Grid();
            gridHost.Children.Add(grid_);
            gridHost.Children.Add(empty_);

            var panel = new StackPanel();
            panel.Children.Add(gridHost);
            panel.Children.Add(buttons);
            panel.Children.Add(ignored_);
            panel.Children.Add(status_);

            Reload_();
            return panel;
         }

         public override object ReadEditor() => null;

         public override void Write(object owner, string property)
         {
         }

         private static DataGridTextColumn Column_(string header, string path, double stars)
            => GridStyles.Trimmed(new DataGridTextColumn
            {
               Header = header,
               Binding = new System.Windows.Data.Binding(path),
               Width = new DataGridLength(stars, DataGridLengthUnitType.Star)
            });

         private static Wpf.Ui.Controls.Button MakeButton_(string text, string id)
         {
            var button = new Wpf.Ui.Controls.Button
            {
               Content = text,
               Appearance = Wpf.Ui.Controls.ControlAppearance.Secondary,
               Margin = new Thickness(0, 0, DesignTokens.Space.Sm, DesignTokens.Space.Sm)
            };
            SetAid(button, id);
            return button;
         }

         private void Say_(StatusLevel level, string text)
         {
            status_.Level = level;
            status_.Text = text;
            status_.Visibility = Visibility.Visible;
            Announce.Request(status_);
         }

         /// <summary>The list as the server holds it now, or null and why it could not be read.</summary>
         private List<string> Read_(out string failure)
         {
            failure = null;
            if (IniStore == null)
            {
               failure = L("Sign in to a server first.");
               return null;
            }

            try
            {
               IniStore.ForgetFallback();
               string value = IniStore.Read(Path, "");
               if (IniStore.FallbackCause != null)
               {
                  // Read from this computer's copy of hMailServer.ini, which may not be
                  // what the server uses, and which a write cannot change.
                  failure = ServerSession.DescribeComError(IniStore.FallbackCause);
                  return null;
               }

               return NeverBanAddressList.Split(value);
            }
            catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
            {
               failure = ServerSession.DescribeComError(ex);
               return null;
            }
         }

         /// <summary>
         /// The rows read again; a list that could not be read says so in their place,
         /// with the buttons held; entries the server ignores are said at Warning.
         /// </summary>
         private void Reload_()
         {
            List<string> entries = Read_(out string failure);
            if (entries == null)
            {
               grid_.ItemsSource = Array.Empty<NeverBanAddressRow>();
               empty_.Visibility = Visibility.Collapsed;
               ignored_.Visibility = Visibility.Collapsed;
               add_.IsEnabled = false;
               Say_(StatusLevel.Critical, F("Could not read the never-ban list: {0}", failure));
               return;
            }

            List<NeverBanAddressRow> rows = NeverBanAddressList.Rows(NeverBanAddressList.Join(entries));
            grid_.ItemsSource = rows;
            add_.IsEnabled = true;
            status_.Visibility = Visibility.Collapsed;
            StatusText.Show(empty_, null, rows.Count, null, L("No addresses, so any address can be auto-banned."));

            string ignored = string.Join(", ", rows.Where(row => !row.Readable).Select(row => row.Entry));
            ignored_.Text = ignored.Length == 0 ? "" : F("The server ignores {0}: not an address or a CIDR block, so no address is spared for it.", ignored);
            ignored_.Visibility = ignored.Length == 0 ? Visibility.Collapsed : Visibility.Visible;
         }

         /// <summary>Writes the list, or says why it could not; true when it was written.</summary>
         private bool Write_(List<string> entries)
         {
            try
            {
               IniStore.Write(Path, NeverBanAddressList.Join(entries));
               return true;
            }
            catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
            {
               Reload_();
               Say_(StatusLevel.Critical, F("Could not save the never-ban list: {0}", ServerSession.DescribeComError(ex)));
               return false;
            }
         }

         private void Add_()
         {
            string added = AddNeverBanAddressDialog.Show(Window.GetWindow(grid_), typed =>
            {
               // Read again at the press, so that an entry written meanwhile is kept
               // and one already listed is said as such.
               List<string> entries = Read_(out string failure);
               if (entries == null)
                  return F("Could not read the never-ban list: {0}", failure);

               string refusal = NeverBanAddressList.Refusal(typed, entries);
               if (refusal != null)
                  return refusal;

               entries.Add(typed.Trim());
               try
               {
                  IniStore.Write(Path, NeverBanAddressList.Join(entries));
                  return null;
               }
               catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
               {
                  return F("Could not save the never-ban list: {0}", ServerSession.DescribeComError(ex));
               }
            });
            if (added == null)
               return;

            // In words of its own, not the list by reverse name's: several languages
            // call that list by what it holds, its names.
            Reload_();
            Say_(StatusLevel.Good, F("{0} is never auto-banned from the next ban on.", added));
         }

         private void Remove_()
         {
            if (grid_.SelectedItem is not NeverBanAddressRow selected)
               return;

            // Cancel on Enter, as for the list by reverse name: an address the entry
            // spared can be banned again from the next failed sign-in.
            if (!MessageBox.Confirm(
                  F("Remove {0} from the addresses that are never auto-banned?\n\nAn address it spared can be auto-banned again from the next ban on.", selected.Entry),
                  L("Control Panel"), L("Re_move"), destructive: true))
            {
               return;
            }

            List<string> entries = Read_(out string failure);
            if (entries == null)
            {
               Reload_();
               return;
            }

            List<string> kept = NeverBanAddressList.Without(entries, selected.Entry);
            if (kept.Count == entries.Count)
            {
               Reload_();
               Say_(StatusLevel.Information, F("{0} was not listed any more.", selected.Entry));
               return;
            }

            if (!Write_(kept))
               return;

            Reload_();
            Say_(StatusLevel.Good, F("{0} can be auto-banned again from the next ban on.", selected.Entry));
         }
      }

      /// <summary>
      /// Puts one address or CIDR block on the never-ban list by address. Its own class
      /// so that its Alt keys are its own scope and not the page's. What it refuses - an
      /// empty box, an entry the server would not read, one listed already, a write the
      /// server refused - is said under the box, in the box's words.
      /// </summary>
      private static class AddNeverBanAddressDialog
      {
         /// <summary>
         /// The entry as added, or null when nothing was. <paramref name="add"/> writes
         /// the typed entry and returns null, or returns why it did not.
         /// </summary>
         public static string Show(Window owner, Func<string, string> add)
         {
            var dialog = new FramedDialog { Owner = owner, Title = L("Add an address to the never-ban list") };

            var entry = new Wpf.Ui.Controls.TextBox { PlaceholderText = "192.0.2.10" }; // no-loc: an example address

            var body = new StackPanel();
            var entryRow = DialogFields.Field(L("A_ddress or block"), entry,
               L("One address, such as 192.0.2.10, or a block in CIDR form, such as 203.0.113.0/24 or 2001:db8::/32."));
            body.Children.Add(entryRow);

            string result = null;

            var ok = new Wpf.Ui.Controls.Button { Content = L("_Add"), Appearance = Wpf.Ui.Controls.ControlAppearance.Primary, MinWidth = 88 };
            ok.Click += (_, _) =>
            {
               DialogFields.ClearErrors(entryRow);
               string typed = entry.Text?.Trim() ?? "";
               string refusal = add(typed);
               if (refusal != null)
               {
                  DialogFields.ShowError(entryRow, refusal);
                  return;
               }

               result = typed;
               dialog.DialogResult = true;
               dialog.Close();
            };

            var cancel = new Wpf.Ui.Controls.Button { Content = L("Cancel"), MinWidth = 88 };
            cancel.Click += (_, _) => dialog.Close();

            dialog.Frame(L("Add an address to the never-ban list"), body, ok, cancel,
               L("A listed address is never auto-banned, and its failed sign-ins are not counted. It applies to the next ban."),
               width: DesignTokens.Dialog.DefaultWidth);
            // A refusal is said under the box without moving Add and Cancel.
            dialog.KeepRoomForFieldErrors();

            return dialog.ShowDialog() == true ? result : null;
         }
      }
   }
}
