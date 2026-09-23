// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Text.Json;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using hMailServer.ControlPanel.Services;
using hMailServer.ControlPanel.Views.Scaffold;
using MessageBox = hMailServer.ControlPanel.Views.Dialogs;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Views
{
   /// <summary>
   /// The setting profiles (hmailserver/docs/Profiles.md): the profile in effect,
   /// each profile's diff against this server, and apply, put back and discard -
   /// over Settings.GetProfiles, GetProfile, ApplyProfile, RevertProfile and
   /// DiscardProfileSnapshot, the same documents the Deck reads over
   /// /api/v1/settings/profiles, made by the same code on the server.
   ///
   /// The page decides nothing. The server answers each entry with the value it
   /// holds now, whether applying would change it and whether that change closes a
   /// listener, trusts an address range or turns a filter off; the apply is sent
   /// with the confirmation only when the box under the diff is ticked, and the
   /// server refuses it without one where one is needed. The profiles' titles,
   /// paragraphs and reasons are the server's own English.
   /// </summary>
   public class ProfilesView : UserControl, IPageLifecycle
   {
      private readonly ComboBox profiles_ = new() { MinWidth = 280, Margin = new Thickness(0, 0, 8, 0) };

      private readonly DataGrid rows_ = new()
      {
         AutoGenerateColumns = false,
         IsReadOnly = true,
         CanUserAddRows = false,
         CanUserDeleteRows = false,
         SelectionMode = DataGridSelectionMode.Single,
         MinHeight = 200
      };

      private readonly InlineNotice status_ = new() { Visibility = Visibility.Collapsed };
      private readonly InlineNotice applied_ = new() { Level = StatusLevel.Information, Visibility = Visibility.Collapsed, Margin = new Thickness(0, 0, 0, 8) };
      private readonly TextBlock summary_ = new() { TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 8, 0, 8) };
      private readonly TextBlock steps_ = new() { TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 8, 0, 0) };
      private readonly CheckBox confirm_ = new()
      {
         Content = L("I _confirm the changes marked above: they close a listener, trust an address range or turn a filter off on this server."),
         Margin = new Thickness(0, 8, 0, 0),
         Visibility = Visibility.Collapsed
      };

      private Wpf.Ui.Controls.Button apply_;
      private Wpf.Ui.Controls.Button revert_;
      private Wpf.Ui.Controls.Button discard_;

      private string snapshotProfile_ = "";
      private int changes_;
      private bool needsConfirmation_;
      private bool loading_;

      public ProfilesView() => Build();

      public void OnEnter() => Reload(null);

      public void OnLeave()
      {
      }

      private void Status_(StatusLevel level, string text)
      {
         status_.Level = level;
         status_.Text = text;
         status_.Visibility = string.IsNullOrEmpty(text) ? Visibility.Collapsed : Visibility.Visible;
      }

      private sealed class ProfileChoice
      {
         public string Name { get; init; }
         public string Title { get; init; }
         public string Summary { get; init; }
         public int Entries { get; init; }
         public int Changes { get; init; }

         // The server's title, and how many of its settings would change here.
         public override string ToString() => Title + " - " + F("{0} of its {1} settings would change on this server.", Changes, Entries);
      }

      /// <summary>A row as the grid shows it: the entry beside the value this server holds.</summary>
      private sealed class RowView
      {
         public string Setting { get; init; }
         public string Now { get; init; }
         public string Profile { get; init; }
         public string Why { get; init; }
         public string Note { get; init; }
      }

      private static string Text_(JsonElement element, string name)
         => element.TryGetProperty(name, out JsonElement value) && value.ValueKind == JsonValueKind.String ? value.GetString() ?? "" : "";

      private static int Number_(JsonElement element, string name)
         => element.TryGetProperty(name, out JsonElement value) && value.ValueKind == JsonValueKind.Number && value.TryGetInt32(out int number) ? number : 0;

      private static string Date_(JsonElement element, string name)
      {
         if (!element.TryGetProperty(name, out JsonElement value) || value.ValueKind != JsonValueKind.Number || !value.TryGetInt64(out long seconds) || seconds <= 0)
            return "";
         return DateTimeOffset.FromUnixTimeSeconds(seconds).ToLocalTime().ToString("yyyy-MM-dd");
      }

      private static string CautionWord_(string caution) => caution switch
      {
         "closes_listener" => L("Closes a listener"),
         "trusts_addresses" => L("Trusts an address range"),
         "turns_filter_off" => L("Turns a filter off"),
         _ => caution
      };

      /// <summary>The list, the record of what is applied, and the diff of the profile chosen.</summary>
      private void Reload(string select)
      {
         Status_(StatusLevel.Normal, "");
         loading_ = true;

         try
         {
            dynamic settings = ServerSession.Current.Application.Settings;
            string listJson;
            try
            {
               listJson = (string)settings.GetProfiles();
            }
            finally
            {
               ServerSession.Release((object)settings);
            }

            using JsonDocument document = JsonDocument.Parse(listJson);
            JsonElement root = document.RootElement;

            string previous = select ?? (profiles_.SelectedItem as ProfileChoice)?.Name;
            var choices = new List<ProfileChoice>();
            foreach (JsonElement profile in root.GetProperty("profiles").EnumerateArray())
            {
               choices.Add(new ProfileChoice
               {
                  Name = Text_(profile, "name"),
                  Title = Text_(profile, "title"),
                  Summary = Text_(profile, "summary"),
                  Entries = Number_(profile, "entries"),
                  Changes = Number_(profile, "changes")
               });
            }

            profiles_.ItemsSource = choices;
            profiles_.SelectedItem = choices.Find(c => c.Name == previous) ?? (choices.Count > 0 ? choices[0] : null);

            snapshotProfile_ = "";
            if (root.TryGetProperty("snapshot", out JsonElement snapshot) && snapshot.ValueKind == JsonValueKind.Object)
               snapshotProfile_ = Text_(snapshot, "profile");

            ShowApplied_(root);
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            Status_(StatusLevel.Critical, ServerSession.DescribeComError(ex));
         }
         finally
         {
            loading_ = false;
         }

         ShowDiff_();
      }

      /// <summary>Which profile is in effect, in this program's words for the server's record.</summary>
      private void ShowApplied_(JsonElement root)
      {
         string text;
         if (!root.TryGetProperty("applied", out JsonElement applied) || applied.ValueKind != JsonValueKind.Object)
            text = L("No profile has been applied. A profile sets about thirty settings for the kind of server this is - a small office, behind a filtering gateway, an outbound relay, a high-volume sender - and shows every change before it is made.");
         else
         {
            string name = Text_(applied, "profile");
            string state = Text_(applied, "state");
            if (!string.IsNullOrEmpty(snapshotProfile_))
               text = F("The {0} profile is applied. What it replaced is kept, and can be put back.", snapshotProfile_);
            else if (state == "reverted")
               text = F("The {0} profile was applied and then reverted on {1}; the settings are the ones it replaced.", name, Date_(applied, "changed"));
            else if (state == "kept")
               text = F("The {0} profile was applied on {1}; its way back was discarded and its settings kept.", name, Date_(applied, "applied"));
            else
               text = F("The {0} profile was applied on {1}.", name, Date_(applied, "applied"));
         }

         applied_.Text = text;
         applied_.Visibility = Visibility.Visible;

         bool held = !string.IsNullOrEmpty(snapshotProfile_);
         revert_.IsEnabled = held;
         discard_.IsEnabled = held;
      }

      private void ShowDiff_()
      {
         if (loading_)
            return;

         rows_.ItemsSource = null;
         summary_.Text = "";
         steps_.Text = "";
         confirm_.IsChecked = false;
         confirm_.Visibility = Visibility.Collapsed;
         apply_.IsEnabled = false;
         changes_ = 0;
         needsConfirmation_ = false;

         if (profiles_.SelectedItem is not ProfileChoice choice)
            return;

         try
         {
            dynamic settings = ServerSession.Current.Application.Settings;
            string diffJson;
            try
            {
               diffJson = (string)settings.GetProfile(choice.Name);
            }
            finally
            {
               ServerSession.Release((object)settings);
            }

            using JsonDocument document = JsonDocument.Parse(diffJson);
            JsonElement root = document.RootElement;

            summary_.Text = Text_(root, "summary");

            var rows = new List<RowView>();
            foreach (JsonElement entry in root.GetProperty("entries").EnumerateArray())
            {
               var notes = new List<string>();
               bool changes = entry.TryGetProperty("changes", out JsonElement c) && c.ValueKind == JsonValueKind.True;
               bool confirm = entry.TryGetProperty("confirm", out JsonElement f) && f.ValueKind == JsonValueKind.True;
               if (confirm)
                  notes.Add(CautionWord_(Text_(entry, "caution")));
               string environment = Text_(entry, "environment");
               if (environment.Length > 0)
                  notes.Add(L("Set by the environment") + " (" + environment + ")");
               string refused = Text_(entry, "refused");
               if (refused.Length > 0)
                  notes.Add(F("Refused: {0}", refused));

               rows.Add(new RowView
               {
                  Setting = Text_(entry, "name"),
                  Now = Text_(entry, "current"),
                  Profile = changes ? "→ " + Text_(entry, "value") : Text_(entry, "value"), // no-loc
                  Why = Text_(entry, "why"),
                  Note = string.Join("; ", notes)
               });
            }
            rows_.ItemsSource = rows;

            var stepLines = new List<string>();
            if (root.TryGetProperty("steps", out JsonElement steps) && steps.ValueKind == JsonValueKind.Array)
            {
               foreach (JsonElement step in steps.EnumerateArray())
                  stepLines.Add("• " + (step.GetString() ?? "")); // no-loc
            }
            steps_.Text = stepLines.Count > 0 ? L("What it cannot do for you") + "\n" + string.Join("\n", stepLines) : "";

            changes_ = Number_(root, "changes");
            needsConfirmation_ = root.TryGetProperty("confirmation_needed", out JsonElement needed) && needed.ValueKind == JsonValueKind.Array && needed.GetArrayLength() > 0;
            confirm_.Visibility = needsConfirmation_ ? Visibility.Visible : Visibility.Collapsed;

            bool applicable = root.TryGetProperty("applicable", out JsonElement a) && a.ValueKind == JsonValueKind.True;
            apply_.IsEnabled = applicable;

            string blocked = Text_(root, "blocked");
            if (blocked.Length > 0)
               Status_(StatusLevel.Information, blocked);
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            Status_(StatusLevel.Critical, ServerSession.DescribeComError(ex));
         }
      }

      private void ApplySelected()
      {
         if (profiles_.SelectedItem is not ProfileChoice choice)
            return;

         bool confirmed = confirm_.IsChecked == true;
         if (needsConfirmation_ && !confirmed)
         {
            Status_(StatusLevel.Warning, L("Tick the confirmation first: this profile closes a listener, trusts an address range or turns a filter off here."));
            return;
         }

         // YesNo, No on Enter, like every other confirmation that changes what the
         // server does.
         if (MessageBox.Show(
               F("Apply the {0} profile? {1} settings change now, and what they held is kept so that they can be put back.", choice.Name, changes_),
               L("Apply a profile"), MessageBoxButton.YesNo, MessageBoxImage.Question, MessageBoxResult.No)
             != MessageBoxResult.Yes)
         {
            return;
         }

         try
         {
            dynamic settings = ServerSession.Current.Application.Settings;
            string resultJson;
            try
            {
               resultJson = (string)settings.ApplyProfile(choice.Name, confirmed);
            }
            finally
            {
               ServerSession.Release((object)settings);
            }

            using JsonDocument document = JsonDocument.Parse(resultJson);
            JsonElement root = document.RootElement;
            Reload(choice.Name);
            Status_(StatusLevel.Good, F("{0}: {1} set, {2} unchanged, {3} skipped.", choice.Name, Number_(root, "set"), Number_(root, "unchanged"), Number_(root, "skipped")));
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            Status_(StatusLevel.Critical, ServerSession.DescribeComError(ex));
         }
      }

      private void RevertApplied()
      {
         if (string.IsNullOrEmpty(snapshotProfile_))
            return;

         if (MessageBox.Show(
               F("Put back the values the {0} profile replaced? A setting changed since is left as it is, and a filter the profile turned on may be turned off again.", snapshotProfile_),
               L("Put back a profile's changes"), MessageBoxButton.YesNo, MessageBoxImage.Question, MessageBoxResult.No)
             != MessageBoxResult.Yes)
         {
            return;
         }

         string name = snapshotProfile_;
         try
         {
            dynamic settings = ServerSession.Current.Application.Settings;
            string resultJson;
            try
            {
               resultJson = (string)settings.RevertProfile();
            }
            finally
            {
               ServerSession.Release((object)settings);
            }

            using JsonDocument document = JsonDocument.Parse(resultJson);
            JsonElement root = document.RootElement;
            Reload(null);
            Status_(StatusLevel.Good, F("{0}: {1} put back, {2} left as they were.", name, Number_(root, "set"), Number_(root, "skipped")));
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            Status_(StatusLevel.Critical, ServerSession.DescribeComError(ex));
         }
      }

      private void DiscardWayBack()
      {
         try
         {
            dynamic settings = ServerSession.Current.Application.Settings;
            try
            {
               settings.DiscardProfileSnapshot();
            }
            finally
            {
               ServerSession.Release((object)settings);
            }

            Reload(null);
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            Status_(StatusLevel.Critical, ServerSession.DescribeComError(ex));
         }
      }

      private void Build()
      {
         var root = new Grid();
         root.SetResourceReference(MarginProperty, "AppPagePadding");
         for (int i = 0; i < 4; i++)
            root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
         root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });

         root.Children.Add(new PageHeader
         {
            Title = L("Setting profiles"),
            Subtitle = L("Each profile sets the settings one kind of server needs. Showing one lists every setting beside the value this server holds now; nothing changes until it is applied.")
         });

         Grid.SetRow(applied_, 1);
         root.Children.Add(applied_);

         var actions = new WrapPanel { Orientation = Orientation.Horizontal, Margin = new Thickness(0, 0, 0, 8) };
         AutomationProperties.SetName(profiles_, L("Setting profiles"));
         profiles_.SelectionChanged += (_, _) => ShowDiff_();
         actions.Children.Add(profiles_);
         apply_ = MakeButton(L("_Apply this profile"), Wpf.Ui.Controls.ControlAppearance.Primary, (_, _) => ApplySelected());
         actions.Children.Add(apply_);
         revert_ = MakeButton(L("_Put back what it changed"), Wpf.Ui.Controls.ControlAppearance.Secondary, (_, _) => RevertApplied());
         actions.Children.Add(revert_);
         discard_ = MakeButton(L("_Keep its settings, forget the way back"), Wpf.Ui.Controls.ControlAppearance.Secondary, (_, _) => DiscardWayBack());
         actions.Children.Add(discard_);
         actions.Children.Add(MakeButton(L("Re_fresh"), Wpf.Ui.Controls.ControlAppearance.Secondary, (_, _) => Reload(null)));
         Grid.SetRow(actions, 2);
         root.Children.Add(actions);

         Grid.SetRow(status_, 3);
         root.Children.Add(status_);

         rows_.Columns.Add(Column_(L("Setting"), nameof(RowView.Setting), DataGridLength.Auto));
         rows_.Columns.Add(Column_(L("This server now"), nameof(RowView.Now), DataGridLength.Auto));
         rows_.Columns.Add(Column_(L("The profile"), nameof(RowView.Profile), DataGridLength.Auto));
         rows_.Columns.Add(Column_(L("Why"), nameof(RowView.Why), new DataGridLength(2, DataGridLengthUnitType.Star)));
         rows_.Columns.Add(Column_(L("Asks for a confirmation"), nameof(RowView.Note), new DataGridLength(1, DataGridLengthUnitType.Star)));
         AutomationProperties.SetName(rows_, L("Setting profiles"));

         var body = new DockPanel();
         DockPanel.SetDock(summary_, Dock.Top);
         body.Children.Add(summary_);
         var bottom = new StackPanel();
         bottom.Children.Add(confirm_);
         bottom.Children.Add(steps_);
         DockPanel.SetDock(bottom, Dock.Bottom);
         body.Children.Add(bottom);
         body.Children.Add(rows_);

         var card = new Card { Padding = new Thickness(8), Content = body };
         Grid.SetRow(card, 4);
         root.Children.Add(card);

         Content = root;
      }

      private static DataGridTextColumn Column_(string header, string path, DataGridLength width)
         => new() { Header = header, Binding = new System.Windows.Data.Binding(path), Width = width };

      private static Wpf.Ui.Controls.Button MakeButton(string text, Wpf.Ui.Controls.ControlAppearance appearance, RoutedEventHandler onClick)
      {
         var button = new Wpf.Ui.Controls.Button
         {
            Content = text,
            Appearance = appearance,
            Margin = new Thickness(0, 0, 8, 0),
            MinWidth = 110
         };
         button.Click += onClick;
         return button;
      }
   }
}
