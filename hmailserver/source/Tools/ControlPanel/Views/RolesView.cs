// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Linq;
using System.Collections.Generic;
using System.Windows;
using System.Windows.Controls;
using hMailServer.ControlPanel.Services;

using Card = hMailServer.ControlPanel.Views.Scaffold.Card;
using EmptyState = hMailServer.ControlPanel.Views.Scaffold.EmptyState;
using InlineNotice = hMailServer.ControlPanel.Views.Scaffold.InlineNotice;
using PageHeader = hMailServer.ControlPanel.Views.Scaffold.PageHeader;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Views
{
   /// <summary>
   /// Who may administer what: the roles this server has, and the accounts that
   /// hold them.
   ///
   /// Until September 2026 this program had two answers to that question and no
   /// third - the server administrator, and an account with an admin level -
   /// and the only place either was visible was a combo box in the account
   /// editor. A role is the named, listed version of the same idea: a help desk
   /// that may reset a password and nothing else, an auditor that may read
   /// everything and change nothing, a domain administrator confined to named
   /// domains rather than to the one its own mailbox sits in.
   ///
   /// The four built-in roles cannot be edited here or anywhere else: their
   /// permissions are in the server's code, not in its database, so that an
   /// UPDATE against the table cannot hand somebody a permission. Custom roles
   /// are made over the REST API and the Control Deck; this page lists them and
   /// gives them out, which is what an administrator standing at this machine
   /// actually needs to do.
   ///
   /// An account that is not listed here is not without authority: it holds
   /// whatever its admin level has always given it. That is why the upgrade
   /// writes no rows and nobody's authority changed on the day roles arrived,
   /// and it is said on the page rather than only in the documentation.
   /// </summary>
   public class RolesView : UserControl, IPageLifecycle
   {
      private sealed class RoleRow
      {
         public string Name { get; set; }
         public string Purpose { get; set; }
         public string BuiltIn { get; set; }
         public string Permissions { get; set; }
      }

      private sealed class AssignmentRow
      {
         public string Address { get; set; }
         public string Role { get; set; }
         public string Domains { get; set; }
      }

      private readonly DataGrid roleGrid_ = new() { AutoGenerateColumns = false, IsReadOnly = true };
      private readonly DataGrid assignmentGrid_ = new() { AutoGenerateColumns = false, IsReadOnly = true };
      private readonly ComboBox roleChoice_ = new() { MinWidth = 160, Margin = new Thickness(0, 0, 8, 0) };
      private readonly Wpf.Ui.Controls.TextBox address_ = new() { MinWidth = 220, Margin = new Thickness(0, 0, 8, 0) };
      private readonly Wpf.Ui.Controls.TextBox domains_ = new() { MinWidth = 180, Margin = new Thickness(0, 0, 8, 0) };
      private readonly InlineNotice notice_ = new() { Visibility = Visibility.Collapsed };
      private readonly EmptyState empty_ = new()
      {
         Icon = Wpf.Ui.Controls.SymbolRegular.Shield24,
         Visibility = Visibility.Collapsed
      };

      public RolesView()
      {
         var root = new Grid();
         root.SetResourceReference(MarginProperty, "AppPagePadding");
         root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
         root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });

         var actions = new StackPanel { Orientation = Orientation.Horizontal };

         var reload = new Wpf.Ui.Controls.Button { Content = L("_Reload") };
         System.Windows.Automation.AutomationProperties.SetAutomationId(reload, "roles-reload");
         reload.Click += (s, e) => Reload_();
         actions.Children.Add(reload);

         root.Children.Add(new PageHeader
         {
            Title = L("Administrator roles"),
            Subtitle = L("A role is a named set of permissions, and an assignment gives one to an account in named domains. The four built-in roles cannot be edited: their permissions are in the server's code so that a change to the database cannot hand somebody authority. An account with no role here holds whatever its admin level has always given it."),
            Actions = actions
         });

         var cards = new StackPanel { Margin = new Thickness(0, 0, 12, 0), MaxWidth = 980, HorizontalAlignment = HorizontalAlignment.Left };

         roleGrid_.Columns.Add(TextColumn_(L("Role"), nameof(RoleRow.Name), 150));
         roleGrid_.Columns.Add(TextColumn_(L("What it is for"), nameof(RoleRow.Purpose), 300));
         roleGrid_.Columns.Add(TextColumn_(L("Built in"), nameof(RoleRow.BuiltIn), 80));
         roleGrid_.Columns.Add(TextColumn_(L("Permissions"), nameof(RoleRow.Permissions), 380));
         cards.Children.Add(new Card { Content = roleGrid_ });

         assignmentGrid_.Columns.Add(TextColumn_(L("Account"), nameof(AssignmentRow.Address), 260));
         assignmentGrid_.Columns.Add(TextColumn_(L("Role"), nameof(AssignmentRow.Role), 150));
         assignmentGrid_.Columns.Add(TextColumn_(L("Domains"), nameof(AssignmentRow.Domains), 260));

         // Each box has its caption above it, as the Domains page's forms do: a
         // placeholder was the only name these had, and it is gone the moment
         // something is typed - and the role list had none at all. The buttons sit
         // on the boxes' baseline, and the row wraps in a narrow window.
         var form = new WrapPanel { Margin = new Thickness(0, 10, 0, 0) };
         // The hint goes in PlaceholderText and the Text stays a literal: a
         // value filled from the catalogue is what turned the sign-in user-name
         // box into a Chinese word in issue 156.
         address_.PlaceholderText = L("Account address");
         domains_.PlaceholderText = L("Domains, comma separated");
         System.Windows.Automation.AutomationProperties.SetName(address_, L("Account address"));
         System.Windows.Automation.AutomationProperties.SetName(roleChoice_, L("Role"));
         System.Windows.Automation.AutomationProperties.SetName(domains_, L("Domains, comma separated"));
         address_.Margin = roleChoice_.Margin = domains_.Margin = new Thickness(0);
         form.Children.Add(FormField_(L("Account"), address_));
         form.Children.Add(FormField_(L("Role"), roleChoice_));
         form.Children.Add(FormField_(L("Domains"), domains_));

         var give = new Wpf.Ui.Controls.Button
         {
            Content = L("_Give the role"),
            Appearance = Wpf.Ui.Controls.ControlAppearance.Primary,
            Margin = new Thickness(0, 0, 8, 12),
            VerticalAlignment = VerticalAlignment.Bottom
         };
         System.Windows.Automation.AutomationProperties.SetAutomationId(give, "roles-assign");
         give.Click += (s, e) => Assign_();
         form.Children.Add(give);

         var take = new Wpf.Ui.Controls.Button
         {
            Content = L("_Take the role away"),
            Margin = new Thickness(0, 0, 0, 12),
            VerticalAlignment = VerticalAlignment.Bottom
         };
         System.Windows.Automation.AutomationProperties.SetAutomationId(take, "roles-unassign");
         take.Click += (s, e) => Unassign_();
         form.Children.Add(take);

         var held = new StackPanel();
         held.Children.Add(assignmentGrid_);
         held.Children.Add(empty_);
         held.Children.Add(form);
         held.Children.Add(notice_);
         cards.Children.Add(new Card { Content = held, Margin = new Thickness(0, 12, 0, 0) });

         var scroll = new ScrollViewer { Content = cards, VerticalScrollBarVisibility = ScrollBarVisibility.Auto };
         Grid.SetRow(scroll, 1);
         root.Children.Add(scroll);

         Content = root;
      }

      /// <summary>One box of the form with its caption above it.</summary>
      private static Scaffold.FieldRow FormField_(string caption, FrameworkElement editor)
         => new() { Label = caption, Content = editor, Margin = new Thickness(0, 0, 8, 12) };

      // Trimmed: a role's purpose and permissions are sentences in fixed
      // columns, and ended mid-word; the cell's tool tip holds the whole.
      private static DataGridTextColumn TextColumn_(string header, string property, double width)
         => GridStyles.Trimmed(new DataGridTextColumn
         {
            Header = header,
            Binding = new System.Windows.Data.Binding(property),
            Width = new DataGridLength(width)
         });

      public void OnEnter() => Reload_();

      public void OnLeave() { }

      private void Reload_()
      {
         notice_.Visibility = Visibility.Collapsed;

         var roles = new List<RoleRow>();
         var assignments = new List<AssignmentRow>();

         try
         {
            // Lines rather than a collection: a new COM interface needs an
            // elevated registration on every machine, and a listing does not
            // earn that. name, built-in, description, permissions.
            foreach (string[] parts in Lines_((string)ServerSession.Current.Application.Roles).Select(line => line.Split('\t')).Where(parts => parts.Length >= 4))
            {
               roles.Add(new RoleRow
               {
                  Name = parts[0],
                  BuiltIn = parts[1] == "1" ? L("Yes") : "",
                  Purpose = parts[2],
                  Permissions = parts[3].Replace(",", ", ")
               });
            }

            foreach (string[] parts in Lines_((string)ServerSession.Current.Application.RoleAssignments).Select(line => line.Split('\t')).Where(parts => parts.Length >= 3))
            {
               // What an empty list reaches is the server's to say (the fourth
               // field): every domain for serveradmin and auditor, the account's
               // own for every other role.
               bool everyDomain = parts.Length > 3 && parts[3] == "1";

               assignments.Add(new AssignmentRow
               {
                  Address = parts[0],
                  Role = parts[1],
                  Domains = everyDomain ? L("every domain") : (parts[2].Length > 0 ? parts[2] : L("the account's own domain"))
               });
            }
         }
         catch (Exception why) when (!ExceptionPolicy.IsFatal(why))
         {
            Warn_(ServerSession.DescribeComError(why));
            return;
         }

         roleGrid_.ItemsSource = roles;
         assignmentGrid_.ItemsSource = assignments;

         assignmentGrid_.Visibility = assignments.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
         empty_.Text = L("Nobody has been given a role yet. An account without one holds the authority its admin level gives it, exactly as before roles existed.");
         empty_.Visibility = assignments.Count > 0 ? Visibility.Collapsed : Visibility.Visible;

         object chosen = roleChoice_.SelectedItem;
         roleChoice_.ItemsSource = roles.ConvertAll(r => r.Name);
         roleChoice_.SelectedItem = chosen;
         if (roleChoice_.SelectedItem == null && roles.Count > 0)
            roleChoice_.SelectedIndex = 0;
      }

      private static IEnumerable<string> Lines_(string value)
         => (value ?? "").Split(new[] { "\r\n", "\n" }, StringSplitOptions.RemoveEmptyEntries);

      private void Assign_()
      {
         string address = (address_.Text ?? "").Trim();

         if (address.Length == 0)
         {
            Warn_(L("An account address is required."));
            return;
         }

         if (roleChoice_.SelectedItem == null)
         {
            Warn_(L("Choose a role to give."));
            return;
         }

         Write_(address, (string)roleChoice_.SelectedItem, (domains_.Text ?? "").Trim());
      }

      private void Unassign_()
      {
         if (assignmentGrid_.SelectedItem is AssignmentRow row)
         {
            Write_(row.Address, "", "");
            return;
         }

         string address = (address_.Text ?? "").Trim();

         if (address.Length == 0)
         {
            Warn_(L("Choose an assignment in the list, or type the account address."));
            return;
         }

         Write_(address, "", "");
      }

      private void Write_(string address, string role, string domains)
      {
         try
         {
            ServerSession.Current.Application.AssignRole(address, role, domains);
         }
         catch (Exception why) when (!ExceptionPolicy.IsFatal(why))
         {
            Warn_(ServerSession.DescribeComError(why));
            return;
         }

         address_.Text = "";
         domains_.Text = "";
         Reload_();
      }

      private void Warn_(string message)
      {
         notice_.Level = StatusLevel.Warning;
         notice_.Text = message;
         notice_.Visibility = Visibility.Visible;
      }
   }
}
