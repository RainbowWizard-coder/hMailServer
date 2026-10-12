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

      // The table's two columns of sentences, floored in Build; the others are
      // floored at their words as each profile is shown (ShowDiff_).
      private DataGridTextColumn why_;
      private DataGridTextColumn note_;

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
      private readonly TextBlock summary_ = new() { TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 0, 0, 8) };

      // What the table's place says while there is no profile to compare: none to
      // choose, or none that could be read - where the card was a rectangle of
      // column headers with nothing under them (review finding F308).
      private readonly EmptyState empty_ = new() { Icon = Wpf.Ui.Controls.SymbolRegular.TableSettings24, Visibility = Visibility.Collapsed };

      // The profiles' own words are the server's, in English: in another language
      // the page's translated heading "What it cannot do for you" stood over the
      // server's English steps, summary and reasons with nothing to say why.
      private readonly TextBlock englishNote_ = new() { TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 8, 0, 0), Visibility = Visibility.Collapsed };

      // The profiles could not be read, so there is nothing to choose either.
      private bool listUnread_;
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

         // How many of its settings would change here, the caption beside the
         // title in the list (review finding F278): the item was a whole sentence,
         // "Small office - settings that would change on this server: 3 of 31.".
         // A label, which no language has to decline for either count.
         public string ChangeCaption => F("Would change here: {0} of {1}", Changes, Entries);

         // The title and the caption, as a screen reader hears the item and the
         // list's type-ahead finds it by its title.
         public override string ToString() => Title + ", " + ChangeCaption; // no-loc: a name and its caption
      }

      /// <summary>
      /// A profile's title, as the server writes it ("Small office"), for the key
      /// its record and its way back name it by ("small-office"): the notice, the
      /// questions and the outcome lines said the key (review finding F278), where
      /// the list and the setup checklist said the title. The key where the list
      /// has no such profile.
      /// </summary>
      private string TitleOf_(string name)
      {
         if (string.IsNullOrEmpty(name))
            return name;
         ProfileChoice choice = (profiles_.ItemsSource as List<ProfileChoice>)?.Find(c => c.Name == name);
         return string.IsNullOrEmpty(choice?.Title) ? name : choice.Title;
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
         return ServerTimeText.Date(DateTimeOffset.FromUnixTimeSeconds(seconds).ToLocalTime().DateTime);
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
         listUnread_ = false;

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
            // Nothing to act on that this read did not say: the notice of the last
            // read goes, with its acts.
            applied_.Visibility = Visibility.Collapsed;
            HideAppliedActs_();
            listUnread_ = true;
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
         // A failure is said at Warning, a fact at Information: a profile that could
         // not be applied, and a way back that cannot be read, were drawn at
         // Information, as if they were facts (the final check of Tier 2, g3 defect 8).
         StatusLevel level = StatusLevel.Information;
         bool hasRecord = root.TryGetProperty("applied", out JsonElement applied) && applied.ValueKind == JsonValueKind.Object;
         string state = hasRecord ? Text_(applied, "state") : "";
         if (snapshotHeld_ && !snapshotReadable_)
         {
            text = L("A way back from a profile is held and cannot be read, so it cannot be put back. Discard it to apply a profile; the audit trail names each value the profile replaced.");
            level = StatusLevel.Warning;
         }
         else if (!hasRecord)
            text = L("No profile has been applied. A profile sets about thirty settings for the kind of server this is - a small office, behind a filtering gateway, an outbound relay, a high-volume sender - and shows every change before it is made.");
         else
         {
            string name = TitleOf_(Text_(applied, "profile"));
            if (snapshotReadable_ && state != "failed")
               text = F("The {0} profile is applied. What it replaced is kept, and can be put back.", TitleOf_(snapshotProfile_));
            else if (state == "declined")
               text = F("The settings this version ships with were chosen on {0}, and no profile applied; any of the four can still be compared with this server.", Date_(applied, "changed"));
            else if (state == "failed")
            {
               // Each sentence names the acts the notice shows with it (review finding
               // F278): with the way back held, Put back what it changed and Discard
               // the previous settings - which keeps what was written - and with none,
               // since every write that would have changed a setting failed, Keep the
               // shipped settings. The one sentence they replace named an act this page
               // no longer has.
               text = snapshotHeld_
                  ? F("The {0} profile could not be applied in full on {1}: some of its settings were not written. Put back what it changed, or discard the previous settings to keep what it wrote, then apply it again.", name, Date_(applied, "applied"))
                  : F("The {0} profile could not be applied on {1}: none of the settings it would change could be written, so there is nothing to put back. Apply it again, or keep the shipped settings.", name, Date_(applied, "applied"));
               level = StatusLevel.Warning;
            }
            else if (state == "reverted")
               text = F("The {0} profile was applied and then reverted on {1}; the settings are the ones it replaced.", name, Date_(applied, "changed"));
            else if (state == "kept")
               text = F("The {0} profile was applied on {1}; its way back was discarded and its settings kept.", name, Date_(applied, "applied"));
            else
               text = F("The {0} profile was applied on {1}.", name, Date_(applied, "applied"));
         }

         applied_.Level = level;
         applied_.Text = text;
         applied_.Visibility = Visibility.Visible;

         // The acts on what is applied are the notice's own, each shown while it
         // can be taken (review finding F278): beside the profile list they were
         // five buttons that wrapped onto two rows, two of them "Keep" and
         // meaning different things, and greyed out more often than not. Putting
         // back and discarding go with a way back that is held; keeping the
         // shipped settings with none, while no profile is in effect - so the two
         // never show together.
         revert_.Visibility = snapshotHeld_ && snapshotReadable_ ? Visibility.Visible : Visibility.Collapsed;
         discard_.Visibility = snapshotHeld_ ? Visibility.Visible : Visibility.Collapsed;
         decline_.Visibility = !snapshotHeld_ && (!hasRecord || state == "reverted" || state == "failed") ? Visibility.Visible : Visibility.Collapsed;
      }

      /// <summary>The applied notice without its acts: a read that failed, before it says anything.</summary>
      private void HideAppliedActs_()
      {
         revert_.Visibility = Visibility.Collapsed;
         discard_.Visibility = Visibility.Collapsed;
         decline_.Visibility = Visibility.Collapsed;
      }

      private void ShowDiff_()
      {
         if (loading_)
            return;

         rows_.ItemsSource = null;
         summary_.Text = "";
         // Collapsed while it says nothing, so that the table's headers, or the empty
         // state under them, are not a blank line down the card.
         summary_.Visibility = Visibility.Collapsed;
         steps_.Text = "";
         confirm_.IsChecked = false;
         confirm_.Visibility = Visibility.Collapsed;
         confirmNote_.Visibility = Visibility.Collapsed;
         apply_.IsEnabled = false;
         changes_ = 0;
         needsConfirmation_ = false;
         diffToken_ = "";
         englishNote_.Visibility = Visibility.Collapsed;

         if (profiles_.SelectedItem is not ProfileChoice choice)
         {
            ShowNothingToCompare_(listUnread_);
            return;
         }

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
            summary_.Visibility = summary_.Text.Length > 0 ? Visibility.Visible : Visibility.Collapsed;

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
            GridStyles.FloorAtTheirWords(rows_, rows, why_, note_);

            var stepLines = new List<string>();
            if (root.TryGetProperty("steps", out JsonElement steps) && steps.ValueKind == JsonValueKind.Array)
            {
               foreach (JsonElement step in steps.EnumerateArray())
                  stepLines.Add("• " + (step.GetString() ?? "")); // no-loc
            }
            // The steps under their heading, and in a language other than English the
            // note that the profiles' own words are English at the end of the heading's
            // line, in the caption's size and colour: on a line of its own, above the
            // table or under the steps, it took one of the rows the table shows at 1280
            // by 800 in Japanese (the final check of Tier 2, g4 defect 11).
            steps_.Inlines.Clear();
            bool noteOnHeading = stepLines.Count > 0 && Loc.Translated;
            if (stepLines.Count > 0)
            {
               steps_.Inlines.Add(new System.Windows.Documents.Run(L("What it cannot do for you")));
               if (noteOnHeading)
               {
                  steps_.Inlines.Add(new System.Windows.Documents.Run("   ")); // no-loc: the gap before the note
                  var note = new System.Windows.Documents.Run(englishNote_.Text);
                  note.SetResourceReference(System.Windows.Documents.TextElement.FontSizeProperty, "AppFontSizeCaption");
                  note.SetResourceReference(System.Windows.Documents.TextElement.ForegroundProperty, "TextFillColorSecondaryBrush");
                  steps_.Inlines.Add(note);
               }

               steps_.Inlines.Add(new System.Windows.Documents.LineBreak());
               steps_.Inlines.Add(new System.Windows.Documents.Run(string.Join("\n", stepLines)));
            }

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

            empty_.Visibility = Visibility.Collapsed;
            englishNote_.Visibility = Loc.Translated && !noteOnHeading ? Visibility.Visible : Visibility.Collapsed;
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            rows_.ItemsSource = null;
            Status_(StatusLevel.Critical, ServerSession.DescribeComError(ex));
            ShowNothingToCompare_(true);
         }
      }

      /// <summary>
      /// The table's place while there is no profile to compare: none that could be
      /// read - the notice above says why - or none chosen.
      /// </summary>
      private void ShowNothingToCompare_(bool unread)
      {
         empty_.Text = unread
            ? L("Nothing to compare until the server can be read.")
            : L("Choose a profile to compare it with this server.");
         empty_.Visibility = Visibility.Visible;
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
                  choice.Title, changes_),
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
            Status_(StatusLevel.Good, F("{0} - set: {1}, unchanged: {2}, skipped: {3}; waiting for a reinitialise: {4}, for a restart: {5}.", choice.Title, Number_(root, "set"), Number_(root, "unchanged"), Number_(root, "skipped"), reinitialize, restart));
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

         string name = TitleOf_(snapshotProfile_);
         if (!MessageBox.Confirm(
               F("Put back the values the {0} profile replaced? A setting changed since is left as it is, and a filter the profile turned on may be turned off again.", name),
               L("Put back a profile's changes"), L("_Put back what it changed"), cancelByDefault: true))
         {
            return;
         }

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

         // Refresh in the header, as on the sibling pages (review finding F278),
         // where it was the last of six controls beside the profile list.
         root.Children.Add(new PageHeader
         {
            PageKey = "profiles",
            Subtitle = L("Each profile sets the settings one kind of server needs. Showing one lists every setting beside the value this server holds now; nothing changes until it is applied."),
            Actions = MakeButton(L("_Refresh"), Wpf.Ui.Controls.ControlAppearance.Secondary, (_, _) => Reload(null), last: true)
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

         // The acts on what is applied, in the applied notice's action slot: each
         // is shown while it can be taken (ShowApplied_).
         var appliedActs = new WrapPanel { Orientation = Orientation.Horizontal };
         revert_ = MakeButton(L("_Put back what it changed"), Wpf.Ui.Controls.ControlAppearance.Secondary, (_, _) => RevertApplied());
         appliedActs.Children.Add(revert_);
         discard_ = MakeButton(L("_Discard the previous settings"), Wpf.Ui.Controls.ControlAppearance.Secondary, (_, _) => DiscardWayBack(), last: true);
         appliedActs.Children.Add(discard_);
         decline_ = MakeButton(L("Keep the _shipped settings"), Wpf.Ui.Controls.ControlAppearance.Secondary, (_, _) => DeclineProfiles(), last: true);
         appliedActs.Children.Add(decline_);
         foreach (FrameworkElement act in appliedActs.Children)
            act.Margin = new Thickness(0, 2, act.Margin.Right, 2);
         applied_.Content = appliedActs;

         // The notice's action slot takes the width its content asks for, and the
         // text the rest: the acts wrap under each other where the notice is narrow,
         // so that its sentence keeps at least half of it - at 760 pixels the two
         // buttons side by side left the German sentence a word to a line.
         applied_.SizeChanged += (_, _) => appliedActs.MaxWidth = Math.Max(160, applied_.ActualWidth * 0.45);
         parts.Children.Add(applied_);

         // The list of profiles and the act on the one chosen.
         var actions = new WrapPanel { Orientation = Orientation.Horizontal, Margin = new Thickness(0, 0, 0, 8) };
         AutomationProperties.SetName(profiles_, L("Setting profiles"));
         profiles_.SelectionChanged += (_, _) => ShowDiff_();
         profiles_.ItemTemplate = ChoiceTemplate_();
         actions.Children.Add(profiles_);
         apply_ = MakeButton(L("_Apply this profile"), Wpf.Ui.Controls.ControlAppearance.Primary, (_, _) => ApplySelected(), last: true);
         actions.Children.Add(apply_);
         parts.Children.Add(actions);

         parts.Children.Add(status_);

         rows_.Columns.Add(Column_(L("Setting"), nameof(RowView.Setting), DataGridLength.Auto));
         rows_.Columns.Add(Column_(L("This server now"), nameof(RowView.Now), DataGridLength.Auto));
         rows_.Columns.Add(Column_(L("The profile"), nameof(RowView.Profile), DataGridLength.Auto));
         why_ = Column_(L("Why"), nameof(RowView.Why), new DataGridLength(2, DataGridLengthUnitType.Star));
         rows_.Columns.Add(why_);
         // Notes: the confirmation a change asks for, and what the environment
         // sets, a refusal or the profile's own note (review finding F278).
         note_ = Column_(L("Notes"), nameof(RowView.Note), new DataGridLength(1, DataGridLengthUnitType.Star));
         rows_.Columns.Add(note_);
         // No column narrower than its own header in the language in force, as the
         // routes grid floors its columns: the two star columns had no floor, and with
         // the card's own inset beside the wider sidebar German cut the last header to
         // "Verlangt eine Bestätig..." at 1280 (review finding F308). Why, the column
         // the table is for, keeps a reason's first words as well - floored at its short
         // header alone it was 61 pixels at 760, "A ne..." - so that where the floors do
         // not fit the grid scrolls sideways, keeping its upright bar as such a grid
         // does (docs/ControlPanelDesign.md, Grids); the three columns before them are
         // floored at their words as each profile is shown (ShowDiff_), since such a
         // grid narrowed after it was laid out wider takes what the two lack from them.
         foreach (DataGridColumn column in rows_.Columns)
            column.MinWidth = Math.Max(column.MinWidth, GridStyles.HeaderWidth(column.Header as string));
         why_.MinWidth = Math.Max(why_.MinWidth, 100);
         ScrollViewer.SetVerticalScrollBarVisibility(rows_, ScrollBarVisibility.Visible);
         AutomationProperties.SetName(rows_, L("Setting profiles"));

         // The card's own FillPanel: the table is its filling part, the summary
         // above it and the confirmation and steps under it keep their heights,
         // so the card asks for all three and the table takes the rest.
         var body = new FillPanel();
         body.Children.Add(summary_);
         // The table on a FillPanel of its own at its three rows, and the empty state
         // laid over it under its column headers, as a list card's is.
         var table = new FillPanel();
         table.Children.Add(FillPanel.Filling(rows_, 0));
         var tableHost = new Grid();
         tableHost.Children.Add(table);
         tableHost.Children.Add(empty_);
         EmptyStateUnderHeaders.Keep(empty_, rows_);
         body.Children.Add(FillPanel.Filling(tableHost, 0));
         var bottom = new StackPanel();
         bottom.Children.Add(confirm_);
         confirmNote_.SetResourceReference(FrameworkElement.StyleProperty, "TextCaption");
         bottom.Children.Add(confirmNote_);
         // The note is the box's help text as well, so that a screen reader reading
         // the box hears what confirming does, as it heard it when the caption said it.
         AutomationProperties.SetHelpText(confirm_, confirmNote_.Text);
         bottom.Children.Add(steps_);
         // That the profiles' words are English, under the table and the steps it
         // speaks of, as a caption, where a profile has no steps: with steps it ends
         // the steps' heading line instead (ShowDiff_), since on a line of its own it
         // took a row of the five the table shows at 1280 by 800 (the final check of
         // Tier 2, g4 defect 11).
         englishNote_.Text = L("The profiles' own words - their summaries, reasons, notes and steps - come from the server, in English.");
         englishNote_.SetResourceReference(FrameworkElement.StyleProperty, "TextCaption");
         bottom.Children.Add(englishNote_);
         body.Children.Add(bottom);

         // The Card's own inset, AppCardPadding, where it was eight pixels: it holds
         // a summary, a table and a confirmation, a card of content rather than a
         // bare list's (review finding F308).
         var card = new Card { Content = body };
         parts.Children.Add(FillPanel.Filling(card, 0));

         Content = root;
      }

      // Trimmed, with the whole reason in the cell's tool tip: "Why" is a
      // sentence and was cut mid-word.
      private static DataGridTextColumn Column_(string header, string path, DataGridLength width)
         => GridStyles.Trimmed(new DataGridTextColumn { Header = header, Binding = new System.Windows.Data.Binding(path), Width = width });

      /// <summary>A button of the page; the last of a row has no gap after it.</summary>
      private static Wpf.Ui.Controls.Button MakeButton(string text, Wpf.Ui.Controls.ControlAppearance appearance, RoutedEventHandler onClick, bool last = false)
      {
         var button = new Wpf.Ui.Controls.Button
         {
            Content = text,
            Appearance = appearance,
            Margin = new Thickness(0, 0, last ? 0 : 8, 0),
            MinWidth = 110
         };
         button.Click += onClick;
         return button;
      }

      /// <summary>
      /// A profile in the list: its title, then how many of its settings would
      /// change here in the caption colour (review finding F278), on one line, in
      /// the open list and in the closed box alike.
      /// </summary>
      private static DataTemplate ChoiceTemplate_()
      {
         var line = new FrameworkElementFactory(typeof(StackPanel));
         line.SetValue(StackPanel.OrientationProperty, Orientation.Horizontal);

         var title = new FrameworkElementFactory(typeof(TextBlock));
         title.SetBinding(TextBlock.TextProperty, new System.Windows.Data.Binding(nameof(ProfileChoice.Title)));
         title.SetValue(FrameworkElement.VerticalAlignmentProperty, VerticalAlignment.Center);
         line.AppendChild(title);

         var caption = new FrameworkElementFactory(typeof(TextBlock));
         caption.SetBinding(TextBlock.TextProperty, new System.Windows.Data.Binding(nameof(ProfileChoice.ChangeCaption)));
         caption.SetValue(FrameworkElement.MarginProperty, new Thickness(DesignTokens.Space.Sm, 0, 0, 0));
         caption.SetValue(FrameworkElement.VerticalAlignmentProperty, VerticalAlignment.Center);
         caption.SetValue(TextBlock.FontSizeProperty, Typography.Caption);
         caption.SetResourceReference(TextBlock.ForegroundProperty, "TextFillColorSecondaryBrush");
         line.AppendChild(caption);

         return new DataTemplate(typeof(ProfileChoice)) { VisualTree = line };
      }
   }
}
