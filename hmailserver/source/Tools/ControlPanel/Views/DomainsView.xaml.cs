// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Windows;
using System.Windows.Controls;
using hMailServer.ControlPanel.Services;
using MessageBox = hMailServer.ControlPanel.Views.Dialogs;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Views
{
   public partial class DomainsView : UserControl, IPageLifecycle
   {
      // The page's columns and cards keep a width below which what they hold runs
      // off their right edges: the domains column 280 pixels (its card 268, room
      // for a name beside Properties and Delete), the column of accounts, aliases
      // and lists 400, an alias or list card 280, and the new account's password
      // box 120 beside Generate and Create account. In the narrowest window
      // (760 by 520) the two columns were 268 and 232 pixels wide: Create account
      // ran off its card, and the alias and list cards, 104 and 116 pixels side by
      // side, had boxes 0 and 13 pixels wide.
      private const double DomainColumnMinWidth = 280;
      private const double DetailColumnMinWidth = 400;
      private const double ListCardMinWidth = 280;
      private const double PasswordMinWidth = 120;
      private static readonly double[] ColumnWeights = { 2, 3 };
      private static readonly double[] ColumnMinWidths = { DomainColumnMinWidth, DetailColumnMinWidth };
      private static readonly double[] ListCardWeights = { 1, 1 };
      private static readonly double[] ListCardMinWidths = { ListCardMinWidth, ListCardMinWidth };

      public DomainsView()
      {
         InitializeComponent();
      }

      public void OnEnter() => ReloadDomains();

      private void PageColumns_SizeChanged(object sender, SizeChangedEventArgs e)
         => ArrangeColumns(SideBySide.Fits(e.NewSize.Width, ColumnWeights, ColumnMinWidths));

      /// <summary>
      /// Two columns - the domains card beside the accounts card and the alias and
      /// list cards - where both keep their minimum widths, and otherwise one: the
      /// domains card goes above the accounts card, in the same FillPanel, so that
      /// the three share the window's height by their weights (two, three and two)
      /// rather than the domains card taking a window of its own.
      /// </summary>
      private void ArrangeColumns(bool twoColumns)
      {
         if (twoColumns == DomainColumn.Children.Contains(DomainCard))
            return;

         if (twoColumns)
         {
            DetailColumn.Children.Remove(DomainCard);
            DomainCard.Margin = new Thickness(0, 0, 12, 0);
            DomainColumn.Children.Add(DomainCard);
            DomainColumn.Visibility = Visibility.Visible;
            Grid.SetColumn(DetailColumn, 1);
            Grid.SetColumnSpan(DetailColumn, 1);
         }
         else
         {
            DomainColumn.Children.Remove(DomainCard);
            DomainCard.Margin = new Thickness(0, 0, 0, 12);
            DetailColumn.Children.Insert(0, DomainCard);
            DomainColumn.Visibility = Visibility.Collapsed;
            Grid.SetColumn(DetailColumn, 0);
            Grid.SetColumnSpan(DetailColumn, 2);
         }
      }

      /// <summary>The alias and list cards side by side where both keep 280 pixels, and otherwise one above the other.</summary>
      private void ListCards_SizeChanged(object sender, SizeChangedEventArgs e)
      {
         bool sideBySide = SideBySide.Fits(e.NewSize.Width, ListCardWeights, ListCardMinWidths);
         ListCards.Columns = sideBySide ? 2 : 1;
         AliasCard.Margin = sideBySide ? new Thickness(0, 0, 6, 0) : new Thickness(0, 0, 0, 6);
         DistCard.Margin = sideBySide ? new Thickness(6, 0, 0, 0) : new Thickness(0, 6, 0, 0);
      }

      /// <summary>
      /// Generate and Create account beside the password while the password box
      /// keeps 120 pixels, and on a line of their own under it where it would not,
      /// as a toolbar's actions wrap.
      /// </summary>
      private void AccountForm_SizeChanged(object sender, SizeChangedEventArgs e)
      {
         double buttons = 0;
         foreach (UIElement button in AccountButtons.Children)
            buttons += button.DesiredSize.Width;

         bool beside = e.NewSize.Width >= PasswordMinWidth + 8 + buttons;
         Grid.SetColumnSpan(PasswordRow, beside ? 1 : 2);
         PasswordRow.Margin = beside ? new Thickness(0, 0, 8, 0) : new Thickness(0);
         Grid.SetRow(AccountButtons, beside ? 1 : 2);
         Grid.SetColumn(AccountButtons, beside ? 1 : 0);
         Grid.SetColumnSpan(AccountButtons, beside ? 1 : 2);
         AccountButtons.Margin = beside ? new Thickness(0) : new Thickness(0, 8, 0, 0);
      }

      public void OnLeave()
      {
      }

      /// <summary>
      /// A domain in the left-hand list. The template dims an inactive domain's
      /// glyph and puts an "Inactive" pill after its name; ToString carries the
      /// same word to a screen reader, because a ListViewItem's accessible name
      /// falls back to ToString() on the bound object.
      /// </summary>
      public class DomainRow
      {
         public string Name { get; set; }
         public bool Active { get; set; }
         public override string ToString() => Active ? Name : F("{0} (inactive)", Name);
      }

      /// <summary>Same shape for the account list.</summary>
      public class AccountRow
      {
         public string Address { get; set; }
         public bool Active { get; set; }
         public override string ToString() => Active ? Address : F("{0} (inactive)", Address);
      }

      private string SelectedDomainName() => (DomainList.SelectedItem as DomainRow)?.Name;

      private void ReloadDomains(string select = null)
      {
         string previousSelection = select ?? SelectedDomainName();

         var rows = new List<DomainRow>();
         try
         {
            dynamic domains = ServerSession.Current.Application.Domains;
            try
            {
               int count = (int)domains.Count;
               for (int i = 0; i < count; i++)
               {
                  dynamic domain = domains.Item[i];
                  rows.Add(new DomainRow { Name = (string)domain.Name, Active = (bool)domain.Active });
                  ServerSession.Release(domain);
               }
            }
            finally
            {
               ServerSession.Release(domains);
            }
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            // Neither the domains of the last read nor "No domains yet." is a fact
            // this page has now: both go, the notice says what happened, with Try
            // again, Add domain waits for a read that works, and the three cards
            // that hang off the selected domain hang off nothing
            // (Services/ReadFailure).
            ReadFailure.Show(DomainNotice, DomainStatus, DomainList, L("Could not read the domains: "), ex, () => ReloadDomains(), AddDomainButton);
            ReloadAccounts();
            ReloadAliases();
            ReloadDistLists();
            AliasStatus.Visibility = Visibility.Collapsed;
            DistStatus.Visibility = Visibility.Collapsed;
            return;
         }

         ReadFailure.Clear(DomainNotice, AddDomainButton);
         DomainList.ItemsSource = rows;
         ListSearch.Apply(DomainList, DomainSearch.SearchText);
         StatusText.Show(DomainStatus, null, rows.Count, null, L("No domains yet. Add the first one to start receiving mail."));

         // Replacing ItemsSource clears the selection; put it back on the same
         // domain when it still exists, so a reload does not yank the accounts
         // pane over to whatever happens to be first.
         if (previousSelection != null)
            DomainList.SelectedItem = rows.Find(r => string.Equals(r.Name, previousSelection, StringComparison.OrdinalIgnoreCase));

         if (rows.Count > 0 && DomainList.SelectedIndex < 0)
            DomainList.SelectedIndex = 0;
      }

      private void DomainSearch_TextChanged(object sender, EventArgs e)
         => ListSearch.Apply(DomainList, DomainSearch.SearchText);

      private void AccountSearch_TextChanged(object sender, EventArgs e)
         => ListSearch.Apply(AccountList, AccountSearch.SearchText);

      private void DomainList_SelectionChanged(object sender, SelectionChangedEventArgs e)
      {
         ReloadAccounts();
         ReloadAliases();
         ReloadDistLists();
      }

      public class AliasRow
      {
         public string Name { get; set; }
         public string Display { get; set; }

         // The alias list is a ListView with an ItemTemplate, so the visible text
         // comes from the Display binding - but a ListViewItem's accessible name
         // falls back to ToString() on the bound object. Without this a screen
         // reader announces the type name for every alias. The domain, account and
         // distribution lists are bound to plain strings and never had the problem.
         public override string ToString() => Display;
      }

      private dynamic OpenSelectedDomain(dynamic domains)
      {
         string domainName = SelectedDomainName();
         return domainName == null ? null : domains.ItemByName[domainName];
      }

      private void ReloadAliases()
      {
         var rows = new List<AliasRow>();
         string error = null;
         if (DomainList.SelectedItem != null)
         {
            dynamic domains = ServerSession.Current.Application.Domains;
            try
            {
               dynamic domain = OpenSelectedDomain(domains);
               dynamic aliases = domain.Aliases;
               int count = (int)aliases.Count;
               for (int i = 0; i < count; i++)
               {
                  dynamic alias = aliases.Item[i];
                  rows.Add(new AliasRow
                  {
                     Name = (string)alias.Name,
                     Display = (string)alias.Name + "  →  " + (string)alias.Value
                  });
                  ServerSession.Release(alias);
               }
               ServerSession.Release(aliases);
               ServerSession.Release(domain);
            }
            catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
            {
               error = ServerSession.DescribeComError(ex);
            }
            finally
            {
               ServerSession.Release(domains);
            }
         }
         AliasList.ItemsSource = rows;
         SetListStatus(AliasStatus, rows.Count, error,
            DomainList.SelectedItem == null ? L("Select a domain to see its aliases.") : L("No aliases for this domain."),
            L("Couldn't load aliases: "));
      }

      private void ReloadDistLists()
      {
         var rows = new List<string>();
         string error = null;
         if (DomainList.SelectedItem != null)
         {
            dynamic domains = ServerSession.Current.Application.Domains;
            try
            {
               dynamic domain = OpenSelectedDomain(domains);
               dynamic lists = domain.DistributionLists;
               int count = (int)lists.Count;
               for (int i = 0; i < count; i++)
               {
                  dynamic list = lists.Item[i];
                  rows.Add((string)list.Address);
                  ServerSession.Release(list);
               }
               ServerSession.Release(lists);
               ServerSession.Release(domain);
            }
            catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
            {
               error = ServerSession.DescribeComError(ex);
            }
            finally
            {
               ServerSession.Release(domains);
            }
         }
         DistList.ItemsSource = rows;
         SetListStatus(DistStatus, rows.Count, error,
            DomainList.SelectedItem == null ? L("Select a domain to see its distribution lists.") : L("No distribution lists for this domain."),
            L("Couldn't load distribution lists: "));
      }

      // Shows a centered empty/error placeholder over a list when it has no rows.
      private static void SetListStatus(Scaffold.EmptyState status, int rowCount, string error, string emptyText, string errorPrefix)
         => StatusText.Show(status, null, rowCount, error, emptyText, errorPrefix);

      private void AddAlias_Click(object sender, RoutedEventArgs e)
      {
         string name = NewAliasName.Text.Trim();
         string value = NewAliasValue.Text.Trim();
         if (DomainList.SelectedItem == null || name.Length == 0 || value.Length == 0)
            return;

         dynamic domains = ServerSession.Current.Application.Domains;
         try
         {
            dynamic domain = OpenSelectedDomain(domains);
            dynamic aliases = domain.Aliases;
            dynamic alias = aliases.Add();
            alias.Name = name;
            alias.Value = value;
            alias.Active = true;
            alias.Save();
            ServerSession.Release(alias);
            ServerSession.Release(aliases);
            ServerSession.Release(domain);
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            MessageBox.Show(F("Could not create the alias: {0}", ex.Message), L("Control Panel"));
            return;
         }
         finally
         {
            ServerSession.Release(domains);
         }

         NewAliasName.Text = NewAliasValue.Text = "";
         ReloadAliases();
      }

      private void DeleteAlias_Click(object sender, RoutedEventArgs e)
      {
         string name = (sender as FrameworkElement)?.Tag as string;
         if (name == null || DomainList.SelectedItem == null)
            return;

         if (MessageBox.Show(F("Delete the alias {0}?", name), L("Control Panel"),
             MessageBoxButton.YesNo, MessageBoxImage.Warning) != MessageBoxResult.Yes)
            return;

         dynamic domains = ServerSession.Current.Application.Domains;
         try
         {
            dynamic domain = OpenSelectedDomain(domains);
            dynamic aliases = domain.Aliases;
            int count = (int)aliases.Count;
            for (int i = 0; i < count; i++)
            {
               dynamic alias = aliases.Item[i];
               if ((string)alias.Name == name)
               {
                  alias.Delete();
                  ServerSession.Release(alias);
                  break;
               }
               ServerSession.Release(alias);
            }
            ServerSession.Release(aliases);
            ServerSession.Release(domain);
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            MessageBox.Show(F("Could not delete the alias: {0}", ex.Message), L("Control Panel"));
         }
         finally
         {
            ServerSession.Release(domains);
         }

         ReloadAliases();
      }

      private void AddDistList_Click(object sender, RoutedEventArgs e)
      {
         string address = NewDistAddress.Text.Trim();
         if (DomainList.SelectedItem == null || address.Length == 0)
            return;

         dynamic domains = ServerSession.Current.Application.Domains;
         try
         {
            dynamic domain = OpenSelectedDomain(domains);
            dynamic lists = domain.DistributionLists;
            dynamic list = lists.Add();
            list.Address = address;
            list.Active = true;
            list.Save();
            ServerSession.Release(list);
            ServerSession.Release(lists);
            ServerSession.Release(domain);
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            MessageBox.Show(F("Could not create the list: {0}", ex.Message), L("Control Panel"));
            return;
         }
         finally
         {
            ServerSession.Release(domains);
         }

         NewDistAddress.Text = "";
         ReloadDistLists();
      }

      private void EditRecipients_Click(object sender, RoutedEventArgs e)
      {
         string address = (sender as FrameworkElement)?.Tag as string;
         string domainName = SelectedDomainName();
         if (address == null || domainName == null)
            return;

         new RecipientsDialog(Window.GetWindow(this), domainName, address).ShowDialog();
      }

      private void EditDistList_Click(object sender, RoutedEventArgs e)
      {
         string address = (sender as FrameworkElement)?.Tag as string;
         string domainName = SelectedDomainName();
         if (address == null || domainName == null)
            return;

         new DistributionListDialog(Window.GetWindow(this), domainName, address).ShowDialog();
         ReloadDistLists();
      }

      private void DeleteDistList_Click(object sender, RoutedEventArgs e)
      {
         string address = (sender as FrameworkElement)?.Tag as string;
         if (address == null || DomainList.SelectedItem == null)
            return;

         if (MessageBox.Show(F("Delete the distribution list {0}?", address), L("Control Panel"),
             MessageBoxButton.YesNo, MessageBoxImage.Warning) != MessageBoxResult.Yes)
            return;

         dynamic domains = ServerSession.Current.Application.Domains;
         try
         {
            dynamic domain = OpenSelectedDomain(domains);
            dynamic lists = domain.DistributionLists;
            int count = (int)lists.Count;
            for (int i = 0; i < count; i++)
            {
               dynamic list = lists.Item[i];
               if ((string)list.Address == address)
               {
                  list.Delete();
                  ServerSession.Release(list);
                  break;
               }
               ServerSession.Release(list);
            }
            ServerSession.Release(lists);
            ServerSession.Release(domain);
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            MessageBox.Show(F("Could not delete the list: {0}", ex.Message), L("Control Panel"));
         }
         finally
         {
            ServerSession.Release(domains);
         }

         ReloadDistLists();
      }

      private void ReloadAccounts()
      {
         string domainName = SelectedDomainName();
         if (domainName == null)
         {
            // With no domain there is nothing to create an account in: the form
            // waits for one, as it waits for a read that works.
            ReadFailure.Clear(AccountNotice);
            AccountsCard.Title = L("Select a domain");
            AccountList.ItemsSource = null;
            AccountStatus.Visibility = Visibility.Collapsed;
            AccountForm.IsEnabled = false;
            return;
         }

         AccountsCard.Title = F("{0} - accounts", domainName);
         NewAccountBox.Text = "user@" + domainName;

         var rows = new List<AccountRow>();
         try
         {
            dynamic domains = ServerSession.Current.Application.Domains;
            try
            {
               dynamic domain = domains.ItemByName[domainName];
               dynamic accounts = domain.Accounts;
               int count = (int)accounts.Count;
               for (int i = 0; i < count; i++)
               {
                  dynamic account = accounts.Item[i];
                  rows.Add(new AccountRow { Address = (string)account.Address, Active = (bool)account.Active });
                  ServerSession.Release(account);
               }
               ServerSession.Release(accounts);
               ServerSession.Release(domain);
            }
            finally
            {
               ServerSession.Release(domains);
            }
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            // Neither the accounts of the last read nor "No accounts in this
            // domain yet." is a fact this page has now: both go, the notice says
            // what happened, with Try again, and the account form waits for a
            // read that works (Services/ReadFailure).
            ReadFailure.Show(AccountNotice, AccountStatus, AccountList, L("Could not read the accounts: "), ex, ReloadAccounts, AccountForm);
            return;
         }

         ReadFailure.Clear(AccountNotice, AccountForm);
         AccountList.ItemsSource = rows;
         ListSearch.Apply(AccountList, AccountSearch.SearchText);
         StatusText.Show(AccountStatus, null, rows.Count, null, L("No accounts in this domain yet."));
      }

      private void EditDomain_Click(object sender, RoutedEventArgs e)
      {
         string name = (sender as FrameworkElement)?.Tag as string;
         if (name == null)
            return;

         new DomainDialog(Window.GetWindow(this), name).ShowDialog();
         ReloadDomains();
      }

      /// <summary>
      /// Deletes a domain and everything under it. The confirmation counts what is
      /// about to go, because "Delete example.com?" reads as removing a list row,
      /// and what actually happens is that every mailbox and every stored message
      /// in the domain is destroyed.
      /// </summary>
      private void DeleteDomain_Click(object sender, RoutedEventArgs e)
      {
         string name = (sender as FrameworkElement)?.Tag as string;
         if (name == null)
            return;

         dynamic domains = ServerSession.Current.Application.Domains;
         try
         {
            int accountCount;
            int domainId;
            dynamic domain = domains.ItemByName[name];
            try
            {
               domainId = (int)domain.ID;
               dynamic accounts = domain.Accounts;
               accountCount = (int)accounts.Count;
               ServerSession.Release(accounts);
            }
            finally
            {
               ServerSession.Release(domain);
            }

            string counted = accountCount == 1 ? L("its 1 account") : F("its {0} accounts", accountCount);

            if (MessageBox.Show(
                   F("Delete the domain {0}?\n\nThis permanently removes {1}, every message stored in them, and all of the domain's aliases and distribution lists. There is no undo.", name, counted),
                   L("Control Panel"), MessageBoxButton.YesNo, MessageBoxImage.Warning) != MessageBoxResult.Yes)
               return;

            domains.DeleteByDBID(domainId);
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            MessageBox.Show(F("Could not delete the domain: {0}", ex.Message), L("Control Panel"));
         }
         finally
         {
            ServerSession.Release(domains);
         }

         ReloadDomains();
      }

      private void AddDomain_Click(object sender, RoutedEventArgs e)
      {
         string name = NewDomainBox.Text.Trim();
         if (name.Length == 0 || !name.Contains('.'))
         {
            MessageBox.Show(L("Enter a valid domain name."), L("Control Panel"));
            return;
         }

         dynamic domains = ServerSession.Current.Application.Domains;
         try
         {
            dynamic domain = domains.Add();
            domain.Name = name;
            domain.Active = true;
            domain.Save();
            ServerSession.Release(domain);
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            // Without this, a duplicate name - the likeliest failure - took the
            // whole window down instead of saying so.
            MessageBox.Show(F("Could not create the domain: {0}", ex.Message), L("Control Panel"));
            return;
         }
         finally
         {
            ServerSession.Release(domains);
         }

         // The new domain is the one selected afterwards, so the accounts card
         // beside the list is its card: left on the domain that was selected
         // before, the next step of a first setup - adding an account - was
         // offered as user@ that other domain.
         NewDomainBox.Text = "";
         ReloadDomains(select: name);
         if (DomainList.SelectedItem != null)
            DomainList.ScrollIntoView(DomainList.SelectedItem);
      }

      private void GenerateAccountPassword_Click(object sender, RoutedEventArgs e)
      {
         string pw = Services.PasswordGenerator.Generate(16);
         NewAccountPassword.Password = pw;
         ClipboardWriter.TrySetText(pw);   // best effort: the eye icon shows the password as well
         Services.Toast.Info(L("Generated password copied to clipboard \u2014 reveal it with the eye icon."), L("Password"));
      }

      private void AddAccount_Click(object sender, RoutedEventArgs e)
      {
         string domainName = SelectedDomainName();
         string address = NewAccountBox.Text.Trim();
         string password = NewAccountPassword.Password;

         if (domainName == null || address.Length == 0 || password.Length == 0)
         {
            MessageBox.Show(L("Address and password are required."), L("Control Panel"));
            return;
         }

         dynamic domains = ServerSession.Current.Application.Domains;
         try
         {
            dynamic domain = domains.ItemByName[domainName];
            dynamic accounts = domain.Accounts;
            dynamic account = accounts.Add();
            account.Address = address;
            account.Password = password;
            account.Active = true;
            account.Save();
            ServerSession.Release(account);
            ServerSession.Release(accounts);
            ServerSession.Release(domain);
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            MessageBox.Show(F("Could not create the account: {0}", L(ServerSentences.MessageOf(ex))), L("Control Panel"));
            return;
         }
         finally
         {
            ServerSession.Release(domains);
         }

         NewAccountPassword.Password = "";
         ReloadAccounts();
      }

      private void EditAccount_Click(object sender, RoutedEventArgs e)
      {
         string address = (sender as FrameworkElement)?.Tag as string;
         string domainName = SelectedDomainName();
         if (address == null || domainName == null)
            return;

         new AccountDialog(Window.GetWindow(this), domainName, address).ShowDialog();
         ReloadAccounts();
      }

      private void DeleteAccount_Click(object sender, RoutedEventArgs e)
      {
         string address = (sender as FrameworkElement)?.Tag as string;
         string domainName = SelectedDomainName();
         if (address == null || domainName == null)
            return;

         if (MessageBox.Show(F("Delete {0}?", address), L("Control Panel"),
             MessageBoxButton.YesNo, MessageBoxImage.Warning) != MessageBoxResult.Yes)
            return;

         dynamic domains = ServerSession.Current.Application.Domains;
         try
         {
            dynamic domain = domains.ItemByName[domainName];
            dynamic accounts = domain.Accounts;
            dynamic account = accounts.ItemByAddress[address];
            account.Delete();
            ServerSession.Release(account);
            ServerSession.Release(accounts);
            ServerSession.Release(domain);
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            MessageBox.Show(F("Could not delete the account: {0}", ex.Message), L("Control Panel"));
         }
         finally
         {
            ServerSession.Release(domains);
         }

         ReloadAccounts();
      }
   }
}
