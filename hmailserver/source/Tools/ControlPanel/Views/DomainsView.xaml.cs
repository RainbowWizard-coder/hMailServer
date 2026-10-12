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
   public partial class DomainsView : UserControl, IPageLifecycle, IRefreshablePage
   {
      // The page's columns and cards keep a width below which what they hold runs
      // off their right edges: the domains column 280 pixels (its card 268, room
      // for a name beside Edit and Delete), the column of accounts, aliases
      // and lists 400, an alias or list card 270, and the new account's password
      // box 120 beside Generate and Create account. In the narrowest window
      // (760 by 520) the two columns were 268 and 232 pixels wide: Create account
      // ran off its card, and the alias and list cards, 104 and 116 pixels side by
      // side, had boxes 0 and 13 pixels wide. An alias or list card was 280 until
      // the sidebar became 310 pixels at 1280 (T3.3 work item 7): the column left
      // the two 273 each there, and at 280 they went one above the other.
      private const double DomainColumnMinWidth = 280;
      private const double DetailColumnMinWidth = 400;
      private const double ListCardMinWidth = 270;
      private const double PasswordMinWidth = 120;
      private static readonly double[] ColumnWeights = { 2, 3 };
      private static readonly double[] ColumnMinWidths = { DomainColumnMinWidth, DetailColumnMinWidth };
      private static readonly double[] ListCardWeights = { 1, 1 };
      private static readonly double[] ListCardMinWidths = { ListCardMinWidth, ListCardMinWidth };

      public DomainsView()
      {
         InitializeComponent();
         WireRows_();
         WireForms_();

         // Each example marked as one (docs/ControlPanelDesign.md): a bare
         // "example.org" in an empty box reads as a value already there.
         NewDomainBox.PlaceholderText = ExamplePlaceholder.Of("example.org"); // no-loc: an example domain
         NewAliasValue.PlaceholderText = ExamplePlaceholder.Of("person@example.net"); // no-loc: an example address

         // Every row of the four lists ends in buttons, and a list's bar is drawn
         // over its rows: while a list scrolls, its rows keep clear of the bar
         // (review finding F101).
         foreach (ListView list in new[] { DomainList, AccountList, AliasList, DistList })
            ScrollBarInset.ForList(list);
         DomainList.ItemContainerGenerator.StatusChanged += DomainRows_StatusChanged;
         NewAccountPassword.PasswordChanged += (s, e) => ShowNewAccountStrength_();
      }

      public void OnEnter()
      {
         // A message under a form's box answers the press that refused it: the
         // page met anew starts without one.
         DialogFields.ClearErrors(NewDomainRow);
         ClearDomainFormErrors_();
         ReloadDomains();
      }

      /// <summary>The messages of the forms that act on the selected domain, which are about that domain's boxes.</summary>
      private void ClearDomainFormErrors_()
         => DialogFields.ClearErrors(NewAccountRow, PasswordRow, NewAliasNameRow, NewAliasValueRow, NewDistAddressRow);

      /// <summary>F5: the domains again, the selected one kept, and with it its accounts, aliases and lists.</summary>
      void IRefreshablePage.Refresh() => ReloadDomains();

      /// <summary>Refresh in the header: what F5 does.</summary>
      private void Refresh_Click(object sender, RoutedEventArgs e) => ReloadDomains();

      // A domain's row: its 40 pixels, which the list's floor of three rows (123,
      // DomainsView.xaml) is built on.
      private const double DomainRowHeight = 40;

      private void PageColumns_SizeChanged(object sender, SizeChangedEventArgs e)
      {
         ArrangeColumns(SideBySide.Fits(e.NewSize.Width, ColumnWeights, ColumnMinWidths));
         FloorDomainList_();
      }

      /// <summary>
      /// The domains list's floor: three rows beside the accounts, and in the one
      /// column of a narrow window as many rows as it shows, up to
      /// WholeRowsFloor.MostRows, so that the page scrolls and the list does not
      /// scroll inside it. Held at three rows there, the fourth domain needed a
      /// scroll bar of the list's own inside the page's, over the rows' delete
      /// buttons (review finding F101).
      /// </summary>
      private void FloorDomainList_()
      {
         bool oneColumn = !DomainColumn.Children.Contains(DomainCard);
         double threeRows = WholeRowsFloor.For(WholeRowsFloor.FewestRows, DomainRowHeight);
         double floor = oneColumn ? Math.Max(threeRows, WholeRowsFloor.For(DomainList.Items.Count, DomainRowPitch_())) : threeRows;
         if (DomainList.MinHeight != floor)
            DomainList.MinHeight = floor;
      }

      /// <summary>
      /// A domain's row as it is drawn, the top of one to the top of the next - its
      /// container's height and the margin the container keeps - once there is one,
      /// and 40 pixels until then. By the container's height alone the floor was the
      /// margin short for every row, and in the narrowest window the fourth domain
      /// still scrolled inside the list.
      /// </summary>
      private double DomainRowPitch_()
         => DomainList.ItemContainerGenerator.ContainerFromIndex(0) is FrameworkElement first && first.ActualHeight > 0
            ? WholeRowsFloor.Pitch(first.ActualHeight, first.Margin.Top, first.Margin.Bottom)
            : DomainRowHeight;

      /// <summary>
      /// The list's rows drawn anew - read, filtered or realised as it scrolls: its
      /// floor again once they are laid out, so that it is built on the rows as drawn
      /// and not on the 40 pixels a row is taken for until there is one.
      /// </summary>
      private void DomainRows_StatusChanged(object sender, EventArgs e)
      {
         if (DomainList.ItemContainerGenerator.Status == System.Windows.Controls.Primitives.GeneratorStatus.ContainersGenerated)
            Dispatcher.BeginInvoke(System.Windows.Threading.DispatcherPriority.Loaded, new Action(FloorDomainList_));
      }

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
         Grid.SetRow(AccountButtonsLine, beside ? 1 : 3);
         Grid.SetColumn(AccountButtonsLine, beside ? 1 : 0);
         Grid.SetColumnSpan(AccountButtonsLine, beside ? 1 : 2);
         AccountButtonsLine.Margin = beside ? new Thickness(0) : new Thickness(0, 8, 0, 0);

         // Beside the password, the buttons sit under a row as tall as its caption,
         // so that they stay beside the box when a message appears under it; under
         // the password, the row takes no height.
         AccountButtonsCaption.SharedSizeGroup = beside ? "PasswordLine" : null;
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

         // The row's Edit button by the domain it opens, where every row's said
         // only "Edit" (review finding F083). It starts with the button's word in
         // every language, as a control's name starts with its caption.
         public string EditName => F("Edit domain {0}", Name);
         public override string ToString() => Active ? Name : F("{0} (inactive)", Name);

         // The row's Delete named for the row too (review finding F102): on every row a
         // screen reader heard "Delete domain", with nothing to say which domain.
         public string DeleteName => F("Delete domain {0}", Name);
      }

      /// <summary>Same shape for the account list.</summary>
      public class AccountRow
      {
         public string Address { get; set; }
         public bool Active { get; set; }

         // The row's Edit button by the account it opens (F083), as DomainRow's.
         public string EditName => F("Edit account {0}", Address);
         public override string ToString() => Active ? Address : F("{0} (inactive)", Address);

         // The row's Delete by the account it deletes (review finding F102).
         public string DeleteName => F("Delete account {0}", Address);
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
            ReadFailure.Show(DomainNotice, DomainStatus, DomainList, L("Could not read the domains: {0}"), ex, () => ReloadDomains(), AddDomainButton);
            ReloadAccounts();
            ReloadAliases();
            ReloadDistLists();
            AccountStatus.Visibility = Visibility.Collapsed;
            AliasStatus.Visibility = Visibility.Collapsed;
            DistStatus.Visibility = Visibility.Collapsed;
            return;
         }

         ReadFailure.Clear(DomainNotice, AddDomainButton);
         DomainList.ItemsSource = rows;
         ListSearch.Apply(DomainList, DomainSearch.SearchText);
         FloorDomainList_();
         StatusText.Show(DomainStatus, null, rows.Count, null, L("No domains yet. Add the first one to start receiving mail."));

         // Replacing ItemsSource clears the selection; put it back on the same
         // domain when it still exists, so a reload does not yank the accounts
         // pane over to whatever happens to be first.
         if (previousSelection != null)
            DomainList.SelectedItem = rows.Find(r => string.Equals(r.Name, previousSelection, StringComparison.OrdinalIgnoreCase));

         if (rows.Count > 0 && DomainList.SelectedIndex < 0)
            DomainList.SelectedIndex = 0;

         // With no domain to select - an empty server - the selection may not have
         // changed, and then nothing put the three cards that hang off a domain in
         // their no-domain state: the account form stayed live under "Select a
         // domain" with the last domain's user@ address in it (review finding F063).
         if (DomainList.SelectedIndex < 0)
         {
            ReloadAccounts();
            ReloadAliases();
            ReloadDistLists();
         }
      }

      private void DomainSearch_TextChanged(object sender, EventArgs e)
      {
         ListSearch.Apply(DomainList, DomainSearch.SearchText);
         FloorDomainList_();
      }

      /// <summary>
      /// The account search filters the selected domain's accounts, and a whole
      /// address of another hosted domain selects that domain first (review finding
      /// F096), whose accounts the selection reads and the search then filters:
      /// typing "user01@example.net" while example.com was selected found nothing.
      /// </summary>
      private void AccountSearch_TextChanged(object sender, EventArgs e)
      {
         var domains = DomainList.ItemsSource as List<DomainRow>;
         string other = HostedAddress.DomainToShow(AccountSearch.SearchText, domains?.ConvertAll(d => d.Name), SelectedDomainName());
         if (other != null && SelectDomain_(other))
            return;

         ListSearch.Apply(AccountList, AccountSearch.SearchText);
      }

      /// <summary>
      /// Selects the hosted domain of that name, which reads its accounts, aliases and
      /// lists, and brings it into view, clearing the domain search where that hides
      /// its row. False where the page holds no such domain.
      /// </summary>
      private bool SelectDomain_(string name)
      {
         DomainRow row = (DomainList.ItemsSource as List<DomainRow>)?.Find(r => string.Equals(r.Name, name, StringComparison.OrdinalIgnoreCase));
         if (row == null)
            return false;

         if (!DomainList.Items.Contains(row))
            DomainSearch.SearchText = "";

         DomainList.SelectedItem = row;
         DomainList.ScrollIntoView(row);
         return ReferenceEquals(DomainList.SelectedItem, row);
      }

      /// <summary>The account of that address in the accounts card, selected and scrolled to, where the card shows it.</summary>
      private void SelectAccount_(string address)
      {
         if ((AccountList.ItemsSource as List<AccountRow>)?.Find(r => string.Equals(r.Address, address, StringComparison.OrdinalIgnoreCase)) is not AccountRow row
             || !AccountList.Items.Contains(row))
            return;

         AccountList.SelectedItem = row;
         AccountList.ScrollIntoView(row);
      }

      private void DomainList_SelectionChanged(object sender, SelectionChangedEventArgs e)
      {
         ClearDomainFormErrors_();

         // An address typed for one domain is not one of the next: the box empties
         // when the domain changes, as it did when it was filled with user@ and the
         // domain.
         NewAccountBox.Text = "";
         ReloadAccounts();
         ReloadAliases();
         ReloadDistLists();
      }

      public class AliasRow
      {
         public string Name { get; set; }

         // Where the alias delivers: the row's second line, under the alias, each
         // line trimmed with its own tool tip. On one trimmed line, "name  →  target",
         // the target was the part the ellipsis took, and it is the half an
         // administrator looks up (review finding F067).
         public string Value { get; set; }
         public string Target => "→ " + Value; // no-loc: an arrow, not a word

         public string Display { get; set; }

         public string DeleteName => F("Delete alias {0}", Name);

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
                  string name = (string)alias.Name;
                  string value = (string)alias.Value;
                  rows.Add(new AliasRow
                  {
                     Name = name,
                     Value = value,
                     Display = name + "  →  " + value
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
         // An alias belongs to a domain: with none selected the form waits for one,
         // where Add alias pressed then did nothing at all (review finding F063).
         AddAliasForm.IsEnabled = DomainList.SelectedItem != null;
         SetListStatus(AliasStatus, rows.Count, error,
            DomainList.SelectedItem == null ? L("Select a domain to see its aliases.") : L("No aliases for this domain."),
            L("Could not read the aliases: {0}"));
      }

      /// <summary>
      /// A distribution list in the lists card: its address, and the names of the
      /// row's Edit and Members buttons, which are glyphs and say which list
      /// each acts on in their tool tips and to a screen reader. ToString carries
      /// the address to a ListViewItem's accessible name, as AliasRow does.
      /// </summary>
      public class DistRow
      {
         public string Address { get; set; }
         public string EditName => F("Edit distribution list {0}", Address);
         public string MembersName => F("Members of {0}", Address);
         public string DeleteName => F("Delete distribution list {0}", Address);
         public override string ToString() => Address;
      }

      private void ReloadDistLists()
      {
         var rows = new List<DistRow>();
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
                  rows.Add(new DistRow { Address = (string)list.Address });
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
         AddListForm.IsEnabled = DomainList.SelectedItem != null;
         SetListStatus(DistStatus, rows.Count, error,
            DomainList.SelectedItem == null ? L("Select a domain to see its distribution lists.") : L("No distribution lists for this domain."),
            L("Could not read the distribution lists: {0}"));
      }

      // Shows a centered empty/error placeholder over a list when it has no rows.
      private static void SetListStatus(Scaffold.EmptyState status, int rowCount, string error, string emptyText, string errorTemplate)
         => StatusText.Show(status, null, rowCount, error, emptyText, errorTemplate);

      private void AddAlias_Click(object sender, RoutedEventArgs e)
      {
         // A name typed without an @ is an alias of the selected domain, as the
         // box's placeholder - "sales@" and the domain - shows (review finding F092).
         string name = HostedAddress.Complete(NewAliasName.Text, SelectedDomainName());
         string value = NewAliasValue.Text.Trim();
         if (DomainList.SelectedItem == null)
            return;

         // An empty box is said on its box, where the press did nothing at all
         // (review finding F064); both on one press, the keyboard in the first.
         DialogFields.ClearErrors(NewAliasNameRow, NewAliasValueRow);
         if (name.Length == 0)
            NewAliasNameRow.Error = L("Enter the alias address.");
         if (value.Length == 0)
            NewAliasValueRow.Error = L("Enter where the alias delivers.");
         if (DialogFields.ShowFirstError(NewAliasNameRow, NewAliasValueRow))
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
            MessageBox.Show(F("Could not create the alias: {0}", ServerSession.DescribeComError(ex)), L("Control Panel"));
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
         if ((sender as FrameworkElement)?.Tag is string name)
            DeleteAlias_(name);
      }

      private void DeleteAlias_(string name)
      {
         if (DomainList.SelectedItem == null)
            return;

         if (!MessageBox.Confirm(Confirmations.DeleteAlias(name), L("Control Panel"), L("_Delete alias"), destructive: true))
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
            MessageBox.Show(F("Could not delete the alias: {0}", ServerSession.DescribeComError(ex)), L("Control Panel"));
         }
         finally
         {
            ServerSession.Release(domains);
         }

         ReloadAliases();
      }

      private void AddDistList_Click(object sender, RoutedEventArgs e)
      {
         string address = HostedAddress.Complete(NewDistAddress.Text, SelectedDomainName());
         if (DomainList.SelectedItem == null)
            return;

         DialogFields.ClearErrors(NewDistAddressRow);
         if (address.Length == 0)
         {
            DialogFields.ShowError(NewDistAddressRow, L("Enter the list's address."));
            return;
         }

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
            MessageBox.Show(F("Could not create the list: {0}", ServerSession.DescribeComError(ex)), L("Control Panel"));
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
         if ((sender as FrameworkElement)?.Tag is string address)
            EditRecipients_(address);
      }

      private void EditRecipients_(string address)
      {
         string domainName = SelectedDomainName();
         if (domainName == null)
            return;

         new RecipientsDialog(Window.GetWindow(this), domainName, address).ShowDialog();
      }

      private void EditDistList_Click(object sender, RoutedEventArgs e)
      {
         if ((sender as FrameworkElement)?.Tag is string address)
            EditDistList_(address);
      }

      private void EditDistList_(string address)
      {
         string domainName = SelectedDomainName();
         if (domainName == null)
            return;

         new DistributionListDialog(Window.GetWindow(this), domainName, address).ShowDialog();
         ReloadDistLists();
      }

      private void DeleteDistList_Click(object sender, RoutedEventArgs e)
      {
         if ((sender as FrameworkElement)?.Tag is string address)
            DeleteDistList_(address);
      }

      private void DeleteDistList_(string address)
      {
         if (DomainList.SelectedItem == null)
            return;

         if (!MessageBox.Confirm(Confirmations.DeleteDistributionList(address), L("Control Panel"), L("_Delete list"), destructive: true))
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
            MessageBox.Show(F("Could not delete the list: {0}", ServerSession.DescribeComError(ex)), L("Control Panel"));
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
            // waits for one, as it waits for a read that works, empty - it held the
            // last domain's user@ address - and the card says what to do, as the
            // alias and list cards do (review finding F063).
            ReadFailure.Clear(AccountNotice);
            AccountsCard.Title = L("Select a domain");
            AccountList.ItemsSource = null;
            NewAccountBox.Text = "";
            NewAccountPassword.Password = "";
            StatusText.Show(AccountStatus, null, 0, null, L("Select a domain to see its accounts."));
            AccountForm.IsEnabled = false;
            ShowAddressPlaceholders_(null);
            return;
         }

         AccountsCard.Title = F("{0} - accounts", domainName);
         ShowAddressPlaceholders_(domainName);

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
            ReadFailure.Show(AccountNotice, AccountStatus, AccountList, L("Could not read the accounts: {0}"), ex, ReloadAccounts, AccountForm);
            return;
         }

         ReadFailure.Clear(AccountNotice, AccountForm);
         AccountList.ItemsSource = rows;
         ListSearch.Apply(AccountList, AccountSearch.SearchText);
         StatusText.Show(AccountStatus, null, rows.Count, null, L("No accounts in this domain yet."));
      }

      /// <summary>
      /// The address boxes of the selected domain show an example address in it as
      /// their placeholders - reserved names, never text in the box (review finding
      /// F092): the Address box held "user@" and the domain as text, so an untouched
      /// Create account made a mailbox called user, and the alias and list boxes read
      /// "alias@domain" and "list@domain". A name typed without an @ is completed with
      /// the domain (HostedAddress.Complete). And Create account is the card's primary
      /// button while a domain is selected and a plain one while none is, when the
      /// domains card's "Add a domain" is the screen's primary (review finding F097).
      /// </summary>
      private void ShowAddressPlaceholders_(string domainName)
      {
         NewAccountBox.PlaceholderText = ExamplePlaceholder.Shown(HostedAddress.Placeholder("name", domainName)); // no-loc: an example address
         NewAliasName.PlaceholderText = ExamplePlaceholder.Shown(HostedAddress.Placeholder("sales", domainName)); // no-loc: an example address
         NewDistAddress.PlaceholderText = ExamplePlaceholder.Shown(HostedAddress.Placeholder("team", domainName)); // no-loc: an example address
         AddAccountButton.Appearance = domainName == null ? Wpf.Ui.Controls.ControlAppearance.Secondary : Wpf.Ui.Controls.ControlAppearance.Primary;
      }

      private void EditDomain_Click(object sender, RoutedEventArgs e)
      {
         if ((sender as FrameworkElement)?.Tag is string name)
            EditDomain_(name);
      }

      private void EditDomain_(string name)
      {
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
         if ((sender as FrameworkElement)?.Tag is string name)
            DeleteDomain_(name);
      }

      private void DeleteDomain_(string name)
      {
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

            if (!MessageBox.Confirm(Confirmations.DeleteDomain(name, accountCount), L("Control Panel"), L("_Delete domain"), destructive: true))
               return;

            domains.DeleteByDBID(domainId);
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            MessageBox.Show(F("Could not delete the domain: {0}", ServerSession.DescribeComError(ex)), L("Control Panel"));
         }
         finally
         {
            ServerSession.Release(domains);
         }

         ReloadDomains();
      }

      /// <summary>The empty state's action: the keyboard goes to the Domain box, the next thing to do on an empty server.</summary>
      private void AddDomainAction_Click(object sender, RoutedEventArgs e)
      {
         NewDomainBox.Focus();
         NewDomainBox.SelectAll();
      }

      private void AddDomain_Click(object sender, RoutedEventArgs e)
      {
         string name = NewDomainBox.Text.Trim();

         // Said on the box, with the keyboard put in it, where a message box said
         // it away from the box and took the keyboard with it (review finding
         // F064). A message box is left for the server's own refusal.
         DialogFields.ClearErrors(NewDomainRow);
         if (name.Length == 0 || !name.Contains('.'))
         {
            DialogFields.ShowError(NewDomainRow, L("Enter a domain name such as example.com."));
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
            MessageBox.Show(F("Could not create the domain: {0}", ServerSession.DescribeComError(ex)), L("Control Panel"));
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
         Services.Toast.Info(L("Generated password copied to the clipboard; reveal it with the eye icon."), L("Password"));
      }

      /// <summary>The level the strength line last said, so that a keystroke that keeps it is not announced again.</summary>
      private PasswordStrength.Level? strengthShown_;

      /// <summary>
      /// The new account's password judged as it is typed, by the verdict and in the
      /// colours the account dialog gives it (PasswordStrength), where the page said
      /// nothing at all and the first mailbox a new administrator made skipped the
      /// check the dialog makes (review finding F072). The words carry the verdict; the
      /// colour repeats it. Announced when the verdict changes, not at every key.
      /// </summary>
      private void ShowNewAccountStrength_()
      {
         (PasswordStrength.Level level, string summary) = PasswordStrength.Evaluate(NewAccountPassword.Password);
         NewAccountStrength.Text = summary;
         NewAccountStrength.Visibility = level == PasswordStrength.Level.Empty ? Visibility.Collapsed : Visibility.Visible;

         // By key, never by a held brush, as the dialog's line: the level's fill-text shade.
         string key = level switch
         {
            PasswordStrength.Level.Strong => StatusSemantics.For(StatusLevel.Good).FillTextBrushKey,
            PasswordStrength.Level.Fair => StatusSemantics.For(StatusLevel.Warning).FillTextBrushKey,
            PasswordStrength.Level.Weak => StatusSemantics.For(StatusLevel.Critical).FillTextBrushKey,
            _ => "TextFillColorSecondaryBrush"
         };
         NewAccountStrength.SetResourceReference(TextBlock.ForegroundProperty, key);

         if (strengthShown_ != level && level != PasswordStrength.Level.Empty)
            Announce.Request(NewAccountStrength);
         strengthShown_ = level;
      }

      /// <summary>
      /// Whether a password may be given to a new account: one that is not weak, or a
      /// weak one the administrator confirms, as the account dialog asks before it saves
      /// one - Cancel the answer Enter gives.
      /// </summary>
      private static bool NewAccountPasswordAccepted_(string password)
      {
         (PasswordStrength.Level level, string summary) = PasswordStrength.Evaluate(password);
         return level != PasswordStrength.Level.Weak
            || MessageBox.Confirm(summary + "\n\n" + L("Create the account with this weak password anyway?"), L("Control Panel"),
                  L("_Create account"), cancelByDefault: true, level: StatusLevel.Warning);
      }

      private void AddAccount_Click(object sender, RoutedEventArgs e)
      {
         string domainName = SelectedDomainName();
         string address = HostedAddress.Complete(NewAccountBox.Text, domainName);
         string password = NewAccountPassword.Password;

         if (domainName == null)
            return;

         // Each box left empty is said on that box, both on one press, where one
         // message box named neither (review finding F064).
         DialogFields.ClearErrors(NewAccountRow, PasswordRow);
         if (address.Length == 0)
            NewAccountRow.Error = L("Enter the account's address.");
         if (password.Length == 0)
            PasswordRow.Error = L("Enter a password, or press Generate.");
         if (DialogFields.ShowFirstError(NewAccountRow, PasswordRow))
            return;

         if (!NewAccountPasswordAccepted_(password))
            return;

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
            // The server's reason in the reader's language where the catalogue has
            // it - a breached password - and a lost link said as one, as every other
            // handler on the page says them (review finding F093).
            MessageBox.Show(F("Could not create the account: {0}", ServerSession.DescribeComError(ex)), L("Control Panel"));
            return;
         }
         finally
         {
            ServerSession.Release(domains);
         }

         // The new account is selected and scrolled to, as a new domain is (review
         // finding F095), where the list was read again with nothing to say which row
         // was the one just made; the box empties for the next.
         NewAccountBox.Text = "";
         NewAccountPassword.Password = "";
         ReloadAccounts();
         SelectAccount_(address);
      }

      private void EditAccount_Click(object sender, RoutedEventArgs e)
      {
         if ((sender as FrameworkElement)?.Tag is string address)
            EditAccount_(address);
      }

      private void EditAccount_(string address)
      {
         string domainName = SelectedDomainName();
         if (domainName == null)
            return;

         var dialog = new AccountDialog(Window.GetWindow(this), domainName, address);
         dialog.ShowDialog();

         // A mailbox moved or renamed to another domain is followed there and
         // selected in it (review finding F095): the page read the old domain's
         // accounts again, and the mailbox simply vanished from them.
         if (HostedAddress.DomainOf(dialog.MovedTo) is string movedDomain)
         {
            ReloadDomains(select: movedDomain);
            SelectAccount_(dialog.MovedTo);
            return;
         }

         ReloadAccounts();
      }

      private void DeleteAccount_Click(object sender, RoutedEventArgs e)
      {
         if ((sender as FrameworkElement)?.Tag is string address)
            DeleteAccount_(address);
      }

      private void DeleteAccount_(string address)
      {
         string domainName = SelectedDomainName();
         if (domainName == null)
            return;

         if (!MessageBox.Confirm(Confirmations.DeleteAccount(address), L("Control Panel"), L("_Delete account"), destructive: true))
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
            MessageBox.Show(F("Could not delete the account: {0}", ServerSession.DescribeComError(ex)), L("Control Panel"));
         }
         finally
         {
            ServerSession.Release(domains);
         }

         ReloadAccounts();
      }
   }
}
