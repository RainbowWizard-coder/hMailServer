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
   /// each profile's diff against this server, and apply, put back, discard and
   /// keeping the shipped settings - over Settings.GetProfiles, GetProfile,
   /// ApplyProfile, RevertProfile, DiscardProfileSnapshot and DeclineProfiles, the
   /// same documents the Deck reads over /api/v1/settings/profiles, made by the same
   /// code on the server.
   ///
   /// The page decides nothing. The server answers each entry with the value it
   /// holds now, whether applying would change it and whether that change closes a
   /// listener, trusts an address range, turns a filter or a check off, or shortens
   /// how long data is kept; the apply is sent with the confirmation only when the
   /// box under the diff is ticked, and always with the diff's token, so that the
   /// server refuses it when the settings moved since the diff was drawn. The
   /// profiles' titles, paragraphs, reasons and notes are the server's own English.
   /// </summary>
   public class ProfilesView : UserControl, IPageLifecycle
   {
      private readonly ComboBox profiles_ = new() { MinWidth = 280, Margin = new Thickness(0, 0, 8, 0) };

      // The floor: the column headers, three rows and a pixel for layout
      // rounding, 136, as the rules page's lists; held by the FillPanel in the
      // card, so that the card's floor is its summary, its three rows and the
      // steps under them, and rises with them.
      private readonly DataGrid rows_ = new()
      {
         AutoGenerateColumns = false,
         IsReadOnly = true,
         CanUserAddRows = false,
         CanUserDeleteRows = false,
         SelectionMode = DataGridSelectionMode.Single,
         MinHeight = 136
      };

      private readonly InlineNotice status_ = new() { Visibility = Visibility.Collapsed };
      private readonly InlineNotice applied_ = new() { Level = StatusLevel.Information, Visibility = Visibility.Collapsed, Margin = new Thickness(0, 0, 0, 8) };
      private readonly TextBlock summary_ = new() { TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 8, 0, 8) };
      private readonly TextBlock steps_ = new() { TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 8, 0, 0) };
      private readonly CheckBox confirm_ = new()
      {
         Content = L("I _confirm the changes marked above."),
         Margin = new Thickness(0, 8, 0, 0),
         Visibility = Visibility.Collapsed
      };
      // What the marked changes do, under the box rather than in its caption: a
      // caption of three lines is read by nobody, and a check box names itself to a
      // screen reader by its whole caption; Build tells the box the note as its help
      // text instead. Indented to the caption: WPF-UI's check box pads 11 before its
      // 20-pixel box and keeps 8 between the box and the caption.
      private readonly TextBlock confirmNote_ = new()
      {
         Text = L("They close a listener, trust an address range, turn a filter or a check off, or shorten how long data is kept. What a shorter window deletes, putting the settings back does not bring back."),
         TextWrapping = TextWrapping.Wrap,
         Margin = new Thickness(39, 0, 0, 0),
         Visibility = Visibility.Collapsed
      };

      private Wpf.Ui.Controls.Button apply_;
      private Wpf.Ui.Controls.Button revert_;
      private Wpf.Ui.Controls.Button discard_;
      private Wpf.Ui.Controls.Button decline_;

      private string snapshotProfile_ = "";
      // A way back is held, readable or not: one that cannot be read can only be
      // discarded, and discarding it is the only way to apply a profile again.
      private bool snapshotHeld_;
      private bool snapshotReadable_;
      private string diffToken_ = "";
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
         public override string ToString() => F("{0} - settings that would change on this server: {1} of {2}.", Title, Changes, Entries);
      }

      /// <summary>A row as the grid shows it: the entry beside the value this server holds.</summary>
      private sealed class RowView
      {
         public string Setting { get; init; }
         public string Now { get; init; }
         public string Profile { get; init; }
         public string Why { get; init; }
         public string Note { get; init; }

         // The setting, what the server has now and what the profile would make
         // it, with their columns' names, then why and the confirmation it asks for.
         public override string ToString() => RowSpeech.Of(Setting,
            RowSpeech.Labelled(L("This server now"), Now), RowSpeech.Labelled(L("The profile"), Profile), Why, Note);
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
         "turns_filter_off" => L("Turns a filter or a check off"),
         "purges" => L("Shortens how long data is kept"),
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
            snapshotHeld_ = false;
            snapshotReadable_ = false;
            if (root.TryGetProperty("snapshot", out JsonElement snapshot) && snapshot.ValueKind == JsonValueKind.Object)
            {
               snapshotHeld_ = true;
               snapshotProfile_ = Text_(snapshot, "profile");
               snapshotReadable_ = !(snapshot.TryGetProperty("readable", out JsonElement readable) && readable.ValueKind == JsonValueKind.False) && snapshotProfile_.Length > 0;
            }

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
         bool hasRecord = root.TryGetProperty("applied", out JsonElement applied) && applied.ValueKind == JsonValueKind.Object;
         string state = hasRecord ? Text_(applied, "state") : "";
         if (snapshotHeld_ && !snapshotReadable_)
            text = L("A way back from a profile is held and cannot be read, so it cannot be put back. Discard it to apply a profile; the audit trail names each value the profile replaced.");
         else if (!hasRecord)
            text = L("No profile has been applied. A profile sets about thirty settings for the kind of server this is - a small office, behind a filtering gateway, an outbound relay, a high-volume sender - and shows every change before it is made.");
         else
         {
            string name = Text_(applied, "profile");
            if (snapshotReadable_ && state != "failed")
               text = F("The {0} profile is applied. What it replaced is kept, and can be put back.", snapshotProfile_);
            else if (state == "declined")
               text = F("The settings this version ships with were chosen on {0}, and no profile applied; any of the four can still be compared with this server.", Date_(applied, "changed"));
            else if (state == "failed")
               text = F("The {0} profile could not be applied in full on {1}: some of its settings were not written. Put back what it wrote, or keep it, and apply it again.", name, Date_(applied, "applied"));
            else if (state == "reverted")
               text = F("The {0} profile was applied and then reverted on {1}; the settings are the ones it replaced.", name, Date_(applied, "changed"));
            else if (state == "kept")
               text = F("The {0} profile was applied on {1}; its way back was discarded and its settings kept.", name, Date_(applied, "applied"));
            else
               text = F("The {0} profile was applied on {1}.", name, Date_(applied, "applied"));
         }

         applied_.Text = text;
         applied_.Visibility = Visibility.Visible;

         revert_.IsEnabled = snapshotHeld_ && snapshotReadable_;
         discard_.IsEnabled = snapshotHeld_;
         // Keeping the shipped settings: while no profile is in effect and no way back
         // is held.
         decline_.IsEnabled = !snapshotHeld_ && (!hasRecord || state == "reverted" || state == "failed");
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
         confirmNote_.Visibility = Visibility.Collapsed;
         apply_.IsEnabled = false;
         changes_ = 0;
         needsConfirmation_ = false;
         diffToken_ = "";

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
               string note = Text_(entry, "note");
               if (note.Length > 0)
                  notes.Add(note);

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
            diffToken_ = Text_(root, "diff_token");
            needsConfirmation_ = root.TryGetProperty("confirmation_needed", out JsonElement needed) && needed.ValueKind == JsonValueKind.Array && needed.GetArrayLength() > 0;
            confirm_.Visibility = needsConfirmation_ ? Visibility.Visible : Visibility.Collapsed;
            confirmNote_.Visibility = confirm_.Visibility;

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
            Status_(StatusLevel.Warning, L("Tick the confirmation first: this profile closes a listener, trusts an address range, turns a filter or a check off, or shortens how long data is kept here."));
            return;
         }

         // Cancel on Enter, like every other confirmation that changes what the
         // server does.
         if (!MessageBox.Confirm(
               PluralF(changes_,
                  "Apply the {0} profile? {1} setting changes, and what it held is kept so that it can be put back.",
                  "Apply the {0} profile? {1} settings change, and what they held is kept so that they can be put back.",
                  choice.Name, changes_),
               L("Apply a profile"), L("_Apply this profile"), cancelByDefault: true))
         {
            return;
         }

         try
         {
            dynamic settings = ServerSession.Current.Application.Settings;
            string resultJson;
            try
            {
               // The token names the diff drawn below: the server refuses the apply
               // when the settings moved since, and the page draws the diff again.
               resultJson = (string)settings.ApplyProfile(choice.Name, confirmed, diffToken_);
            }
            finally
            {
               ServerSession.Release((object)settings);
            }

            using JsonDocument document = JsonDocument.Parse(resultJson);
            JsonElement root = document.RootElement;
            Reload(choice.Name);
            int reinitialize = 0, restart = 0;
            if (root.TryGetProperty("waiting", out JsonElement waiting) && waiting.ValueKind == JsonValueKind.Object)
            {
               reinitialize = Number_(waiting, "reinitialize");
               restart = Number_(waiting, "restart");
            }
            Status_(StatusLevel.Good, F("{0} - set: {1}, unchanged: {2}, skipped: {3}; waiting for a reinitialise: {4}, for a restart: {5}.", choice.Name, Number_(root, "set"), Number_(root, "unchanged"), Number_(root, "skipped"), reinitialize, restart));
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            // A refusal - the diff moved since it was drawn, among others - is said,
            // and the diff drawn again as the server answers it now.
            string message = ServerSession.DescribeComError(ex);
            Reload(choice.Name);
            Status_(StatusLevel.Critical, message);
         }
      }

      private void RevertApplied()
      {
         if (string.IsNullOrEmpty(snapshotProfile_))
            return;

         if (!MessageBox.Confirm(
               F("Put back the values the {0} profile replaced? A setting changed since is left as it is, and a filter the profile turned on may be turned off again.", snapshotProfile_),
               L("Put back a profile's changes"), L("_Put back what it changed"), cancelByDefault: true))
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
            int reinitialize = 0, restart = 0;
            if (root.TryGetProperty("waiting", out JsonElement waiting) && waiting.ValueKind == JsonValueKind.Object)
            {
               reinitialize = Number_(waiting, "reinitialize");
               restart = Number_(waiting, "restart");
            }
            Status_(StatusLevel.Good, F("{0} - put back: {1}, left as they were: {2}; waiting for a reinitialise: {3}, for a restart: {4}.", name, Number_(root, "set"), Number_(root, "skipped"), reinitialize, restart));
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

      private void DeclineProfiles()
      {
         if (!MessageBox.Confirm(
               L("Keep the settings this version ships with, and apply no profile? The checklist stops asking; any profile can still be applied later."),
               L("Keep the shipped settings"), L("Keep the _shipped settings"), cancelByDefault: true))
         {
            return;
         }

         try
         {
            dynamic settings = ServerSession.Current.Application.Settings;
            try
            {
               settings.DeclineProfiles();
            }
            finally
            {
               ServerSession.Release((object)settings);
            }

            Reload(null);
            Status_(StatusLevel.Good, L("The shipped settings are kept; no profile is applied."));
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
         root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
         root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });

         root.Children.Add(new PageHeader
         {
            Title = L("Setting profiles"),
            Subtitle = L("Each profile sets the settings one kind of server needs. Showing one lists every setting beside the value this server holds now; nothing changes until it is applied.")
         });

         // The IP ranges shape: the card takes the height the parts above it
         // leave in the window, down to its floor - its summary, three rows of
         // the table and the steps under them - and below that the page scrolls.
         // The card was a grid's star row under rows sized to their content,
         // which in the narrowest window (760 by 520) left no room for the table
         // or the "What it cannot do" list, and nothing scrolled.
         ScrollViewer scroll = FillPanel.ScrollingPage(out FillPanel parts);
         Grid.SetRow(scroll, 1);
         root.Children.Add(scroll);

         parts.Children.Add(applied_);

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
         decline_ = MakeButton(L("Keep the _shipped settings"), Wpf.Ui.Controls.ControlAppearance.Secondary, (_, _) => DeclineProfiles());
         actions.Children.Add(decline_);
         actions.Children.Add(MakeButton(L("_Refresh"), Wpf.Ui.Controls.ControlAppearance.Secondary, (_, _) => Reload(null)));
         parts.Children.Add(actions);

         parts.Children.Add(status_);

         rows_.Columns.Add(Column_(L("Setting"), nameof(RowView.Setting), DataGridLength.Auto));
         rows_.Columns.Add(Column_(L("This server now"), nameof(RowView.Now), DataGridLength.Auto));
         rows_.Columns.Add(Column_(L("The profile"), nameof(RowView.Profile), DataGridLength.Auto));
         rows_.Columns.Add(Column_(L("Why"), nameof(RowView.Why), new DataGridLength(2, DataGridLengthUnitType.Star)));
         rows_.Columns.Add(Column_(L("Asks for a confirmation"), nameof(RowView.Note), new DataGridLength(1, DataGridLengthUnitType.Star)));
         AutomationProperties.SetName(rows_, L("Setting profiles"));

         // The card's own FillPanel: the table is its filling part, the summary
         // above it and the confirmation and steps under it keep their heights,
         // so the card asks for all three and the table takes the rest.
         var body = new FillPanel();
         body.Children.Add(summary_);
         body.Children.Add(FillPanel.Filling(rows_, 0));
         var bottom = new StackPanel();
         bottom.Children.Add(confirm_);
         confirmNote_.SetResourceReference(FrameworkElement.StyleProperty, "TextCaption");
         bottom.Children.Add(confirmNote_);
         // The note is the box's help text as well, so that a screen reader reading
         // the box hears what confirming does, as it heard it when the caption said it.
         AutomationProperties.SetHelpText(confirm_, confirmNote_.Text);
         bottom.Children.Add(steps_);
         body.Children.Add(bottom);

         var card = new Card { Padding = new Thickness(8), Content = body };
         parts.Children.Add(FillPanel.Filling(card, 0));

         Content = root;
      }

      // Trimmed, with the whole reason in the cell's tool tip: "Why" is a
      // sentence and was cut mid-word.
      private static DataGridTextColumn Column_(string header, string path, DataGridLength width)
         => GridStyles.Trimmed(new DataGridTextColumn { Header = header, Binding = new System.Windows.Data.Binding(path), Width = width });

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
