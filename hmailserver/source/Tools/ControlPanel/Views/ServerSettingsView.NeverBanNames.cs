// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using hMailServer.ControlPanel.Services;
using MessageBox = hMailServer.ControlPanel.Views.Dialogs;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Views
{
   public partial class ServerSettingsView
   {
      /// <summary>
      /// The never-ban list by reverse name (GitLab work item #56), on the Auto-ban
      /// page beside AutoBanNeverBan: the patterns, added and removed one at a time
      /// as the domain white and black list's are, and a check of what the list
      /// would decide about one address.
      ///
      /// A row of the page that writes on its own buttons, not on Save: the list is
      /// a table of the server's (Settings.AutoBanNeverBanNames,
      /// AddAutoBanNeverBanName, RemoveAutoBanNeverBanName - members of an
      /// interface that already exists, so no registration step), and an entry is
      /// in force for the next ban the moment it is added. The check is there
      /// because the list depends on something the server cannot do for itself - a
      /// provider's reverse names must resolve back to its addresses - and an
      /// administrator should see whether a pattern will work before a ban shows
      /// whether it did.
      /// </summary>
      private sealed class NeverBanNameRows : ComSetting, INotPersisted
      {
         // The grid and the status line of the editor last built. Made anew by
         // every BuildEditor, not once: the definition is built once, with the
         // page, and BuildUi builds the editors again on every visit and every
         // Reload, so a grid made once and added to the second visit's panel was
         // "already the logical child of another element" - WPF threw, the shell
         // called it "Server unavailable", and the Auto-ban page was blank on
         // every second visit. The actions below act on whichever build is on
         // the page, which is these.
         private DataGrid grid_;
         private TextBlock status_;

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

            status_ = new TextBlock
            {
               TextWrapping = TextWrapping.Wrap,
               Visibility = Visibility.Collapsed
            };

            grid_.Columns.Add(Column_(L("Name or suffix"), nameof(NeverBanNameRow.Pattern), new DataGridLength(2, DataGridLengthUnitType.Star)));
            grid_.Columns.Add(Column_(L("Note"), nameof(NeverBanNameRow.Note), new DataGridLength(2, DataGridLengthUnitType.Star)));
            grid_.Columns.Add(Column_(L("Added"), nameof(NeverBanNameRow.Created), DataGridLength.Auto));
            Describe(grid_, "never-ban-names");

            // A polite live region whose accessible name is its text: with a fixed
            // name ("Never-ban list status") a reader said the label in place of
            // the outcome, and nothing raised the event that announces it at all.
            status_.Margin = new Thickness(0, DesignTokens.Space.Sm, 0, 0);
            status_.FontSize = Typography.Caption;
            AutomationProperties.SetLiveSetting(status_, AutomationLiveSetting.Polite);

            var add = MakeButton(L("_Add name…"), Wpf.Ui.Controls.ControlAppearance.Secondary, "never-ban-add");
            add.Click += (_, _) => Add_();

            var remove = MakeButton(L("Re_move name"), Wpf.Ui.Controls.ControlAppearance.Secondary, "never-ban-remove");
            ButtonStyles.DangerText(remove);
            remove.Click += (_, _) => Remove_();
            SelectionGate.Bind(grid_, remove);

            var check = MakeButton(L("C_heck an address…"), Wpf.Ui.Controls.ControlAppearance.Secondary, "never-ban-check");
            check.Click += (_, _) => Check_();

            var buttons = new WrapPanel { Margin = new Thickness(0, DesignTokens.Space.Sm, 0, 0) };
            buttons.Children.Add(add);
            buttons.Children.Add(remove);
            buttons.Children.Add(check);

            var panel = new StackPanel();
            panel.Children.Add(grid_);
            panel.Children.Add(buttons);
            panel.Children.Add(status_);

            Reload_();
            return panel;
         }

         public override object ReadEditor() => null;

         public override void Write(object owner, string property)
         {
         }

         private static DataGridTextColumn Column_(string header, string path, DataGridLength width)
            => new() { Header = header, Binding = new System.Windows.Data.Binding(path), Width = width };

         private static Wpf.Ui.Controls.Button MakeButton(string text, Wpf.Ui.Controls.ControlAppearance appearance, string id)
         {
            var button = new Wpf.Ui.Controls.Button
            {
               Content = text,
               Appearance = appearance,
               Margin = new Thickness(0, 0, DesignTokens.Space.Sm, DesignTokens.Space.Sm)
            };
            SetAid(button, id);
            return button;
         }

         private void Say_(string text, bool ok)
         {
            status_.Text = text;
            status_.Visibility = Visibility.Visible;
            status_.SetResourceReference(TextBlock.ForegroundProperty, ok ? "AppSuccessBrush" : "AppDangerBrush");
            Announce.Request(status_);
         }

         private void Reload_()
         {
            try
            {
               dynamic settings = ServerSession.Current.Application.Settings;
               string json;
               try
               {
                  json = (string)settings.AutoBanNeverBanNames;
               }
               finally
               {
                  ServerSession.Release((object)settings);
               }

               IReadOnlyList<NeverBanNameRow> rows = NeverBanNames.ParseList(json, out string error);
               grid_.ItemsSource = rows;

               if (error != null)
                  Say_(error, false);
            }
            catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
            {
               grid_.ItemsSource = Array.Empty<NeverBanNameRow>();
               Say_(ServerSession.DescribeComError(ex), false);
            }
         }

         private void Add_()
         {
            string added = AddNeverBanNameDialog.Show(Window.GetWindow(grid_));
            if (added == null)
               return;

            Reload_();
            Say_(F("{0} is on the never-ban list. It applies to the next ban.", added), true);
         }

         private void Remove_()
         {
            if (grid_.SelectedItem is not NeverBanNameRow selected)
            {
               Say_(L("Select an entry first."), false);
               return;
            }

            // YesNo, No on Enter, like every other confirmation that changes what
            // the server does: an address the pattern spared can be banned again
            // from the next failed logon.
            if (MessageBox.Show(
                  F("Remove {0} from the never-ban list?\n\nAn address it spared can be auto-banned again from the next ban on.", selected.Pattern),
                  L("Remove from the never-ban list"), MessageBoxButton.YesNo, MessageBoxImage.Question,
                  MessageBoxResult.No)
                != MessageBoxResult.Yes)
            {
               return;
            }

            try
            {
               bool removed;
               dynamic settings = ServerSession.Current.Application.Settings;
               try
               {
                  removed = (bool)settings.RemoveAutoBanNeverBanName(selected.Id);
               }
               finally
               {
                  ServerSession.Release((object)settings);
               }

               Reload_();
               Say_(removed ? F("{0} was removed from the never-ban list.", selected.Pattern) : F("{0} was not on the never-ban list any more.", selected.Pattern), true);
            }
            catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
            {
               Say_(ServerSession.DescribeComError(ex), false);
            }
         }

         private void Check_()
         {
            string address = DialogFields.PromptText(Window.GetWindow(grid_), L("Check an address"), L("IPv4 or IPv6 address"));
            if (string.IsNullOrWhiteSpace(address))
               return;

            try
            {
               string json;
               dynamic settings = ServerSession.Current.Application.Settings;
               try
               {
                  json = (string)settings.CheckAutoBanNeverBanName(address.Trim());
               }
               finally
               {
                  ServerSession.Release((object)settings);
               }

               NeverBanCheck verdict = NeverBanNames.ParseCheck(json, out string error);
               if (verdict == null)
               {
                  Say_(error, false);
                  return;
               }

               Say_(NeverBanNames.Describe(verdict), verdict.Spared);
            }
            catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
            {
               Say_(ServerSession.DescribeComError(ex), false);
            }
         }
      }

      /// <summary>
      /// Puts one pattern on the never-ban list: the name or suffix, and a note for
      /// whoever reads the list next. Its own class so that its Alt keys are its own
      /// scope and not the page's. The server's refusal - a public suffix, an
      /// address, a pattern already listed - is shown under the pattern, in its
      /// words.
      /// </summary>
      private static class AddNeverBanNameDialog
      {
         /// <summary>The pattern as added, or null when nothing was.</summary>
         public static string Show(Window owner)
         {
            var dialog = new FramedDialog { Owner = owner, Title = L("Add a name to the never-ban list") };

            var pattern = new Wpf.Ui.Controls.TextBox();
            AutomationProperties.SetName(pattern, L("Name or suffix"));

            var note = new Wpf.Ui.Controls.TextBox();
            AutomationProperties.SetName(note, L("Why it is listed"));

            var body = new StackPanel();
            var patternRow = DialogFields.Field(L("_Name or suffix"), pattern,
               L("mail.example.net is that name alone; .example.net is the name and every name beneath it. Never a public suffix such as .com, and never a cloud whose customers' machines carry the provider's name."));
            body.Children.Add(patternRow);
            body.Children.Add(DialogFields.Field(L("N_ote"), note, L("Kept with the entry, for whoever reads the list next.")));

            string result = null;

            var ok = new Wpf.Ui.Controls.Button { Content = L("_Add"), Appearance = Wpf.Ui.Controls.ControlAppearance.Primary, MinWidth = 88 };
            ok.Click += (_, _) =>
            {
               DialogFields.ClearErrors(patternRow);

               string typed = pattern.Text?.Trim() ?? "";
               if (typed.Length == 0)
               {
                  DialogFields.ShowError(patternRow, L("A name is required."));
                  return;
               }

               try
               {
                  dynamic settings = ServerSession.Current.Application.Settings;
                  try
                  {
                     settings.AddAutoBanNeverBanName(typed, note.Text ?? "");
                  }
                  finally
                  {
                     ServerSession.Release((object)settings);
                  }

                  result = typed;
                  dialog.DialogResult = true;
                  dialog.Close();
               }
               catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
               {
                  DialogFields.ShowError(patternRow, ServerSession.DescribeComError(ex));
               }
            };

            var cancel = new Wpf.Ui.Controls.Button { Content = L("Cancel"), MinWidth = 88 };
            cancel.Click += (_, _) => dialog.Close();

            dialog.Frame(L("Add a name to the never-ban list"), body, ok, cancel,
               L("An address is spared only when one of its reverse names is covered AND resolves back to the address, since anybody who holds an address can write its reverse name. It applies to the next ban."),
               width: 520);

            return dialog.ShowDialog() == true ? result : null;
         }
      }
   }
}
