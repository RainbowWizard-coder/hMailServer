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
   /// The domain white and black list: sending organisations this server has
   /// decided about by their domain, for the operator whose correspondents will
   /// never sit still on one address.
   ///
   /// A white domain skips greylisting and the refusals the spam score would
   /// make, while every test still runs and logs; a black domain is refused at
   /// MAIL FROM before any of that. Keyed by the envelope sender's domain, the
   /// HELO name, or both. It is not the anti-spam white list (an address range
   /// that skips every test) and not the allow and block list (which decides per
   /// recipient, at delivery); the page says so, because the three are easy to
   /// confuse and are not the same thing.
   ///
   /// The list comes from AntiSpam.DomainPolicyList as JSON and the two writes are
   /// AddDomainPolicyListEntry and RemoveDomainPolicyListEntry - members of an
   /// interface that already exists, so this page needed no new COM interface and
   /// no registration step. Reached through dynamic for the same reason the
   /// suppression list is.
   /// </summary>
   public class DomainPolicyListView : UserControl, IPageLifecycle
   {
      private readonly Toolbar toolbar_ = new() { SearchPlaceholder = L("Domain contains") };

      private readonly DataGrid entries_ = new()
      {
         AutoGenerateColumns = false,
         IsReadOnly = true,
         CanUserAddRows = false,
         CanUserDeleteRows = false,
         SelectionMode = DataGridSelectionMode.Single,
         MinHeight = 160
      };

      private readonly InlineNotice status_ = new() { Visibility = Visibility.Collapsed };
      private readonly EmptyState empty_ = new() { Icon = Wpf.Ui.Controls.SymbolRegular.Globe24, Visibility = Visibility.Collapsed };

      private List<EntryView> all_ = new();

      // Add waits for a read that works (Services/ReadFailure).
      private Button addButton_;

      public DomainPolicyListView() => Build();

      // Read on entry rather than in the constructor, because the page cache keeps
      // this instance for the session: a row added over REST since the page was
      // first opened must be shown when somebody comes back to it.
      public void OnEnter() => Reload();

      public void OnLeave()
      {
      }

      private void Status_(StatusLevel level, string text)
      {
         ReadFailure.Clear(status_);
         status_.Level = level;
         status_.Text = text;
         status_.Visibility = Visibility.Visible;
      }

      /// <summary>An entry as the grid shows it: the server's words turned into the reader's.</summary>
      private sealed class EntryView
      {
         public int Id { get; init; }
         public string Pattern { get; init; }
         public string MatchOn { get; init; }
         public string MatchOnWord { get; init; }
         public string Action { get; init; }
         public string ActionWord { get; init; }
         public string Note { get; init; }
         public string Created { get; init; }
      }

      private static string MatchOnText_(string word) => word switch
      {
         "envelope" => L("Envelope sender's domain"),
         "helo" => L("HELO name"),
         _ => L("Either")
      };

      private static string ActionText_(string word) => word == "black" ? L("Black: refused at MAIL FROM") : L("White: not greylisted, not refused on score");

      private static string Text_(JsonElement element, string name)
      {
         JsonElement value;
         if (element.ValueKind != JsonValueKind.Object || !element.TryGetProperty(name, out value))
            return "";
         return value.ValueKind == JsonValueKind.String ? value.GetString() ?? "" : value.ToString();
      }

      private static int Int_(JsonElement element, string name)
      {
         JsonElement value;
         if (element.ValueKind != JsonValueKind.Object || !element.TryGetProperty(name, out value))
            return 0;
         return value.ValueKind == JsonValueKind.Number && value.TryGetInt32(out int number) ? number : 0;
      }

      private static List<EntryView> Parse_(string json)
      {
         var views = new List<EntryView>();

         using JsonDocument document = JsonDocument.Parse(json);
         JsonElement items;

         if (document.RootElement.ValueKind != JsonValueKind.Object || !document.RootElement.TryGetProperty("items", out items) || items.ValueKind != JsonValueKind.Array)
            return views;

         foreach (JsonElement item in items.EnumerateArray())
         {
            string matchOn = Text_(item, "match_on");
            string action = Text_(item, "action");

            views.Add(new EntryView
            {
               Id = Int_(item, "id"),
               Pattern = Text_(item, "pattern"),
               MatchOn = MatchOnText_(matchOn),
               MatchOnWord = matchOn,
               Action = ActionText_(action),
               ActionWord = action,
               Note = Text_(item, "note"),
               Created = Text_(item, "created")
            });
         }

         return views;
      }

      private void Reload()
      {
         ReadFailure.Clear(status_, addButton_);
         status_.Visibility = Visibility.Collapsed;

         try
         {
            dynamic antiSpam = ServerSession.Current.Application.Settings.AntiSpam;

            try
            {
               all_ = Parse_((string)antiSpam.DomainPolicyList);
            }
            finally
            {
               ServerSession.Release((object)antiSpam);
            }

            Present_();
         }
         catch (JsonException ex)
         {
            Unread_(null, F("The server's answer could not be read: {0}", ex.Message));
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            Unread_(L("Could not read the list: "), ServerSession.DescribeComError(ex));
         }
      }

      /// <summary>
      /// A read that failed: neither the rows of the last read nor "The list is
      /// empty" is a fact this page has now, so both go, the notice says what
      /// happened, with Try again, and Add waits for a read that works
      /// (Services/ReadFailure).
      /// </summary>
      private void Unread_(string prefix, string why)
      {
         all_ = new List<EntryView>();
         ReadFailure.Show(status_, empty_, entries_, prefix, why, Reload, addButton_);
      }

      // A filter over the rows already here: the whole list is a handful of
      // domains, read in one call, so there is nothing to ask the server for.
      private void Present_()
      {
         // A search over a list that could not be read runs the read again rather
         // than the filter: filtering nothing showed "The list is empty, and an
         // empty list changes nothing" under the notice that says the list could
         // not be read, which is the one claim that notice is there to withhold
         // (Services/ReadFailure). A read that works comes back here with the
         // failure cleared.
         if (ReadFailure.IsShown(status_))
         {
            Reload();
            return;
         }

         string search = toolbar_.SearchText?.Trim() ?? "";
         var shown = new List<EntryView>();

         foreach (EntryView entry in all_)
         {
            if (search.Length == 0 ||
                entry.Pattern.IndexOf(search, StringComparison.OrdinalIgnoreCase) >= 0 ||
                entry.Note.IndexOf(search, StringComparison.OrdinalIgnoreCase) >= 0)
               shown.Add(entry);
         }

         entries_.ItemsSource = shown;

         StatusText.Show(empty_, null, all_.Count, null,
            L("The list is empty, and an empty list changes nothing: every domain is greylisted and scored as before."));

         if (all_.Count > 0 && shown.Count < all_.Count)
            Status_(StatusLevel.Information, F("{0} of {1} entries match the search.", shown.Count, all_.Count));
      }

      private void AddEntry()
      {
         string added = AddDomainPolicyDialog.Show(Window.GetWindow(this));

         if (added != null)
         {
            Reload();
            Status_(StatusLevel.Good, F("{0} is on the list. It applies to the next session from that domain.", added));
         }
      }

      private void RemoveSelected()
      {
         if (entries_.SelectedItem is not EntryView selected)
         {
            Status_(StatusLevel.Information, L("Select an entry first."));
            return;
         }

         // YesNo, No on Enter, like every other confirmation that changes what the
         // server does: a black domain removed is accepted again at once.
         if (MessageBox.Show(
               F("Remove {0} from the list?\n\nA black domain is accepted again at once; a white domain goes back under greylisting and the spam score from the next session.", selected.Pattern),
               L("Remove from the domain list"), MessageBoxButton.YesNo, MessageBoxImage.Question,
               MessageBoxResult.No)
             != MessageBoxResult.Yes)
         {
            return;
         }

         try
         {
            bool removed;
            dynamic antiSpam = ServerSession.Current.Application.Settings.AntiSpam;
            try
            {
               removed = (bool)antiSpam.RemoveDomainPolicyListEntry(selected.Id);
            }
            finally
            {
               ServerSession.Release((object)antiSpam);
            }

            Reload();
            Status_(removed ? StatusLevel.Good : StatusLevel.Information,
               removed ? F("{0} was removed from the list.", selected.Pattern) : F("{0} was not on the list any more.", selected.Pattern));
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
         root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
         root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
         root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
         root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });

         root.Children.Add(new PageHeader
         {
            Title = L("Domain white and black list"),
            Subtitle = L("Sending organisations decided about by their domain rather than by the IP address of the week. White: not greylisted and never refused on the spam score, while every test still runs and logs. Black: refused at MAIL FROM before greylisting, scoring or any lookup. Matched on the envelope sender's domain, the HELO name, or either; inbound unauthenticated sessions only.")
         });

         var explanation = new InlineNotice
         {
            Level = StatusLevel.Information,
            Text = L("Not the anti-spam white list, which is an IP range that skips every test, and not the allow and block list, which decides per recipient at delivery. example.com matches that domain alone; .example.com matches the domain and everything beneath it.")
         };

         toolbar_.PreviewKeyDown += (_, e) => { if (e.Key == System.Windows.Input.Key.Enter) Present_(); };

         var actions = new StackPanel { Orientation = Orientation.Horizontal };
         addButton_ = MakeButton(L("_Add…"), Wpf.Ui.Controls.ControlAppearance.Primary, (_, _) => AddEntry());
         actions.Children.Add(addButton_);
         var remove = MakeButton(L("_Remove"), Wpf.Ui.Controls.ControlAppearance.Secondary, (_, _) => RemoveSelected());
         ButtonStyles.DangerText(remove);
         actions.Children.Add(remove);
         SelectionGate.Bind(entries_, remove);
         actions.Children.Add(MakeButton(L("Re_fresh"), Wpf.Ui.Controls.ControlAppearance.Secondary, (_, _) => Reload()));
         toolbar_.Actions = actions;
         Grid.SetRow(toolbar_, 1);
         root.Children.Add(toolbar_);

         Grid.SetRow(explanation, 2);
         root.Children.Add(explanation);

         Grid.SetRow(status_, 3);
         root.Children.Add(status_);

         BuildColumns_();
         AutomationProperties.SetName(entries_, L("Domain white and black list"));

         var host = new Grid();
         host.Children.Add(entries_);
         host.Children.Add(empty_);
         var listCard = new Card { Padding = new Thickness(8), Content = host };
         Grid.SetRow(listCard, 4);
         root.Children.Add(listCard);

         Content = root;
      }

      private void BuildColumns_()
      {
         entries_.Columns.Add(Text_(L("Domain or suffix"), nameof(EntryView.Pattern), new DataGridLength(2, DataGridLengthUnitType.Star)));
         entries_.Columns.Add(Text_(L("Matched on"), nameof(EntryView.MatchOn), DataGridLength.Auto));
         entries_.Columns.Add(Text_(L("Action"), nameof(EntryView.Action), DataGridLength.Auto));
         entries_.Columns.Add(Text_(L("Note"), nameof(EntryView.Note), new DataGridLength(2, DataGridLengthUnitType.Star)));
         entries_.Columns.Add(Text_(L("Added"), nameof(EntryView.Created), DataGridLength.Auto));
      }

      private static DataGridTextColumn Text_(string header, string path, DataGridLength width)
         => new() { Header = header, Binding = new System.Windows.Data.Binding(path), Width = width };

      private static Button MakeButton(string text, Wpf.Ui.Controls.ControlAppearance appearance, RoutedEventHandler onClick)
      {
         var button = new Wpf.Ui.Controls.Button
         {
            Content = text,
            Appearance = appearance,
            Margin = new Thickness(8, 0, 0, 0),
            MinWidth = 110
         };
         button.Click += onClick;
         return button;
      }
   }

   /// <summary>
   /// Puts one domain on the list: the pattern, what it is matched on, white or
   /// black, and a note for whoever reads the list next. Its own class so that its
   /// Alt keys are its own scope and not the page's. The server's refusal - a
   /// pattern that is not a domain, a pair already listed - is shown under the
   /// pattern, in its words.
   /// </summary>
   internal static class AddDomainPolicyDialog
   {
      /// <summary>The pattern as added, or null when nothing was.</summary>
      public static string Show(Window owner)
      {
         var dialog = new FramedDialog { Owner = owner, Title = L("Add a domain to the list") };

         var pattern = new Wpf.Ui.Controls.TextBox();
         AutomationProperties.SetName(pattern, L("Domain or suffix"));

         var matchOn = new ComboBox();
         matchOn.Items.Add(DialogFields.Combo(L("Either the envelope sender's domain or the HELO name"), 2));
         matchOn.Items.Add(DialogFields.Combo(L("The envelope sender's domain"), 0));
         matchOn.Items.Add(DialogFields.Combo(L("The HELO name"), 1));
         DialogFields.SelectCombo(matchOn, 2, orFirst: true);
         AutomationProperties.SetName(matchOn, L("Matched on"));

         var action = new ComboBox();
         action.Items.Add(DialogFields.Combo(L("White: not greylisted, not refused on the spam score"), 0));
         action.Items.Add(DialogFields.Combo(L("Black: refused at MAIL FROM"), 1));
         DialogFields.SelectCombo(action, 0, orFirst: true);
         AutomationProperties.SetName(action, L("Action"));

         var note = new Wpf.Ui.Controls.TextBox();
         AutomationProperties.SetName(note, L("Why it is listed"));

         var body = new StackPanel();
         var patternRow = DialogFields.Field(L("_Domain or suffix"), pattern, L("example.com is that domain alone; .example.com is the domain and everything beneath it."));
         body.Children.Add(patternRow);
         body.Children.Add(DialogFields.Field(L("_Matched on"), matchOn, L("A domain with rotating outbound hosts is usually recognised by the HELO name; a domain that forges nothing by its envelope sender.")));
         body.Children.Add(DialogFields.Field(L("A_ction"), action, L("White still runs every test and still marks at the mark threshold; black refuses before a single test runs.")));
         body.Children.Add(DialogFields.Field(L("_Note"), note, L("Kept with the entry, for whoever reads the list next.")));

         string result = null;

         var ok = new Wpf.Ui.Controls.Button { Content = L("_Add"), Appearance = Wpf.Ui.Controls.ControlAppearance.Primary, MinWidth = 88 };
         ok.Click += (_, _) =>
         {
            DialogFields.ClearErrors(patternRow);

            string typed = pattern.Text?.Trim() ?? "";
            if (typed.Length == 0)
            {
               DialogFields.ShowError(patternRow, L("A domain is required."));
               return;
            }

            string matchWord = DialogFields.ComboValue(matchOn, 2) switch { 0 => "envelope", 1 => "helo", _ => "both" };
            string actionWord = DialogFields.ComboValue(action, 0) == 1 ? "black" : "white";

            try
            {
               dynamic antiSpam = ServerSession.Current.Application.Settings.AntiSpam;
               try
               {
                  antiSpam.AddDomainPolicyListEntry(typed, matchWord, actionWord, note.Text ?? "");
               }
               finally
               {
                  ServerSession.Release((object)antiSpam);
               }

               result = typed;
               dialog.DialogResult = true;
               dialog.Close();
            }
            catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
            {
               DialogFields.ShowError(patternRow, ServerSession.DescribeComError(ex));
            }
         };

         var cancel = new Wpf.Ui.Controls.Button { Content = L("Cancel"), MinWidth = 88 };
         cancel.Click += (_, _) => dialog.Close();

         dialog.Frame(L("Add a domain to the white and black list"), body, ok, cancel,
            L("The entry applies to the next session from that domain. It is consulted for unauthenticated inbound sessions of an IP range that runs the spam tests, and never to override authentication."),
            width: 520);

         return dialog.ShowDialog() == true ? result : null;
      }
   }
}
