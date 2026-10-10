// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Linq;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using hMailServer.ControlPanel.Services;
using hMailServer.ControlPanel.Views.Scaffold;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Views
{
   /// <summary>
   /// The accounts that belong to one security group.
   ///
   /// This existed nowhere in the Control Panel, and its absence made the whole
   /// feature unusable rather than merely awkward: a group could be created on the
   /// Groups page and granted rights on a public folder in
   /// <see cref="FolderPermissionsDialog"/>, but no account could ever be put into
   /// it, so the permission it was granted applied to nobody. Filling it in
   /// required writing a COM script - which is precisely the shape AGENTS.md's GUI
   /// rule exists to stop, since most administrators never write one.
   ///
   /// Members are stored as (group, account) rows rather than as names, so the
   /// list here is built by resolving each member's AccountID back to an address.
   /// A member whose account has since been deleted therefore has nothing to
   /// resolve; it is shown as an orphan rather than hidden, because a row that
   /// silently vanishes from a permissions list is the sort of thing an
   /// administrator only discovers when someone cannot read a folder.
   ///
   /// On the standard frame: the group's name is the heading, the two buttons
   /// that act on the list sit under it, what happened is a notice above it,
   /// and Close is alone in the footer.
   /// </summary>
   public class GroupMembersDialog : FluentDialogWindow
   {
      private readonly int groupId_;
      private readonly string groupName_;

      // On a card with the compact inset, as the public folders list is: the
      // rows take the application's list row style (the selection fill and the
      // brand pill, in place of WPF-UI's solid accent band), and the card is the
      // list's frame.
      private readonly ListBox list_ = new() { Height = 260, BorderThickness = new Thickness(0), Background = System.Windows.Media.Brushes.Transparent };

      /// <summary>Member row database ids, parallel to <see cref="list_"/>.</summary>
      private readonly List<int> memberIds_ = new();

      /// <summary>Each member's account id, parallel to <see cref="list_"/>: the accounts the picker leaves out.</summary>
      private readonly List<int> memberAccountIds_ = new();

      private readonly TextBlock status_ = DialogFields.Note("");
      private readonly InlineNotice notice_ = DialogFields.Notice();

      // What an empty group means, in the list's place rather than a blank box
      // under a line of text (review finding F207). Add account… under the list
      // is its action.
      private readonly EmptyState empty_ = new() { Icon = Wpf.Ui.Controls.SymbolRegular.Person24, Padding = new Thickness(12), Visibility = Visibility.Collapsed };

      public GroupMembersDialog(Window owner, int groupId, string groupName)
      {
         groupId_ = groupId;
         groupName_ = groupName ?? "";

         Owner = owner;
         Title = F("Members - {0}", groupName_);

         var body = new StackPanel();
         body.Children.Add(notice_);

         AutomationProperties.SetName(list_, F("Accounts in the group {0}", groupName_));
         AutomationProperties.SetAutomationId(list_, "group-members-list");
         var listHost = new Grid();
         listHost.Children.Add(list_);
         listHost.Children.Add(empty_);
         body.Children.Add(new Card
         {
            Padding = new Thickness(DesignTokens.Space.Sm),
            Margin = new Thickness(0, 0, 0, DesignTokens.Space.Sm),
            Content = listHost
         });
         body.Children.Add(status_);

         var actions = new StackPanel { Orientation = Orientation.Horizontal };

         // A secondary button: the dialog's one Primary is Close, the footer's only
         // button (review finding F296).
         var add = new Wpf.Ui.Controls.Button
         {
            Content = L("_Add account…"),
            Margin = new Thickness(0, 0, DesignTokens.Space.Sm, 0),
            MinWidth = 110
         };
         ControlHelp.Explain(add, L("Add an account to this group"));
         AutomationProperties.SetAutomationId(add, "group-members-add");
         add.Click += (s, e) => AddMember();
         actions.Children.Add(add);

         // A list's Remove: red words on a secondary button, pressed only while an
         // account is chosen (review findings F183 and F076). It was the filled red
         // of a confirmation's own answer, and live with nothing chosen.
         var remove = new Wpf.Ui.Controls.Button
         {
            Content = L("Re_move"),
            MinWidth = 90
         };
         ButtonStyles.DangerText(remove);
         SelectionGate.Bind(list_, remove);
         ControlHelp.Explain(remove, L("Remove the selected account from this group"));
         AutomationProperties.SetAutomationId(remove, "group-members-remove");
         remove.Click += (s, e) => RemoveSelected();
         actions.Children.Add(remove);

         body.Children.Add(actions);

         // Escape only, and deliberately no default button: Enter with a row
         // selected must not re-open the add prompt. Close, the footer's only
         // button, is drawn Primary, as every dialog's lone Close is (F296).
         var close = new Wpf.Ui.Controls.Button { Content = L("Close"), Appearance = Wpf.Ui.Controls.ControlAppearance.Primary, MinWidth = 88 };
         close.Click += (s, e) => Close();

         // Headed by the window's title, the kind and then the name, where the
         // heading was the bare name and the kind only in a title the frame does
         // not show (review finding F293).
         UseFrame(Title, body, null, close,
            L("A group exists so that several accounts can be given the same rights at once - grant the group access to a public folder under Public folders, and every account listed here gets that access. An empty group grants nothing to anyone."),
            width: DesignTokens.Dialog.DefaultWidth);
         Loaded += (s, e) => Reload();
      }

      /// <summary>The group this dialog edits, or null when it has been deleted underneath us.</summary>
      private dynamic OpenGroup()
      {
         dynamic groups = ServerSession.Current.Application.Settings.Groups;
         return groups.ItemByDBID[groupId_];
      }

      private void Reload()
      {
         list_.Items.Clear();
         memberIds_.Clear();
         memberAccountIds_.Clear();
         status_.Text = "";
         empty_.Visibility = Visibility.Collapsed;

         try
         {
            dynamic group = OpenGroup();
            dynamic members = group.Members;

            int count = (int)members.Count;

            for (int i = 0; i < count; i++)
            {
               dynamic member = members[i];

               int memberId = (int)member.ID;
               string label;

               // Account is a COM property that resolves the stored AccountID. It can
               // legitimately fail when the account has been deleted since the member
               // row was written, which is exactly the case worth showing rather than
               // swallowing.
               try
               {
                  dynamic account = member.Account;
                  label = account == null ? null : (string)account.Address;
               }
               catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
               {
                  label = null;
               }

               int accountId = (int)member.AccountID;
               if (string.IsNullOrWhiteSpace(label))
                  label = F("(account {0} no longer exists)", accountId);

               memberIds_.Add(memberId);
               memberAccountIds_.Add(accountId);
               list_.Items.Add(label);
            }

            // The empty group's sentence in the list's place; the count under a
            // list that has rows.
            StatusText.Show(empty_, null, count, null,
               L("This group has no accounts in it, so any permission granted to it currently applies to nobody."));
            status_.Text = count == 0
               ? ""
               : Plural(count, "{0} account is a member.", "{0} accounts are members.");
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            notice_.Show(StatusLevel.Critical, L("The group members could not be read: ") + ServerSession.DescribeComError(ex));
         }
      }

      /// <summary>Every account on the server, by its id and address, for the picker.</summary>
      private static List<GroupMemberCandidates.Account> ReadAllAccounts()
      {
         var accounts = new List<GroupMemberCandidates.Account>();

         // Every object read here is a fresh COM identity handed out by the server,
         // so each is released as soon as it has been read, innermost first - the
         // same discipline as the other collection walks in this application.
         dynamic domains = null;
         try
         {
            domains = ServerSession.Current.Application.Domains;
            int domainCount = (int)domains.Count;

            for (int d = 0; d < domainCount; d++)
            {
               dynamic domain = null;
               dynamic domainAccounts = null;
               try
               {
                  domain = domains[d];
                  domainAccounts = domain.Accounts;
                  int accountCount = (int)domainAccounts.Count;

                  for (int a = 0; a < accountCount; a++)
                  {
                     dynamic account = null;
                     try
                     {
                        account = domainAccounts[a];
                        accounts.Add(new GroupMemberCandidates.Account((int)account.ID, (string)account.Address));
                     }
                     finally
                     {
                        ServerSession.Release((object)account);
                     }
                  }
               }
               finally
               {
                  ServerSession.Release((object)domainAccounts);
                  ServerSession.Release((object)domain);
               }
            }
         }
         finally
         {
            ServerSession.Release((object)domains);
         }

         return accounts;
      }

      /// <summary>
      /// Add account…: a list under a search box of the accounts that are not
      /// members yet, several of which can be chosen and added at once (review
      /// finding F203), where a drop-down of every address on the server - the
      /// members among them - could not be typed in and added one account at a time.
      /// The accounts added are said by address, and the first of them is selected.
      /// </summary>
      private void AddMember()
      {
         notice_.Hide();
         var added = new List<string>();
         try
         {
            List<GroupMemberCandidates.Account> everyAccount = ReadAllAccounts();
            IReadOnlyList<GroupMemberCandidates.Account> candidates = GroupMemberCandidates.NotMembers(everyAccount, memberAccountIds_);

            if (candidates.Count == 0)
            {
               notice_.Show(StatusLevel.Information, everyAccount.Count == 0
                  ? L("There are no accounts on this server to add.")
                  : GroupMemberCandidates.EveryAccountIsAMember);
               return;
            }

            List<GroupMemberCandidates.Account> chosen = PickAccountDialog.Pick(this, groupName_, candidates);
            if (chosen.Count == 0)
               return;

            dynamic group = OpenGroup();
            dynamic members = group.Members;

            // The members as the server has them now: an account another session made
            // a member since the list was read is not given a second row, which would
            // grant nothing more and leave two rows to remove.
            var present = new HashSet<int>();
            int existing = (int)members.Count;
            for (int i = 0; i < existing; i++)
               present.Add((int)members[i].AccountID);

            foreach (GroupMemberCandidates.Account account in chosen)
            {
               if (!present.Add(account.Id))
                  continue;

               // Add() already stamps the group id from the collection it belongs to,
               // so only the account and the Save are needed here.
               dynamic member = members.Add();
               member.AccountID = account.Id;
               member.Save();
               ServerSession.Release((object)member);
               added.Add(account.Address);
            }

            Reload();
            if (added.Count == 0)
            {
               notice_.Show(StatusLevel.Information, chosen.Count == 1
                  ? F("{0} is already a member of this group.", chosen[0].Address)
                  : L("The accounts chosen are members of this group already."));
               return;
            }

            notice_.Show(StatusLevel.Good, GroupMemberCandidates.Added(added));
            int first = list_.Items.IndexOf(added[0]);
            if (first >= 0)
            {
               list_.SelectedIndex = first;
               list_.ScrollIntoView(list_.Items[first]);
            }
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            // The accounts added before the failure are on the list, read again.
            if (added.Count > 0)
               Reload();
            notice_.Show(StatusLevel.Critical, F("The account could not be added to the group: {0}", ServerSession.DescribeComError(ex)));
         }
      }

      private void RemoveSelected()
      {
         int index = list_.SelectedIndex;
         if (index < 0 || index >= memberIds_.Count)
            return;

         string label = (string)list_.Items[index];

         if (!Dialogs.Confirm(F("Remove {0} from {1}?\r\n\r\nAny access this group has been granted stops applying to that account.", label, groupName_),
                             L("Control Panel"), L("Re_move"), destructive: true))
         {
            return;
         }

         notice_.Hide();
         try
         {
            dynamic group = OpenGroup();
            group.Members.DeleteByDBID(memberIds_[index]);
            Reload();
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            notice_.Show(StatusLevel.Critical, F("The account could not be removed from the group: {0}", ServerSession.DescribeComError(ex)));
         }
      }
   }

   /// <summary>
   /// Picks accounts to add from a list under a search box (review finding F203). A
   /// list rather than a free-text box because a group member is stored as an
   /// account id: a typed address that does not exist cannot be stored at all, so
   /// a text box would only produce an error after the fact. The list holds the
   /// accounts that are not members yet (GroupMemberCandidates), filtered as the
   /// search is typed in; several can be chosen with Ctrl and Shift, and Enter adds
   /// them - in the search box, the one account it leaves when it leaves one. It was
   /// a drop-down of every address on the server that could not be typed in.
   /// </summary>
   internal static class PickAccountDialog
   {
      public static List<GroupMemberCandidates.Account> Pick(Window owner, string groupName, IReadOnlyList<GroupMemberCandidates.Account> accounts)
      {
         var result = new List<GroupMemberCandidates.Account>();
         FramedDialog dialog = Build(owner, groupName, accounts, result);
         return dialog.ShowDialog() == true ? result : new List<GroupMemberCandidates.Account>();
      }

      /// <summary>The picker, made but not shown; <paramref name="result"/> takes the accounts Add adds.</summary>
      internal static FramedDialog Build(Window owner, string groupName, IReadOnlyList<GroupMemberCandidates.Account> accounts, List<GroupMemberCandidates.Account> result)
      {
         string heading = F("Add accounts to {0}", groupName ?? "");
         var dialog = new FramedDialog { Owner = owner, Title = heading };
         var shown = new List<GroupMemberCandidates.Account>(accounts ?? Array.Empty<GroupMemberCandidates.Account>());

         var search = new Wpf.Ui.Controls.TextBox();
         AutomationProperties.SetAutomationId(search, "group-members-pick-search");

         // On a card with the compact inset, as the member list is.
         var list = new ListBox
         {
            SelectionMode = SelectionMode.Extended,
            Height = 240,
            BorderThickness = new Thickness(0),
            Background = System.Windows.Media.Brushes.Transparent,
            ItemsSource = shown
         };
         AutomationProperties.SetName(list, L("Accounts that are not members"));
         AutomationProperties.SetAutomationId(list, "group-members-pick");
         var empty = new EmptyState { Icon = Wpf.Ui.Controls.SymbolRegular.Search24, Padding = new Thickness(12), Visibility = Visibility.Collapsed };
         var listHost = new Grid();
         listHost.Children.Add(list);
         listHost.Children.Add(empty);

         var body = new StackPanel();
         body.Children.Add(DialogFields.Field(L("_Find an account"), search));
         body.Children.Add(new Card
         {
            Padding = new Thickness(DesignTokens.Space.Sm),
            Margin = new Thickness(0, 0, 0, DesignTokens.Space.Sm),
            Content = listHost
         });
         body.Children.Add(DialogFields.Note(L("Ctrl+click or Shift+click chooses several accounts; Enter adds the accounts chosen.")));

         var ok = new Wpf.Ui.Controls.Button
         {
            Content = L("_Add"),
            Appearance = Wpf.Ui.Controls.ControlAppearance.Primary,
            MinWidth = 88
         };
         AutomationProperties.SetAutomationId(ok, "group-members-pick-add");
         SelectionGate.Bind(list, ok);
         ok.Click += (s, e) =>
         {
            result.AddRange(list.SelectedItems.OfType<GroupMemberCandidates.Account>());
            if (result.Count == 0)
               return;
            dialog.DialogResult = true;
            dialog.Close();
         };

         var cancel = new Wpf.Ui.Controls.Button { Content = L("Cancel"), MinWidth = 88 };
         cancel.Click += (s, e) => dialog.Close();

         // The search filters as it is typed in; where it hides every account the
         // list's place says so. A choice the search hides is no longer chosen, so
         // that Add adds only accounts that can be seen.
         search.TextChanged += (s, e) =>
         {
            string query = search.Text?.Trim() ?? "";
            ListSearch.Apply(list, query, item => (item as GroupMemberCandidates.Account)?.Address ?? "");
            foreach (GroupMemberCandidates.Account hidden in list.SelectedItems.OfType<GroupMemberCandidates.Account>().Where(a => !ListSearch.Contains(a.Address, query)).ToList())
               list.SelectedItems.Remove(hidden);
            bool none = query.Length > 0 && !shown.Any(a => ListSearch.Contains(a.Address, query));
            empty.Text = none ? GroupMemberCandidates.NoneMatches(query) : "";
            empty.Visibility = none ? Visibility.Visible : Visibility.Collapsed;
         };

         // Enter in the search box adds what is chosen, or the one account the
         // search leaves; Down goes to the list. Enter on the list adds what is
         // chosen, and a double-click on an account adds it.
         search.PreviewKeyDown += (s, e) =>
         {
            if (e.Key == System.Windows.Input.Key.Down && list.Items.Count > 0)
            {
               if (list.SelectedItem == null)
                  list.SelectedIndex = 0;
               GridKeys.FocusSelectedRow(list);
               e.Handled = true;
            }
            else if (e.Key == System.Windows.Input.Key.Enter)
            {
               if (list.SelectedItems.Count == 0 && list.Items.Count == 1)
                  list.SelectedIndex = 0;
               GridKeys.Press(ok);
               e.Handled = true;
            }
         };
         list.PreviewKeyDown += (s, e) =>
         {
            if (e.Key != System.Windows.Input.Key.Enter || list.SelectedItems.Count == 0)
               return;
            GridKeys.Press(ok);
            e.Handled = true;
         };
         list.MouseDoubleClick += (s, e) =>
         {
            if (GridKeys.RowAt(list, e.OriginalSource as DependencyObject) != null)
               GridKeys.Press(ok);
         };

         dialog.Frame(heading, body, ok, cancel, width: DesignTokens.Dialog.DefaultWidth);
         dialog.Loaded += (s, e) => search.Focus();
         return dialog;
      }
   }
}
