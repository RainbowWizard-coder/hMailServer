// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Text.Json;
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
   /// Journaling and dual delivery: a copy of a domain's mail, or of one
   /// account's, sent to an address.
   ///
   /// The archive this server has always had keeps files on this machine. A
   /// compliance archive and a migration both want the other thing - the mail
   /// somewhere else, in a system this one does not own - and that is what a rule
   /// here does. Exchange calls it journaling and sends an envelope report with
   /// the message attached; Google calls it dual delivery and sends the message
   /// itself. They are the same mechanism with two shapes of copy, so one rule
   /// says which shape it wants.
   ///
   /// Three Utilities methods, as the suppression list's page uses: a listing
   /// that answers the same JSON document the REST route does, a write that takes
   /// the words rather than the numbers the column holds, and a delete. A new COM
   /// interface would need an elevated registration on every machine, and a page
   /// of seven fields does not earn that.
   ///
   /// Server administrators only, and the page says why: a rule is a standing
   /// instruction that somebody's mail is copied to an address they cannot see.
   /// </summary>
   public class JournalingView : UserControl, IPageLifecycle
   {
      private sealed class RuleRow
      {
         public long Id { get; init; }
         public string Target { get; init; } = "";
         public string Scope { get; init; } = "";
         public string Direction { get; init; } = "";
         public string Mode { get; init; } = "";
         public string JournalAddress { get; init; } = "";
         // On or Off (JournalRuleText.State): an Off rule's cell was empty.
         public string Enabled { get; init; } = "";
         public bool IsEnabled { get; init; }
         public string Note { get; init; } = "";

         // Whose mail, which way and what is sent, then where it goes with its
         // column's name, whether the rule is on or off, and the note.
         public override string ToString() => RowSpeech.Of(Target, Scope, Direction, Mode,
            RowSpeech.Labelled(L("Where it goes"), JournalAddress), Enabled, Note);
      }

      private readonly DataGrid rules_ = new()
      {
         AutoGenerateColumns = false,
         IsReadOnly = true,
         CanUserAddRows = false,
         CanUserDeleteRows = false,
         SelectionMode = DataGridSelectionMode.Single,
         MinHeight = 200
      };

      // The form's boxes take their column's width (BuildForm_).
      private readonly ComboBox scope_ = new() { HorizontalAlignment = HorizontalAlignment.Stretch };
      private readonly ComboBox direction_ = new() { HorizontalAlignment = HorizontalAlignment.Stretch };
      private readonly ComboBox mode_ = new() { HorizontalAlignment = HorizontalAlignment.Stretch };
      private readonly Wpf.Ui.Controls.TextBox target_ = new();
      private readonly Wpf.Ui.Controls.TextBox address_ = new();
      private readonly Wpf.Ui.Controls.TextBox note_ = new();

      private readonly InlineNotice notice_ = new() { Visibility = Visibility.Collapsed };

      // What the last add, switch or removal did, above the list it acted on, at
      // its level - a Good notice that names the rule, or the server's refusal.
      // The three acts said nothing: the list was read again, and whether a rule
      // had been added, switched or removed was for the reader to find in it
      // (review finding F255). notice_ under the form is for what the form says.
      private readonly InlineNotice outcome_ = new() { Visibility = Visibility.Collapsed };

      // Turn off or Turn on, by the selected rule (JournalRuleText.Toggle).
      private Wpf.Ui.Controls.Button toggle_;

      // A read that failed, above the cards, where the rules were; notice_ under
      // the form is for what the form says (Services/ReadFailure).
      private readonly InlineNotice readNotice_ = new() { Visibility = Visibility.Collapsed };

      // Add the rule waits for a read that works.
      private Wpf.Ui.Controls.Button add_;

      // The card that holds the list, its empty state and the actions on the
      // chosen rule: hidden with them while a read has failed, since a bare card
      // under the failure notice is an element with nothing in it.
      private Card listCard_;

      // The actions on the rule chosen in the list, shown while there is a list.
      private readonly StackPanel chosen_ = new() { Orientation = Orientation.Horizontal, Margin = new Thickness(0, 10, 0, 0) };
      private readonly EmptyState empty_ = new()
      {
         Icon = Wpf.Ui.Controls.SymbolRegular.DocumentCopy24,
         Visibility = Visibility.Collapsed
      };

      public JournalingView()
      {
         var root = new Grid();
         root.SetResourceReference(MarginProperty, "AppPagePadding");
         root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
         root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });

         var actions = new StackPanel { Orientation = Orientation.Horizontal };

         var reload = new Wpf.Ui.Controls.Button { Content = L("_Refresh") };
         System.Windows.Automation.AutomationProperties.SetAutomationId(reload, "journal-reload");
         reload.Click += (s, e) => Reload_();
         actions.Children.Add(reload);

         root.Children.Add(new PageHeader
         {
            PageKey = "journaling",
            Subtitle = L("A copy of a domain's mail, or of one account's, sent to an address - as an envelope report with the message attached, or as the message itself. Nothing is copied until a rule exists here, and a rule is not the archive: it sends mail elsewhere rather than keeping files on this machine."),
            Actions = actions
         });

         // The page's width, as the other list pages have it: held left at 1,100
         // pixels, the cards were as wide as their fixed columns, and with columns
         // that share the width they would be as narrow as their floors.
         var cards = new StackPanel { Margin = new Thickness(0, 0, 12, 0) };

         // The two addresses and the note share the card's width, Where it goes
         // twice as much as each of the others since it is the one an archive's
         // or a migration's address is told by, each cut short with an ellipsis
         // and whole in its tool tip; the four short words take
         // their own width. Seven fixed columns came to 1,100 pixels on a card of
         // about 900, so that the grid scrolled sideways in a 1280 window and the
         // note was never seen, Where it goes was off the card in the smallest
         // window, and "Beide Richtungen" was cut to "Beide Richtung" with nothing
         // to say so (review finding F255).
         rules_.Columns.Add(SharedColumn_(L("Whose mail"), nameof(RuleRow.Target), 160, 1));
         rules_.Columns.Add(FittedColumn_(L("Domain or account"), nameof(RuleRow.Scope)));
         rules_.Columns.Add(FittedColumn_(L("Which way"), nameof(RuleRow.Direction)));
         rules_.Columns.Add(FittedColumn_(L("What is sent"), nameof(RuleRow.Mode)));
         rules_.Columns.Add(SharedColumn_(L("Where it goes"), nameof(RuleRow.JournalAddress), 160, 2));
         rules_.Columns.Add(FittedColumn_(L("On"), nameof(RuleRow.Enabled)));
         rules_.Columns.Add(SharedColumn_(L("Note"), nameof(RuleRow.Note), 80, 1));

         // What acts on the rule chosen in the list sits with the list, and can be
         // pressed only while one is chosen; beside the form below it read as part
         // of adding a rule. It says which way it will switch the rule: "Switch
         // the rule over" said neither.
         toggle_ = new Wpf.Ui.Controls.Button { Margin = new Thickness(0, 0, 8, 0) };
         ShowToggle_(true);
         System.Windows.Automation.AutomationProperties.SetAutomationId(toggle_, "journal-toggle");
         toggle_.Click += (s, e) => Toggle_();
         rules_.SelectionChanged += (s, e) => { if (rules_.SelectedItem is RuleRow row) ShowToggle_(row.IsEnabled); };

         var remove = new Wpf.Ui.Controls.Button { Content = L("Re_move the rule") };
         ButtonStyles.DangerText(remove);
         System.Windows.Automation.AutomationProperties.SetAutomationId(remove, "journal-remove");
         remove.Click += (s, e) => Remove_();

         chosen_.Children.Add(toggle_);
         chosen_.Children.Add(remove);
         SelectionGate.Bind(rules_, toggle_, remove);
         // Delete removes the selected rule after asking; Insert goes to the form.
         GridKeys.Wire(rules_, null, GridKeys.Pressing(remove), GridKeys.Focusing(target_));

         cards.Children.Add(readNotice_);
         System.Windows.Automation.AutomationProperties.SetAutomationId(outcome_, "journal-outcome");
         cards.Children.Add(outcome_);

         var listed = new StackPanel();
         listed.Children.Add(rules_);
         listed.Children.Add(empty_);
         listed.Children.Add(chosen_);
         listCard_ = new Card { Content = listed };
         cards.Children.Add(listCard_);

         scope_.Items.Add(L("Domain"));
         scope_.Items.Add(L("Account"));
         scope_.SelectedIndex = 0;

         direction_.Items.Add(L("Both ways"));
         direction_.Items.Add(L("Arriving"));
         direction_.Items.Add(L("Sent"));
         direction_.SelectedIndex = 0;

         mode_.Items.Add(L("Envelope report"));
         mode_.Items.Add(L("The message itself"));
         mode_.SelectedIndex = 0;

         // The hint goes in PlaceholderText and the Text stays a literal: a value
         // filled from the catalogue is what turned the sign-in user-name box into
         // a Chinese word in issue 156. Each placeholder is an example of what the
         // box takes, where it repeated the caption above it ("Where the copy
         // goes" under Where it goes); Whose mail's follows the choice beside it.
         target_.PlaceholderText = JournalRuleText.TargetExample(false);
         scope_.SelectionChanged += (s, e) => target_.PlaceholderText = JournalRuleText.TargetExample(scope_.SelectedIndex == 1);
         address_.PlaceholderText = "journal@archive.example"; // no-loc: an example address, the same in every language
         note_.PlaceholderText = L("Optional");
         // Each box has its caption above it - the grid's own column names - as
         // the Domains page's forms do: a placeholder was the only name the boxes
         // had, gone once something is typed, and the three lists had none. The
         // caption names the box (FieldRow), in every language: a name set here
         // as well was a second string, which three catalogues translated as
         // another word than the caption's.
         add_ = new Wpf.Ui.Controls.Button
         {
            Content = L("_Add the rule"),
            Appearance = Wpf.Ui.Controls.ControlAppearance.Primary,
            HorizontalAlignment = HorizontalAlignment.Right,
            Margin = new Thickness(0)
         };
         System.Windows.Automation.AutomationProperties.SetAutomationId(add_, "journal-add");
         add_.Click += (s, e) => Add_();
         Grid form = BuildForm_();
         // Enter in the form adds the rule; Escape empties its three boxes.
         FormSubmit.Bind(form, add_, target_, address_, note_);

         var editing = new StackPanel();
         editing.Children.Add(form);
         editing.Children.Add(notice_);
         // Titled, as the other pages' add forms are (Add route, Add port): the
         // untitled card under the list did not say what its boxes were for.
         cards.Children.Add(new Card { Title = L("Add a journaling rule"), Content = editing, Margin = new Thickness(0, 12, 0, 0) });

         var scroll = new ScrollViewer { Content = cards, VerticalScrollBarVisibility = ScrollBarVisibility.Auto };
         Grid.SetRow(scroll, 1);
         root.Children.Add(scroll);

         Content = root;
      }

      /// <summary>
      /// The add form: its boxes in the grid's order, three to a line, and Add
      /// the rule under them at the right edge. In a wrapping row the boxes kept
      /// widths of their own, Domain or account came before Whose mail, and in a
      /// 1280 window Note wrapped onto a line by itself beside the button (review
      /// finding F255).
      /// </summary>
      private Grid BuildForm_()
      {
         var form = new Grid();
         for (int i = 0; i < 3; i++)
            form.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
         for (int i = 0; i < 3; i++)
            form.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });

         (string Caption, FrameworkElement Box)[] fields =
         {
            (L("W_hose mail"), target_), (L("_Domain or account"), scope_), (L("Wh_ich way"), direction_),
            (L("What is _sent"), mode_), (L("Where it _goes"), address_), (L("_Note"), note_)
         };
         for (int i = 0; i < fields.Length; i++)
         {
            fields[i].Box.Margin = new Thickness(0);
            Scaffold.FieldRow row = FormField_(fields[i].Caption, fields[i].Box);
            // The last column keeps no gap at its right, so the boxes end where Add does.
            if (i % 3 == 2)
               row.Margin = new Thickness(0, 0, 0, 12);
            Grid.SetRow(row, i / 3);
            Grid.SetColumn(row, i % 3);
            form.Children.Add(row);
         }

         Grid.SetRow(add_, 2);
         Grid.SetColumn(add_, 0);
         Grid.SetColumnSpan(add_, 3);
         form.Children.Add(add_);
         return form;
      }

      /// <summary>One box of the form with its caption above it.</summary>
      private static Scaffold.FieldRow FormField_(string caption, FrameworkElement editor)
         => new() { Label = caption, Content = editor, Margin = new Thickness(0, 0, 8, 12) };

      /// <summary>
      /// A column that shares the width left by the others by
      /// <paramref name="weight"/>, floored at <paramref name="floor"/> or its
      /// header's width where that is more, its cells cut short with an
      /// ellipsis and whole in their tool tips.
      /// </summary>
      private static DataGridTextColumn SharedColumn_(string header, string property, double floor, double weight)
         => GridStyles.Trimmed(new DataGridTextColumn
         {
            Header = header,
            Binding = new System.Windows.Data.Binding(property),
            Width = new DataGridLength(weight, DataGridLengthUnitType.Star),
            MinWidth = Math.Max(floor, GridStyles.HeaderWidth(header))
         });

      /// <summary>A column of short words, as wide as its widest and never narrower than its header.</summary>
      private static DataGridTextColumn FittedColumn_(string header, string property)
         => new DataGridTextColumn
         {
            Header = header,
            Binding = new System.Windows.Data.Binding(property),
            Width = DataGridLength.Auto,
            MinWidth = GridStyles.HeaderWidth(header)
         };

      /// <summary>
      /// The selected rule's switch says which way it will go: Turn off for a rule
      /// that is on, Turn on for one that is off. Each has a key of its own, since
      /// both are captions of the page (Alt+T and Alt+O in English).
      /// </summary>
      private void ShowToggle_(bool enabled)
      {
         if (enabled)
            toggle_.Content = L("_Turn off");
         else
            toggle_.Content = L("Turn _on");
      }

      /// <summary>What an act on a rule did, above the list, at its level.</summary>
      private void Say_(StatusLevel level, string text)
      {
         outcome_.Level = level;
         outcome_.Text = text;
         outcome_.Visibility = Visibility.Visible;
      }

      /// <summary>Selects the rule with this id after a read, and brings it into view.</summary>
      private void Select_(long id)
      {
         if (rules_.ItemsSource is not List<RuleRow> rows)
            return;

         RuleRow found = rows.Find(r => r.Id == id);
         if (found == null)
            return;

         rules_.SelectedItem = found;
         rules_.ScrollIntoView(found);
      }

      public void OnEnter() => Reload_();

      public void OnLeave() { }

      // The server's words, translated for the grid and for the two pickers.
      // Anything the server one day adds that this program does not know is shown
      // as the server said it, rather than as an empty cell.
      private static string ScopeCaption_(string scope)
         => scope == "account" ? L("Account") : scope == "domain" ? L("Domain") : scope;

      private static string DirectionCaption_(string direction)
         => direction switch
         {
            "inbound" => L("Arriving"),
            "outbound" => L("Sent"),
            "both" => L("Both ways"),
            _ => direction
         };

      private static string ModeCaption_(string mode)
         => mode switch
         {
            "report" => L("Envelope report"),
            "copy" => L("The message itself"),
            _ => mode
         };

      // Refresh reads the list again and says nothing of an act before it; an act
      // reads it first and then says what it did (Say_), so that its outcome is
      // seen over the list it changed.
      private void Reload_()
      {
         notice_.Visibility = Visibility.Collapsed;
         outcome_.Visibility = Visibility.Collapsed;

         var rows = new List<RuleRow>();

         try
         {
            dynamic utilities = ServerSession.Current.Application.Utilities;
            string document = (string)utilities.GetJournalRules();

            using JsonDocument parsed = JsonDocument.Parse(string.IsNullOrEmpty(document) ? "{\"items\":[]}" : document);

            if (parsed.RootElement.TryGetProperty("items", out JsonElement items) && items.ValueKind == JsonValueKind.Array)
            {
               foreach (JsonElement item in items.EnumerateArray())
               {
                  bool enabled = item.TryGetProperty("enabled", out JsonElement on) && on.ValueKind == JsonValueKind.True;

                  rows.Add(new RuleRow
                  {
                     Id = item.TryGetProperty("id", out JsonElement id) && id.TryGetInt64(out long value) ? value : 0,
                     Target = Text_(item, "target"),
                     Scope = ScopeCaption_(Text_(item, "scope")),
                     Direction = DirectionCaption_(Text_(item, "direction")),
                     Mode = ModeCaption_(Text_(item, "mode")),
                     JournalAddress = Text_(item, "journal_address"),
                     Enabled = JournalRuleText.State(enabled),
                     IsEnabled = enabled,
                     Note = Text_(item, "note")
                  });
               }
            }
         }
         catch (JsonException)
         {
            Unread_(null, L("The server's answer could not be read."));
            return;
         }
         catch (Exception why) when (!ExceptionPolicy.IsFatal(why))
         {
            Unread_(L("Could not read the journaling rules: "), ServerSession.DescribeComError(why));
            return;
         }

         ReadFailure.Clear(readNotice_, add_);
         listCard_.Visibility = Visibility.Visible;
         rules_.ItemsSource = rows;
         rules_.Visibility = rows.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
         chosen_.Visibility = rules_.Visibility;
         empty_.Text = L("No journaling rules. Nothing is copied anywhere, and the check costs one comparison for each message the server accepts.");
         empty_.Visibility = rows.Count > 0 ? Visibility.Collapsed : Visibility.Visible;
      }

      /// <summary>
      /// A read that failed: neither the rows of the last read nor "No journaling
      /// rules." is a fact this page has now, so both go, with the card that held
      /// them, the notice above says what happened, with Try again, and Add the
      /// rule waits for a read that works (Services/ReadFailure).
      /// </summary>
      private void Unread_(string prefix, string why)
      {
         ReadFailure.Show(readNotice_, empty_, rules_, prefix, why, Reload_, add_);
         rules_.Visibility = Visibility.Collapsed;
         chosen_.Visibility = Visibility.Collapsed;
         listCard_.Visibility = Visibility.Collapsed;
      }

      private static string Text_(JsonElement item, string name)
         => item.TryGetProperty(name, out JsonElement value) && value.ValueKind == JsonValueKind.String ? (value.GetString() ?? "") : "";

      private void Add_()
      {
         string target = (target_.Text ?? "").Trim();
         string address = (address_.Text ?? "").Trim();

         if (target.Length == 0)
         {
            Warn_(L("A domain or an account address is required."));
            return;
         }

         if (address.Length == 0)
         {
            Warn_(L("The address the copy goes to is required."));
            return;
         }

         string scope = scope_.SelectedIndex == 1 ? "account" : "domain";
         string direction = direction_.SelectedIndex == 1 ? "inbound" : direction_.SelectedIndex == 2 ? "outbound" : "both";
         string mode = mode_.SelectedIndex == 1 ? "copy" : "report";

         long added;
         try
         {
            dynamic utilities = ServerSession.Current.Application.Utilities;
            added = Convert.ToInt64(utilities.SaveJournalRule(0, scope, target, address, direction, mode, true, (note_.Text ?? "").Trim()));
         }
         catch (Exception why) when (!ExceptionPolicy.IsFatal(why))
         {
            // The server's refusal of what the form holds, under the form, at
            // the level of a refusal.
            notice_.Level = StatusLevel.Critical;
            notice_.Text = ServerSession.DescribeComError(why);
            notice_.Visibility = Visibility.Visible;
            return;
         }

         target_.Text = "";
         address_.Text = "";
         note_.Text = "";
         Reload_();
         Select_(added);
         Say_(StatusLevel.Good, JournalRuleText.Added(target));
      }

      private void Toggle_()
      {
         if (rules_.SelectedItem is not RuleRow row)
         {
            Warn_(L("Choose a rule in the list."));
            return;
         }

         // The switch is the one change worth making without retyping the rule:
         // a rule switched off stops copying and keeps everything it said, which
         // is what an administrator wants while a journal address is being moved.
         try
         {
            dynamic utilities = ServerSession.Current.Application.Utilities;
            utilities.SaveJournalRule(
               row.Id,
               row.Scope == L("Account") ? "account" : "domain",
               row.Target,
               row.JournalAddress,
               row.Direction == L("Arriving") ? "inbound" : row.Direction == L("Sent") ? "outbound" : "both",
               row.Mode == L("The message itself") ? "copy" : "report",
               !row.IsEnabled,
               row.Note);
         }
         catch (Exception why) when (!ExceptionPolicy.IsFatal(why))
         {
            Say_(StatusLevel.Critical, ServerSession.DescribeComError(why));
            return;
         }

         Reload_();
         Select_(row.Id);
         Say_(StatusLevel.Good, JournalRuleText.Switched(row.Target, !row.IsEnabled));
      }

      private void Remove_()
      {
         if (rules_.SelectedItem is not RuleRow row)
         {
            Warn_(L("Choose a rule in the list."));
            return;
         }

         if (!Dialogs.Confirm(F("Remove the journaling rule for {0}? What it has already sent stays where it was sent.", row.Target),
                              L("Control Panel"), L("Re_move"), destructive: true))
            return;

         int at = rules_.SelectedIndex;
         bool removed;
         try
         {
            dynamic utilities = ServerSession.Current.Application.Utilities;
            removed = Convert.ToBoolean(utilities.DeleteJournalRule(row.Id));
         }
         catch (Exception why) when (!ExceptionPolicy.IsFatal(why))
         {
            Say_(StatusLevel.Critical, ServerSession.DescribeComError(why));
            return;
         }

         Reload_();

         // The row that took its place is selected, or the last where it was the last.
         if (rules_.ItemsSource is List<RuleRow> rows)
         {
            int next = ListOutcome.IndexAfterDelete(rows.Count, at);
            if (next >= 0)
               Select_(rows[next].Id);
         }

         Say_(removed ? StatusLevel.Good : StatusLevel.Information, JournalRuleText.Removed(row.Target, removed));
      }

      private void Warn_(string message)
      {
         notice_.Level = StatusLevel.Warning;
         notice_.Text = message;
         notice_.Visibility = Visibility.Visible;
      }
   }
}
