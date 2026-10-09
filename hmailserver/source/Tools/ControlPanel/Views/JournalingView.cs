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
         public string Enabled { get; init; } = "";
         public bool IsEnabled { get; init; }
         public string Note { get; init; } = "";
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

      private readonly ComboBox scope_ = new() { MinWidth = 110, Margin = new Thickness(0, 0, 8, 0) };
      private readonly ComboBox direction_ = new() { MinWidth = 120, Margin = new Thickness(0, 0, 8, 0) };
      private readonly ComboBox mode_ = new() { MinWidth = 160, Margin = new Thickness(0, 0, 8, 0) };
      private readonly Wpf.Ui.Controls.TextBox target_ = new() { MinWidth = 200, Margin = new Thickness(0, 0, 8, 0) };
      private readonly Wpf.Ui.Controls.TextBox address_ = new() { MinWidth = 200, Margin = new Thickness(0, 0, 8, 0) };
      private readonly Wpf.Ui.Controls.TextBox note_ = new() { MinWidth = 140, Margin = new Thickness(0, 0, 8, 0) };

      private readonly InlineNotice notice_ = new() { Visibility = Visibility.Collapsed };

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
            Title = L("Journaling and dual delivery"),
            Subtitle = L("A copy of a domain's mail, or of one account's, sent to an address - as an envelope report with the message attached, or as the message itself. Nothing is copied until a rule exists here, and a rule is not the archive: it sends mail elsewhere rather than keeping files on this machine."),
            Actions = actions
         });

         var cards = new StackPanel { Margin = new Thickness(0, 0, 12, 0), MaxWidth = 1100, HorizontalAlignment = HorizontalAlignment.Left };

         rules_.Columns.Add(TextColumn_(L("Whose mail"), nameof(RuleRow.Target), 220));
         rules_.Columns.Add(TextColumn_(L("Domain or account"), nameof(RuleRow.Scope), 120));
         rules_.Columns.Add(TextColumn_(L("Which way"), nameof(RuleRow.Direction), 110));
         rules_.Columns.Add(TextColumn_(L("What is sent"), nameof(RuleRow.Mode), 170));
         rules_.Columns.Add(TextColumn_(L("Where it goes"), nameof(RuleRow.JournalAddress), 240));
         rules_.Columns.Add(TextColumn_(L("On"), nameof(RuleRow.Enabled), 60));
         rules_.Columns.Add(TextColumn_(L("Note"), nameof(RuleRow.Note), 180));

         // What acts on the rule chosen in the list sits with the list, and can be
         // pressed only while one is chosen; beside the form below it read as part
         // of adding a rule.
         var toggle = new Wpf.Ui.Controls.Button { Content = L("S_witch the rule over"), Margin = new Thickness(0, 0, 8, 0) };
         System.Windows.Automation.AutomationProperties.SetAutomationId(toggle, "journal-toggle");
         toggle.Click += (s, e) => Toggle_();

         var remove = new Wpf.Ui.Controls.Button { Content = L("Re_move the rule") };
         ButtonStyles.DangerText(remove);
         System.Windows.Automation.AutomationProperties.SetAutomationId(remove, "journal-remove");
         remove.Click += (s, e) => Remove_();

         chosen_.Children.Add(toggle);
         chosen_.Children.Add(remove);
         SelectionGate.Bind(rules_, toggle, remove);

         cards.Children.Add(readNotice_);

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
         // a Chinese word in issue 156.
         target_.PlaceholderText = L("A domain, or an account address");
         address_.PlaceholderText = L("Where the copy goes");
         note_.PlaceholderText = L("Why (optional)");
         System.Windows.Automation.AutomationProperties.SetName(scope_, L("Domain or account"));
         System.Windows.Automation.AutomationProperties.SetName(target_, L("Whose mail"));
         System.Windows.Automation.AutomationProperties.SetName(direction_, L("Which way"));
         System.Windows.Automation.AutomationProperties.SetName(mode_, L("What is sent"));
         System.Windows.Automation.AutomationProperties.SetName(address_, L("Where it goes"));
         System.Windows.Automation.AutomationProperties.SetName(note_, L("Note"));

         // Each box has its caption above it - the grid's own column names - as
         // the Domains page's forms do: a placeholder was the only name the boxes
         // had, gone once something is typed, and the three lists had none.
         var form = new WrapPanel();
         foreach (Control box in new Control[] { scope_, target_, direction_, mode_, address_, note_ })
            box.Margin = new Thickness(0);
         form.Children.Add(FormField_(L("Domain or account"), scope_));
         form.Children.Add(FormField_(L("Whose mail"), target_));
         form.Children.Add(FormField_(L("Which way"), direction_));
         form.Children.Add(FormField_(L("What is sent"), mode_));
         form.Children.Add(FormField_(L("Where it goes"), address_));
         form.Children.Add(FormField_(L("Note"), note_));

         add_ = new Wpf.Ui.Controls.Button
         {
            Content = L("_Add the rule"),
            Appearance = Wpf.Ui.Controls.ControlAppearance.Primary,
            Margin = new Thickness(0, 0, 8, 12),
            VerticalAlignment = VerticalAlignment.Bottom
         };
         System.Windows.Automation.AutomationProperties.SetAutomationId(add_, "journal-add");
         add_.Click += (s, e) => Add_();
         form.Children.Add(add_);

         var editing = new StackPanel();
         editing.Children.Add(form);
         editing.Children.Add(notice_);
         cards.Children.Add(new Card { Content = editing, Margin = new Thickness(0, 12, 0, 0) });

         var scroll = new ScrollViewer { Content = cards, VerticalScrollBarVisibility = ScrollBarVisibility.Auto };
         Grid.SetRow(scroll, 1);
         root.Children.Add(scroll);

         Content = root;
      }

      /// <summary>One box of the form with its caption above it.</summary>
      private static Scaffold.FieldRow FormField_(string caption, FrameworkElement editor)
         => new() { Label = caption, Content = editor, Margin = new Thickness(0, 0, 8, 12) };

      private static DataGridTextColumn TextColumn_(string header, string property, double width)
         => new DataGridTextColumn
         {
            Header = header,
            Binding = new System.Windows.Data.Binding(property),
            Width = new DataGridLength(width)
         };

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

      private void Reload_()
      {
         notice_.Visibility = Visibility.Collapsed;

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
                     Enabled = enabled ? L("Yes") : "",
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

         try
         {
            dynamic utilities = ServerSession.Current.Application.Utilities;
            utilities.SaveJournalRule(0, scope, target, address, direction, mode, true, (note_.Text ?? "").Trim());
         }
         catch (Exception why) when (!ExceptionPolicy.IsFatal(why))
         {
            Warn_(ServerSession.DescribeComError(why));
            return;
         }

         target_.Text = "";
         address_.Text = "";
         note_.Text = "";
         Reload_();
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
            Warn_(ServerSession.DescribeComError(why));
            return;
         }

         Reload_();
      }

      private void Remove_()
      {
         if (rules_.SelectedItem is not RuleRow row)
         {
            Warn_(L("Choose a rule in the list."));
            return;
         }

         if (!Dialogs.Confirm(F("Remove the journaling rule for {0}? What it has already sent stays where it was sent.", row.Target),
                              L("Control Panel"), L("_Remove"), destructive: true))
            return;

         try
         {
            dynamic utilities = ServerSession.Current.Application.Utilities;
            utilities.DeleteJournalRule(row.Id);
         }
         catch (Exception why) when (!ExceptionPolicy.IsFatal(why))
         {
            Warn_(ServerSession.DescribeComError(why));
            return;
         }

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
