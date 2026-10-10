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

         // The permissions as the server lists them, for RoleGrant.ReachesEverything.
         public string Tokens { get; set; }

         // What a screen reader says for the row: the role and what it is for, not
         // the name of this class, which is what a grid row is announced as otherwise.
         public override string ToString() => RowSpeech.Of(Name, Purpose);
      }

      // The roles as the last reload read them, by name: what Give the role asks
      // about before it gives one that reaches everything.
      private List<RoleRow> roles_ = new();

      // "Choose a role" over the empty box: the box opens on no role, and a
      // combo box has no placeholder of its own.
      private readonly TextBlock rolePlaceholder_ = new()
      {
         IsHitTestVisible = false,
         VerticalAlignment = VerticalAlignment.Center,
         Margin = new Thickness(11, 0, 32, 0),
         TextTrimming = TextTrimming.CharacterEllipsis
      };

      private sealed class AssignmentRow
      {
         public string Address { get; set; }
         public string Role { get; set; }
         public string Domains { get; set; }

         // The account, its role and its domains, as one statement.
         public override string ToString() => RowSpeech.Of(Address, Role, Domains);
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

      // A read that failed, above the cards, where the roles were; notice_ under
      // the form is for what the form says (Services/ReadFailure).
      private readonly InlineNotice readNotice_ = new() { Visibility = Visibility.Collapsed };

      // Give the role waits for a read that works; Take the role away for a row
      // of a read that worked, chosen in the list.
      private Wpf.Ui.Controls.Button give_;
      private Wpf.Ui.Controls.Button take_;

      public RolesView()
      {
         var root = new Grid();
         root.SetResourceReference(MarginProperty, "AppPagePadding");
         root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
         root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });

         var actions = new StackPanel { Orientation = Orientation.Horizontal };

         var reload = new Wpf.Ui.Controls.Button { Content = L("_Refresh") };
         System.Windows.Automation.AutomationProperties.SetAutomationId(reload, "roles-reload");
         reload.Click += (s, e) => Reload_();
         actions.Children.Add(reload);

         root.Children.Add(new PageHeader
         {
            PageKey = "roles",
            // What the page is, in one sentence: the built-in roles and the admin
            // level are said on the roles card, where the subtitle said them in two
            // and the empty state said the second again (review finding F085).
            Subtitle = L("Who may administer what: the roles this server has, and the accounts that hold them."),
            Actions = actions
         });

         // The cards are as wide as the page, as the header is, less the room the
         // page's scroll bar takes on the right: capped at 980 pixels they ended
         // short of the header's button, and the assignments grid filled half its
         // card (review finding F068).
         var cards = new StackPanel { Margin = new Thickness(0, 0, DesignTokens.Space.Md, 0) };
         cards.Children.Add(readNotice_);

         // The role and Built in as wide as their words, the two sentences sharing
         // the rest two to three, each down to a floor, and no sideways scrolling:
         // four fixed columns of 910 pixels in a card of about 900 put a scroll bar
         // under the rows and cut the very sentences the grid is for at 1280
         // (F068; ControlPanelDesign.md, Grids). The sentences wrap rather than
         // end in an ellipsis: they are what the grid is for, and it holds a
         // handful of roles.
         roleGrid_.Columns.Add(TextColumn_(L("Role"), nameof(RoleRow.Name), FixedWidth_(L("Role"), 140)));
         roleGrid_.Columns.Add(TextColumn_(L("What it is for"), nameof(RoleRow.Purpose), StarWidth_(2), wrap: true));
         roleGrid_.Columns.Add(TextColumn_(L("Built in"), nameof(RoleRow.BuiltIn), FixedWidth_(L("Built in"), 80, L("Yes"), L("No"))));
         roleGrid_.Columns.Add(TextColumn_(L("Permissions"), nameof(RoleRow.Permissions), StarWidth_(3), wrap: true));
         ScrollViewer.SetHorizontalScrollBarVisibility(roleGrid_, ScrollBarVisibility.Disabled);
         // Each card and its grid say what they hold: only the column headers told
         // the roles from who holds one, and neither grid had a name.
         System.Windows.Automation.AutomationProperties.SetName(roleGrid_, L("Roles"));
         System.Windows.Automation.AutomationProperties.SetName(assignmentGrid_, L("Who holds a role"));
         cards.Children.Add(new Card
         {
            Title = L("Roles"),
            Description = Loc.Sentences(
               L("The four built-in roles cannot be edited: their permissions are in the server's code so that a change to the database cannot hand somebody authority."),
               L("An account with no role here holds whatever its admin level has always given it.")),
            Content = roleGrid_
         });

         // The account and its domains share the card's width, the role between
         // them as wide as its words: at 260, 150 and 260 pixels the grid ended
         // some 230 pixels short of its card at 1280 (F068).
         assignmentGrid_.Columns.Add(TextColumn_(L("Account"), nameof(AssignmentRow.Address), StarWidth_(2)));
         assignmentGrid_.Columns.Add(TextColumn_(L("Role"), nameof(AssignmentRow.Role), FixedWidth_(L("Role"), 140)));
         assignmentGrid_.Columns.Add(TextColumn_(L("Domains"), nameof(AssignmentRow.Domains), StarWidth_(2)));
         ScrollViewer.SetHorizontalScrollBarVisibility(assignmentGrid_, ScrollBarVisibility.Disabled);

         // Each box has its caption above it, as the Domains page's forms do: a
         // placeholder was the only name these had, and it is gone the moment
         // something is typed - and the role list had none at all. The buttons sit
         // on the boxes' baseline, and the row wraps in a narrow window.
         var form = new WrapPanel { Margin = new Thickness(0, 10, 0, 0) };
         // The hint goes in PlaceholderText and the Text stays a literal: a
         // value filled from the catalogue is what turned the sign-in user-name
         // box into a Chinese word in issue 156. The placeholders are examples,
         // where they said the captions above them again (review finding F087),
         // and are the same in every language: example.com is a name, not a word.
         address_.PlaceholderText = "helpdesk@example.com"; // no-loc: an example address
         domains_.PlaceholderText = "example.com, example.net"; // no-loc: example domains
         address_.Margin = roleChoice_.Margin = domains_.Margin = new Thickness(0);
         form.Children.Add(FormField_(L("_Account"), address_));
         // The box opens on no role: it opened on the first the server lists,
         // serveradmin, beside the primary Give the role (review finding F055).
         rolePlaceholder_.Text = L("Choose a role");
         rolePlaceholder_.SetResourceReference(TextBlock.ForegroundProperty, "TextFillColorTertiaryBrush");
         System.Windows.Automation.AutomationProperties.SetHelpText(roleChoice_, L("Choose a role"));
         roleChoice_.SelectionChanged += (s, e) => ShowRolePlaceholder_();
         var roleBox = new Grid();
         roleBox.Children.Add(roleChoice_);
         roleBox.Children.Add(rolePlaceholder_);
         form.Children.Add(FormField_(L("R_ole"), roleBox));
         // What an empty box gives, which the grid showed only once the role was
         // given: the account's own domain for most roles, every domain for
         // serveradmin and auditor (RoleManager::ReachesEveryDomainWhenUnlisted).
         // The row is as wide as its hint wraps to, so that the hint does not take
         // the form's whole line.
         Scaffold.FieldRow domainsRow = FormField_(L("_Domains"), domains_);
         domainsRow.Hint = L("Empty: the account's own domain, or every domain for serveradmin and auditor. * is every domain for any role.");
         domainsRow.Width = DomainsRowWidth;
         form.Children.Add(domainsRow);

         give_ = new Wpf.Ui.Controls.Button
         {
            Content = L("_Give the role"),
            Appearance = Wpf.Ui.Controls.ControlAppearance.Primary,
            Margin = new Thickness(0, 0, 8, 12),
            VerticalAlignment = VerticalAlignment.Bottom
         };
         System.Windows.Automation.AutomationProperties.SetAutomationId(give_, "roles-assign");
         give_.Click += (s, e) => Assign_();
         form.Children.Add(OnTheBoxesLine_(give_, L("_Domains")));
         // Enter in the form gives the role; Escape empties the two boxes.
         FormSubmit.Bind(form, give_, address_, domains_);

         take_ = new Wpf.Ui.Controls.Button
         {
            Content = L("_Take the role away"),
            Margin = new Thickness(0, 0, 0, 12),
            VerticalAlignment = VerticalAlignment.Bottom
         };
         System.Windows.Automation.AutomationProperties.SetAutomationId(take_, "roles-unassign");
         take_.Click += (s, e) => Unassign_();
         form.Children.Add(OnTheBoxesLine_(take_, L("_Domains")));

         // Take the role away takes it from the assignment chosen in the list and
         // nothing else, so it can be pressed only while one is chosen, and it is the
         // list pages' destructive action, red words on a secondary button (review
         // finding F069). It was live at all times and, with nothing chosen, took
         // the role of whatever address was typed into the form for giving one.
         ButtonStyles.DangerText(take_);
         SelectionGate.Bind(assignmentGrid_, take_);

         var held = new StackPanel();
         held.Children.Add(assignmentGrid_);
         held.Children.Add(empty_);
         held.Children.Add(form);
         held.Children.Add(notice_);
         cards.Children.Add(new Card { Title = L("Who holds a role"), Content = held, Margin = new Thickness(0, 12, 0, 0) });

         var scroll = new ScrollViewer { Content = cards, VerticalScrollBarVisibility = ScrollBarVisibility.Auto };
         Grid.SetRow(scroll, 1);
         root.Children.Add(scroll);

         Content = root;
      }

      /// <summary>
      /// A caption's room above a button, drawn in the caption's style and never
      /// seen: it has no automation peer, so that a screen reader does not find a
      /// second Domains beside the box's own.
      /// </summary>
      private sealed class CaptionRoom : TextBlock
      {
         protected override System.Windows.Automation.Peers.AutomationPeer OnCreateAutomationPeer() => null;
      }

      /// <summary>
      /// The width of the Domains box's row, which its hint wraps to: as wide as
      /// the box was beside its old placeholder, so that the form's line still
      /// holds the three boxes and both buttons at 1280 pixels.
      /// </summary>
      private const double DomainsRowWidth = 230;

      /// <summary>
      /// One box of the form with its caption above it, at the top of the form's
      /// line, so that a hint or a message under one box leaves the others where
      /// they are.
      /// </summary>
      private static Scaffold.FieldRow FormField_(string caption, FrameworkElement editor)
         => new() { Label = caption, Content = editor, Margin = new Thickness(0, 0, 8, 12), VerticalAlignment = VerticalAlignment.Top };

      /// <summary>
      /// A button of the form on the boxes' line: under a caption's room - the
      /// caption of a box beside it, drawn in the caption's style and hidden - so
      /// that it stands beside the boxes whatever a box's hint or message adds
      /// under them. It stood at the bottom of the form's line, which the Domains
      /// box's hint would have put three lines below the boxes.
      /// </summary>
      private static FrameworkElement OnTheBoxesLine_(Wpf.Ui.Controls.Button button, string besideCaption)
      {
         var room = new CaptionRoom { Text = MnemonicText.Strip(besideCaption), Visibility = Visibility.Hidden, Margin = new Thickness(0, 0, 0, 4) };
         room.SetResourceReference(StyleProperty, "TextBody");
         button.VerticalAlignment = VerticalAlignment.Top;
         var line = new StackPanel { VerticalAlignment = VerticalAlignment.Top, Margin = button.Margin };
         button.Margin = new Thickness(0);
         line.Children.Add(room);
         line.Children.Add(button);
         return line;
      }

      // Trimmed, with the whole in the cell's tool tip, or wrapped: a role's
      // purpose and permissions are sentences, and ended mid-word in fixed
      // columns.
      private static DataGridTextColumn TextColumn_(string header, string property, (DataGridLength Width, double Floor) size, bool wrap = false)
      {
         var column = new DataGridTextColumn
         {
            Header = header,
            Binding = new System.Windows.Data.Binding(property),
            Width = size.Width,
            MinWidth = size.Floor
         };
         if (!wrap)
            return GridStyles.Trimmed(column);

         var lines = new Style(typeof(TextBlock), column.ElementStyle);
         lines.Setters.Add(new Setter(TextBlock.TextWrappingProperty, TextWrapping.Wrap));
         lines.Setters.Add(new Setter(FrameworkElement.MarginProperty, new Thickness(2, 6, 2, 6)));
         column.ElementStyle = lines;
         return column;
      }

      /// <summary>The floor of a column that shares the grid's width (a sentence's).</summary>
      private const double SentenceColumnFloor = 160;

      /// <summary>A column of words that fit: as wide as asked, its header and its widest word in the current language.</summary>
      private static (DataGridLength, double) FixedWidth_(string header, double width, params string[] words)
      {
         double floor = System.Math.Max(width, GridStyles.HeaderWidth(header));
         foreach (string word in words)
            floor = System.Math.Max(floor, GridStyles.CellWidth(word));
         return (new DataGridLength(floor), floor);
      }

      /// <summary>A column of sentences: a share of what the fixed columns leave, down to a floor.</summary>
      private static (DataGridLength, double) StarWidth_(double weight)
         => (new DataGridLength(weight, DataGridLengthUnitType.Star), SentenceColumnFloor);

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
                  // Yes or No, as every other grid says it, where a custom role's
                  // cell was empty; a built-in role's purpose in the reader's
                  // language, where it was the English the database holds
                  // (review findings F086 and F084).
                  BuiltIn = parts[1] == "1" ? L("Yes") : L("No"),
                  Purpose = RoleWords.Purpose(parts[0], parts[1] == "1", parts[2]),
                  Permissions = parts[3].Replace(",", ", "),
                  Tokens = parts[3]
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
                  Domains = everyDomain ? L("every domain") : (parts[2].Length > 0 ? RoleWords.DomainsText(parts[2]) : L("the account's own domain"))
               });
            }
         }
         catch (Exception why) when (!ExceptionPolicy.IsFatal(why))
         {
            // Neither the rows of the last read nor "Nobody has been given a role
            // yet." is a fact this page has now: the roles, the assignments and
            // the choice of role go, the notice above the cards says what
            // happened, at Critical, with Try again, Give the role waits for a
            // read that works (Services/ReadFailure), and Take the role away, with
            // no row left to choose, cannot be pressed.
            ReadFailure.Show(readNotice_, empty_, assignmentGrid_, L("Could not read the roles: "), why, Reload_, give_);
            roleGrid_.ItemsSource = null;
            roleChoice_.ItemsSource = null;
            assignmentGrid_.Visibility = Visibility.Collapsed;
            return;
         }

         ReadFailure.Clear(readNotice_, give_);
         roleGrid_.ItemsSource = roles;
         roles_ = roles;
         assignmentGrid_.ItemsSource = assignments;

         assignmentGrid_.Visibility = assignments.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
         // The roles card says what an account without a role holds; the empty
         // state said it again (F085).
         empty_.Text = L("Nobody has been given a role yet.");
         empty_.Visibility = assignments.Count > 0 ? Visibility.Collapsed : Visibility.Visible;

         // A role chosen before stays chosen while the server still has it; none is
         // chosen for anybody.
         object chosen = roleChoice_.SelectedItem;
         roleChoice_.ItemsSource = roles.ConvertAll(r => r.Name);
         roleChoice_.SelectedItem = chosen;
         ShowRolePlaceholder_();
      }

      private void ShowRolePlaceholder_()
         => rolePlaceholder_.Visibility = roleChoice_.SelectedItem == null ? Visibility.Visible : Visibility.Collapsed;

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

         // A role that reaches everything is asked about first, Cancel being the
         // answer Enter gives.
         string role = (string)roleChoice_.SelectedItem;
         string tokens = roles_.Find(r => string.Equals(r.Name, role, StringComparison.Ordinal))?.Tokens;
         if (RoleGrant.ReachesEverything(role, tokens) &&
             !Dialogs.Confirm(RoleGrant.Question(address, role), L("Control Panel"), L("_Give the role"), destructive: true, cancelByDefault: true))
            return;

         Write_(address, role, RoleWords.DomainsToSend(domains_.Text));
      }

      /// <summary>
      /// Takes the role of the assignment chosen in the list away, after asking, the
      /// role and the account named and Cancel the answer Enter gives. Only that
      /// row: with none chosen it took the role of whatever address was typed into
      /// the form for giving one, without a word (review finding F069).
      /// </summary>
      private void Unassign_()
      {
         if (assignmentGrid_.SelectedItem is not AssignmentRow row)
            return;

         if (!Dialogs.Confirm(Confirmations.TakeRoleAway(row.Role, row.Address), L("Control Panel"), L("_Take the role away"), destructive: true))
            return;

         Write_(row.Address, "", "");
      }

      private void Write_(string address, string role, string domains)
      {
         try
         {
            ServerSession.Current.Application.AssignRole(address, role, domains);
         }
         catch (Exception why) when (!ExceptionPolicy.IsFatal(why))
         {
            Fail_(L("Could not change the role: ") + ServerSession.DescribeComError(why));
            return;
         }

         address_.Text = "";
         domains_.Text = "";
         Reload_();
      }

      /// <summary>A prompt for the form: something to fill in first.</summary>
      private void Warn_(string message)
      {
         notice_.Level = StatusLevel.Warning;
         notice_.Text = message;
         notice_.Visibility = Visibility.Visible;
      }

      /// <summary>
      /// A write that failed, at Critical: it is a failure, not a prompt, and the
      /// Groups page beside this one says the same kind of thing at that level.
      /// </summary>
      private void Fail_(string message)
      {
         notice_.Level = StatusLevel.Critical;
         notice_.Text = message;
         notice_.Visibility = Visibility.Visible;
      }
   }
}
