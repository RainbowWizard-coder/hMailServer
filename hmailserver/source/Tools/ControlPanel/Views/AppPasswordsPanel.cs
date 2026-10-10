// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Windows;
using System.Windows.Controls;
using hMailServer.ControlPanel.Services;
using hMailServer.ControlPanel.Views.Scaffold;
using MessageBox = hMailServer.ControlPanel.Views.Dialogs;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Views
{
   /// <summary>
   /// One account's application-specific passwords, embedded in the account editor.
   ///
   /// An app password is a credential for a single mail client, revocable on its own
   /// without changing the password the account holder actually knows. It is the
   /// prerequisite for per-account two-factor authentication, because an IMAP or POP3
   /// client has nowhere to type a code.
   ///
   /// A generated password is shown ONCE, in the card below the list, and there is no
   /// "show" action anywhere on this page - the server stores only a hash, so offering
   /// one would be promising something the store cannot deliver. The list is therefore
   /// about the things it CAN answer: what each credential is for, when it was issued,
   /// and whether it has ever been used. That last column is the useful one: a
   /// credential that has never authenticated is one that can be deleted with no
   /// consequences at all.
   /// </summary>
   public class AppPasswordsPanel : UserControl
   {
      private readonly string domainName_;
      private readonly string address_;

      /// <summary>
      /// Four facts per credential, so it is a table.
      ///
      /// It used to be a ListBox of "{0}{1}   issued {2}   {3}" - the name, a
      /// parenthesised "(revoked)" only when it applied, the issue date, and either
      /// "never used" or "last used ...". Because the revoked marker was present or
      /// absent rather than a column, the dates after it started in a different
      /// place on every row, and whether a credential was live had to be read out of
      /// the middle of a sentence rather than seen down a column. That is the one
      /// thing anybody comes to this tab to check.
      /// </summary>
      private readonly DataGrid list_ = new()
      {
         AutoGenerateColumns = false,
         IsReadOnly = true,
         CanUserAddRows = false,
         CanUserDeleteRows = false,
         CanUserResizeRows = false,
         SelectionMode = DataGridSelectionMode.Single,
         SelectionUnit = DataGridSelectionUnit.FullRow,
         HeadersVisibility = DataGridHeadersVisibility.Column,
         GridLinesVisibility = DataGridGridLinesVisibility.None,
         BorderThickness = new Thickness(0),
         Background = System.Windows.Media.Brushes.Transparent,
         RowBackground = System.Windows.Media.Brushes.Transparent,
         MinHeight = 160
      };

      private readonly TextBox newName_ = new() { VerticalContentAlignment = VerticalAlignment.Center };
      private FieldRow nameRow_;
      private readonly TextBlock status_ = new() { FontSize = Typography.Caption, Margin = new Thickness(0, 10, 0, 0), TextWrapping = TextWrapping.Wrap };

      private readonly Card issuedCard_ = new() { Padding = new Thickness(12), Margin = new Thickness(0, 12, 0, 0), Visibility = Visibility.Collapsed };
      private readonly TextBox issuedValue_ = new()
      {
         IsReadOnly = true,
         FontFamily = new System.Windows.Media.FontFamily(Typography.MonoFontFamily),
         FontSize = Typography.Mono,
         BorderThickness = new Thickness(0),
         Background = System.Windows.Media.Brushes.Transparent
      };

      public AppPasswordsPanel(string domainName, string address)
      {
         domainName_ = domainName;
         address_ = address;

         Build();
      }

      /// <summary>Loads the list. Called when the account editor opens the tab.</summary>
      public void Reload()
      {
         var rows = new System.Collections.Generic.List<CredentialRow>();
         list_.ItemsSource = rows;

         if (string.IsNullOrEmpty(address_))
         {
            // A new account has no address to hang credentials off yet. The panel is
            // built when the dialog opens, so it does not learn the address the moment
            // the account is saved - say so plainly rather than failing on the first
            // button press.
            status_.Text = L("Save the account and reopen it - an app password belongs to an account that exists.");
            newName_.IsEnabled = false;
            return;
         }

         try
         {
            dynamic passwords = OpenCollection();
            try
            {
               int count = (int)passwords.Count;

               for (int i = 0; i < count; i++)
               {
                  dynamic item = passwords[i];
                  try
                  {
                     string lastUsed = (string)item.LastUsedTime;

                     rows.Add(new CredentialRow
                     {
                        Name = (string)item.Name,
                        Active = (bool)item.Active,
                        CreatedTime = (string)item.CreatedTime,
                        LastUsed = lastUsed,
                        Id = (int)item.ID
                     });
                  }
                  finally
                  {
                     ServerSession.Release(item);
                  }
               }

               // ItemsSource was set to the list before it was filled, so the grid
               // has to be told the contents changed - a plain List raises nothing.
               list_.Items.Refresh();

               status_.Text = count == 0
                  ? L("No app passwords. Create one per mail client, so that losing a device revokes only that device.")
                  : Plural(count, "{0} app password.", "{0} app passwords.");
            }
            finally
            {
               ServerSession.Release(passwords);
            }
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            status_.Text = ServerSession.DescribeComError(ex);
         }
      }

      private dynamic OpenCollection()
      {
         dynamic domains = ServerSession.Current.Application.Domains;
         dynamic domain = domains.ItemByName[domainName_];
         dynamic accounts = domain.Accounts;
         dynamic account = accounts.ItemByAddress[address_];
         dynamic passwords = account.AppPasswords;

         ServerSession.Release(account);
         ServerSession.Release(accounts);
         ServerSession.Release(domain);
         ServerSession.Release(domains);

         return passwords;
      }

      /// <summary>One app password as the tab shows it.</summary>
      private sealed class CredentialRow
      {
         public string Name { get; init; }
         public bool Active { get; init; }
         public string CreatedTime { get; init; }
         public string LastUsed { get; init; }
         public int Id { get; init; }

         /// <summary>
         /// A word, not a blank. "Active" earns its column: an empty cell reads as
         /// missing data, and this is the field somebody is checking when they come
         /// here after losing a phone.
         /// </summary>
         public string State => Active ? L("Active") : L("Revoked");

         // The name and the state, then the two dates with their columns' names.
         public override string ToString() => RowSpeech.Of(Name, State,
            RowSpeech.Labelled(L("Issued"), CreatedTime), RowSpeech.Labelled(L("Last used"), LastUsed));

         public string LastUsedText => string.IsNullOrEmpty(LastUsed) ? L("Never") : LastUsed;
      }

      /// <summary>
      /// Name, whether it still works, when it was issued, when it was last used.
      ///
      /// State gets its own column rather than a parenthesis inside the name,
      /// because whether a credential is live is the question this tab answers and
      /// it should be readable down a column instead of found mid-sentence.
      /// </summary>
      private void BuildColumns()
      {
         list_.Columns.Add(new DataGridTextColumn
         {
            Header = L("Name"),
            Binding = new System.Windows.Data.Binding(nameof(CredentialRow.Name)),
            Width = new DataGridLength(2, DataGridLengthUnitType.Star)
         });

         list_.Columns.Add(new DataGridTextColumn
         {
            Header = L("State"),
            Binding = new System.Windows.Data.Binding(nameof(CredentialRow.State)),
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
            Header = L("Issued"),
            Binding = new System.Windows.Data.Binding(nameof(CredentialRow.CreatedTime)),
            Width = DataGridLength.Auto
         });

         list_.Columns.Add(new DataGridTextColumn
         {
            Header = L("Last used"),
            Binding = new System.Windows.Data.Binding(nameof(CredentialRow.LastUsedText)),
            Width = DataGridLength.Auto
         });
      }

      private void Build()
      {
         // No inset of the panel's own: the account dialog's other tabs start at
         // the dialog's inset, and this one started four pixels further in.
         var root = new StackPanel { Margin = new Thickness(0, 8, 0, 0) };

         var hint = new TextBlock
         {
            Text = L("A separate password for one mail client. Losing a laptop then means revoking one line here, rather than changing the account password and reconfiguring every other client."),
            TextWrapping = TextWrapping.Wrap,
            Margin = new Thickness(0, 0, 0, 12)
         };
         hint.SetResourceReference(ForegroundProperty, "TextFillColorSecondaryBrush");
         root.Children.Add(hint);

         // The name captioned above its box, as every other field of the dialog
         // is, with Create beside it; the three buttons that act on the selected
         // row go under the list, where they act. In one row - an inline "Name",
         // a 220-pixel box, Create, Revoke / restore, Delete and Refresh, each at
         // least 92 pixels - Delete lost its right edge in the dialog's 592
         // pixels and Refresh was not on the tab at all.
         var create = new Grid { Margin = new Thickness(0, 0, 0, 10) };
         create.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
         create.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
         newName_.Text = "";
         nameRow_ = DialogFields.Field(L("_Name"), newName_);
         nameRow_.Margin = new Thickness(0, 0, 8, 0);
         create.Children.Add(nameRow_);
         // Secondary: the panel is a tab of the account dialog, whose Save is the
         // one Primary button (review finding F195).
         Wpf.Ui.Controls.Button createButton = MakeButton(L("Cr_eate"), Wpf.Ui.Controls.ControlAppearance.Secondary, (_, _) => Create());
         createButton.Margin = new Thickness(0);
         createButton.VerticalAlignment = VerticalAlignment.Bottom;
         Grid.SetColumn(createButton, 1);
         create.Children.Add(createButton);
         root.Children.Add(create);

         var card = new Card { Padding = new Thickness(DesignTokens.ListCardPadding) };
         BuildColumns();

         card.Content = list_;
         root.Children.Add(card);

         // Wraps where the three do not fit on one line, rather than running off
         // the tab. Revoke / restore and Delete act on the selected row, so they
         // can be pressed only while there is one; Delete is the list pages' one
         // Delete style, red words rather than a filled red button.
         var rowActions = new WrapPanel { HorizontalAlignment = HorizontalAlignment.Right, Margin = new Thickness(0, 8, 0, 0) };
         Wpf.Ui.Controls.Button revoke = MakeButton(L("Re_voke / restore"), Wpf.Ui.Controls.ControlAppearance.Secondary, (_, _) => ToggleActive());
         rowActions.Children.Add(revoke);
         Wpf.Ui.Controls.Button delete = MakeButton(L("_Delete"), Wpf.Ui.Controls.ControlAppearance.Secondary, (_, _) => Delete());
         ButtonStyles.DangerText(delete);
         rowActions.Children.Add(delete);
         rowActions.Children.Add(MakeButton(L("_Refresh"), Wpf.Ui.Controls.ControlAppearance.Secondary, (_, _) => Reload()));
         SelectionGate.Bind(list_, revoke, delete);
         root.Children.Add(rowActions);

         var issued = new StackPanel();
         var issuedTitle = new TextBlock
         {
            Text = L("Copy this now - it is not stored and cannot be shown again"),
            FontWeight = FontWeights.SemiBold,
            Margin = new Thickness(0, 0, 0, 6),
            TextWrapping = TextWrapping.Wrap
         };
         issued.Children.Add(issuedTitle);
         issued.Children.Add(issuedValue_);
         var copy = MakeButton(L("_Copy"), Wpf.Ui.Controls.ControlAppearance.Secondary, (_, _) => CopyIssued());
         copy.Margin = new Thickness(0, 8, 0, 0);
         copy.HorizontalAlignment = HorizontalAlignment.Left;
         issued.Children.Add(copy);
         issuedCard_.Content = issued;
         root.Children.Add(issuedCard_);

         status_.SetResourceReference(ForegroundProperty, "TextFillColorSecondaryBrush");
         root.Children.Add(status_);

         Content = root;
      }

      private static Wpf.Ui.Controls.Button MakeButton(string text, Wpf.Ui.Controls.ControlAppearance appearance, RoutedEventHandler onClick)
      {
         var b = new Wpf.Ui.Controls.Button { Content = text, Appearance = appearance, Margin = new Thickness(8, 0, 0, 0), MinWidth = 92 };
         b.Click += onClick;
         return b;
      }

      private void Create()
      {
         if (string.IsNullOrEmpty(address_))
         {
            status_.Text = L("Save the account and reopen it first.");
            return;
         }

         string name = newName_.Text.Trim();

         // On the field it is about, with the keyboard put in it, rather than
         // in the status line under the list.
         DialogFields.ClearErrors(nameRow_);
         if (name.Length == 0)
         {
            DialogFields.ShowError(nameRow_, L("Give it a name first - it is what you will recognise this credential by when you come to revoke it."));
            return;
         }

         try
         {
            dynamic passwords = OpenCollection();
            try
            {
               dynamic item = passwords.Add();
               try
               {
                  item.Name = name;
                  string clearText = (string)item.Generate();
                  item.Save();

                  issuedValue_.Text = clearText;
                  issuedCard_.Visibility = Visibility.Visible;
                  newName_.Text = "";
               }
               finally
               {
                  ServerSession.Release(item);
               }
            }
            finally
            {
               ServerSession.Release(passwords);
            }

            Reload();
            status_.Text = F("Created \"{0}\". Enter it in the client as that account's password.", name);
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            status_.Text = ServerSession.DescribeComError(ex);
         }
      }

      private int SelectedId()
      {
         return list_.SelectedItem is CredentialRow row ? row.Id : 0;
      }

      private void ToggleActive()
      {
         int id = SelectedId();
         if (id == 0)
         {
            status_.Text = L("Select an app password first.");
            return;
         }

         try
         {
            dynamic passwords = OpenCollection();
            try
            {
               dynamic item = passwords.ItemByDBID[id];
               try
               {
                  item.Active = !(bool)item.Active;
                  item.Save();
               }
               finally
               {
                  ServerSession.Release(item);
               }
            }
            finally
            {
               ServerSession.Release(passwords);
            }

            Reload();
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            status_.Text = ServerSession.DescribeComError(ex);
         }
      }

      private void Delete()
      {
         int id = SelectedId();
         if (id == 0)
         {
            status_.Text = L("Select an app password first.");
            return;
         }

         string name = list_.SelectedItem is CredentialRow row ? row.Name : "";
         if (!MessageBox.Confirm(
               F("Delete the app password {0}? The client using it stops connecting at once.", name),
               L("Delete app password"), L("_Delete"), destructive: true))
            return;

         try
         {
            dynamic passwords = OpenCollection();
            try
            {
               passwords.DeleteByDBID(id);
            }
            finally
            {
               ServerSession.Release(passwords);
            }

            Reload();
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            status_.Text = ServerSession.DescribeComError(ex);
         }
      }

      private void CopyIssued()
      {
         if (ClipboardWriter.TrySetText(issuedValue_.Text, out string error))
            status_.Text = L("Copied.");
         else
            status_.Text = L("Could not copy: ") + error;
      }
   }
}
