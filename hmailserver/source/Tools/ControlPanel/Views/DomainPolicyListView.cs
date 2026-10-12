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

      // No MinHeight of the grid's own: the card around it is floored, and
      // measured at the height it is given, by the page's FillPanel.
      private readonly DataGrid entries_ = new()
      {
         AutoGenerateColumns = false,
         IsReadOnly = true,
         CanUserAddRows = false,
         CanUserDeleteRows = false,
         SelectionMode = DataGridSelectionMode.Single
      };

      private readonly InlineNotice status_ = new() { Visibility = Visibility.Collapsed };
      private readonly EmptyState empty_ = new() { Icon = Wpf.Ui.Controls.SymbolRegular.Globe24, Visibility = Visibility.Collapsed };

      // How many entries the list holds, and while the search hides some how many
      // it shows, beside the title, where an Information notice said "Entries that
      // match the search: 1 of 4." (review findings F288, F210).
      private readonly StatusPill count_ = new() { Level = StatusLevel.Normal, Visibility = Visibility.Collapsed };

      // The empty state's way to add the first domain, which a search that hides
      // every entry takes away and its clearing gives back.
      private Wpf.Ui.Controls.Button addFirst_;

      private List<EntryView> all_ = new();

      // The Added column, floored at its widest date whenever the rows are read (FloorAdded_).
      private DataGridTextColumn addedColumn_;

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
         // Allowed or Blocked, and what that does, its cell's tool tip (DomainPolicyWords).
         public string Action { get; init; }
         public string ActionTip { get; init; }
         public string ActionWord { get; init; }

         // The status mark that leads the Action cell: the Critical cross for
         // Blocked, the Normal circle for Allowed (ActionMark_).
         public FrameworkElement ActionMark { get; init; }
         public string Note { get; init; }
         public string Created { get; init; }

         // The reader's short date and time of Created, the server's clock.
         public string Added { get; init; }

         // The domain and what is done with it, in the words the cell's tool tip
         // has, then the rest with their columns' names.
         public override string ToString() => RowSpeech.Of(Pattern, ActionTip,
            RowSpeech.Labelled(L("Matched on"), MatchOn), Note, RowSpeech.Labelled(L("Added (server time)"), Added));
      }

      private static string MatchOnText_(string word) => word switch
      {
         "envelope" => L("Envelope sender's domain"),
         "helo" => L("HELO name"),
         _ => L("Either")
      };

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
               Action = DomainPolicyWords.Action(action),
               ActionTip = DomainPolicyWords.ActionTip(action),
               ActionWord = action,
               ActionMark = ActionMark_(action),
               Note = Text_(item, "note"),
               Created = Text_(item, "created"),
               Added = DomainPolicyWords.Added(Text_(item, "created"))
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
         count_.Visibility = Visibility.Collapsed;
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
         FloorAdded_();

         // The count beside the title, and while a search is typed in how many it
         // shows: no notice, which the search left saying "1 of 4" under a list of
         // four once it was typed away (review findings F288, F210).
         count_.Text = ListSearchCount.PillText(search, shown.Count, all_.Count, Plural(all_.Count, "{0} entry", "{0} entries"));
         count_.Visibility = Visibility.Visible;

         // A search that hides every entry says so, without the way to add the
         // first domain to a list that has some; it showed a blank card.
         if (ListSearchCount.NothingMatches(search, shown.Count, all_.Count))
         {
            empty_.Action = null;
            empty_.Text = ListSearchCount.NothingMatchesText(search);
            empty_.Visibility = Visibility.Visible;
            return;
         }

         empty_.Action = addFirst_;
         StatusText.Show(empty_, null, all_.Count, null,
            L("The list is empty, and an empty list changes nothing: every domain is greylisted and scored as before."));
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

      /// <summary>
      /// Edit…, Enter and a double-click: the add dialog on the selected entry,
      /// whose save changes it in place, keeping the date it was added, where an
      /// entry could only be removed and added again (review finding F266). The
      /// entry stays selected, and the notice says it was saved.
      ///
      /// The change is the REST API's, which a default install has off
      /// (RestApiPort 0), so the dialog opens only once the REST client has a
      /// session - asking for the one-time code again, since the press asks for
      /// it - and otherwise the notice says why in this page's words, at Warning:
      /// the dialog opened filled in on any entry, took the administrator's
      /// changes and refused them only at Save, with the settings pages' sentence
      /// that no COM property carries "these settings" (found re-checking
      /// a0ceb4014).
      /// </summary>
      private void EditSelected()
      {
         if (entries_.SelectedItem is not EntryView selected)
            return;

         if (!AddDomainPolicyDialog.CanChange(Window.GetWindow(this), saving: false, out string why))
         {
            Status_(StatusLevel.Warning, why);
            return;
         }

         string saved = AddDomainPolicyDialog.Edit(Window.GetWindow(this), selected.Id, selected.Pattern, selected.MatchOnWord, selected.ActionWord, selected.Note);
         if (saved == null)
            return;

         Reload();
         if (entries_.ItemsSource is List<EntryView> shown && shown.Find(e => e.Id == selected.Id) is EntryView again)
         {
            entries_.SelectedItem = again;
            entries_.ScrollIntoView(again);
         }

         Status_(StatusLevel.Good, ListOutcome.Saved(saved, DateTime.Now));
      }

      private void RemoveSelected()
      {
         if (entries_.SelectedItem is not EntryView selected)
         {
            Status_(StatusLevel.Information, L("Select an entry first."));
            return;
         }

         // Remove, drawn as the destructive act it is, with Cancel on Enter, like
         // every other confirmation that changes what the server does: a blocked
         // domain removed is accepted again at once.
         if (!MessageBox.Confirm(
               F("Remove {0} from the list?\n\nA blocked domain is accepted again at once; an allowed domain goes back under greylisting and the spam score from the next session.", selected.Pattern),
               L("Remove from the domain list"), L("Re_move"), destructive: true))
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
         root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });

         // The count is a polite live region, so that a screen reader hears it as
         // it changes, as the notice it replaced was heard (Scaffold/StatusPill).
         AutomationProperties.SetLiveSetting(count_, AutomationLiveSetting.Polite);
         root.Children.Add(new PageHeader
         {
            Status = count_,
            Actions = MakeButton(L("_Refresh"), Wpf.Ui.Controls.ControlAppearance.Secondary, (_, _) => Reload()),
            PageKey = "domainpolicylist",
            // Two sentences: what the list is, and which list to use instead. The
            // notice under the toolbar that said the second, in a fragment that
            // began "Not the spam exemptions...", stood on the page for good, at
            // the Information level of a passing fact; what Allowed and Blocked
            // do is the Action cell's tool tip and the dialog's, and what a
            // leading dot matches the dialog's (review finding F266).
            Subtitle = L("Sending organisations allowed or blocked by their domain or HELO name, in inbound sessions that have not signed in. To exempt an address or an IP range from every check use the spam exemptions; to decide per recipient use the allow and block list.")
         });

         // Filtered as it is typed in, as the domains and IP ranges pages are: a
         // filter over the rows already here, which asked for Enter, and nothing on
         // the page said so (review finding F210). Not over a list that could not
         // be read, which Present_ would read again on every keystroke: its notice
         // has Try again.
         toolbar_.SearchTextChanged += (_, _) => { if (!ReadFailure.IsShown(status_)) Present_(); };

         // The IP ranges shape: the list takes the height the toolbar and the
         // notices above it leave in the window, down to a floor of three rows,
         // and below that the page scrolls. The list was a grid's star row under
         // rows sized to their content, which in the narrowest window (760 by
         // 520) left it half a row under the explanation.
         ScrollViewer scroll = FillPanel.ScrollingPage(out FillPanel parts);
         Grid.SetRow(scroll, 1);
         root.Children.Add(scroll);

         // The list's verbs, Delete and then Add…, the Primary, at the right edge;
         // Refresh is the page's and is in the header (docs/ControlPanelDesign.md,
         // List pages; review finding F288). Wraps in a narrow window.
         var actions = new WrapPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Right };
         var remove = MakeButton(L("_Delete"), Wpf.Ui.Controls.ControlAppearance.Secondary, (_, _) => RemoveSelected());
         ButtonStyles.DangerText(remove);
         actions.Children.Add(remove);
         SelectionGate.Bind(entries_, remove);
         addButton_ = MakeButton(L("_Add…"), Wpf.Ui.Controls.ControlAppearance.Primary, (_, _) => AddEntry());
         actions.Children.Add(addButton_);
         // Delete deletes the selected entry after asking; Insert adds one.
         GridKeys.Wire(entries_, null, remove, addButton_);
         foreach (FrameworkElement button in actions.Children)
            button.Margin = new Thickness(button.Margin.Left, 2, button.Margin.Right, 2);

         // Edit…, before Delete, opens the selected entry; Enter, F2 and a
         // double-click press it (review finding F266).
         var edit = MakeButton(L("_Edit…"), Wpf.Ui.Controls.ControlAppearance.Secondary, (_, _) => EditSelected());
         edit.Margin = new Thickness(8, 2, 0, 2);
         AutomationProperties.SetAutomationId(edit, "domainpolicy-edit");
         actions.Children.Insert(actions.Children.IndexOf(remove), edit);
         SelectionGate.Bind(entries_, edit);
         GridKeys.Wire(entries_, edit, null, null);
         toolbar_.Actions = actions;
         parts.Children.Add(toolbar_);

         parts.Children.Add(status_);

         BuildColumns_();
         AutomationProperties.SetName(entries_, L("Allowed and blocked domains"));

         // The grid's three rows on a FillPanel of its own and the card floored
         // at what it holds, so that the empty state laid over the grid, its
         // sentence and the action that adds the first domain (review finding
         // F207), raises the card's floor where it needs more than the rows.
         var rows = new FillPanel();
         rows.Children.Add(FillPanel.Filling(entries_, FillPanel.ListRowsFloor));
         var first = new Wpf.Ui.Controls.Button { Content = L("Add the _first domain…") };
         AutomationProperties.SetAutomationId(first, "domainpolicy-add-first");
         first.Click += (_, _) => AddEntry();
         addFirst_ = first;
         empty_.Action = first;
         var host = new Grid();
         host.Children.Add(rows);
         host.Children.Add(empty_);

         // The empty state under the grid's column headers, centred where the
         // rows would be, never drawn across the headers' line.
         EmptyStateUnderHeaders.Keep(empty_, entries_);

         var listCard = new Card { Padding = new Thickness(DesignTokens.ListCardPadding), Content = host };
         parts.Children.Add(FillPanel.Filling(listCard, 0));

         Content = root;
      }

      // Every column trimmed with its whole text in a tool tip and floored at its
      // header's width: Added read "2026-" and the note "Always accept, whatev",
      // four letters wide in German, with nothing to say they were cut, while
      // the Action column spent about 250 pixels on "Allowed: not greylisted, not
      // refused on score" in every row (review finding F266). Action is one word
      // now, what it does its tool tip; Added is the reader's short date and
      // time, marked as the server's, as wide as its widest date (FloorAdded_).
      private void BuildColumns_()
      {
         entries_.Columns.Add(Shared_(L("Domain or suffix"), nameof(EntryView.Pattern), 160));
         entries_.Columns.Add(Fitted_(L("Matched on"), nameof(EntryView.MatchOn)));
         entries_.Columns.Add(ActionColumn_());
         entries_.Columns.Add(Shared_(L("Note"), nameof(EntryView.Note), 100));
         string added = L("Added (server time)");
         addedColumn_ = GridStyles.Trimmed(new DataGridTextColumn
         {
            Header = added,
            Binding = new System.Windows.Data.Binding(nameof(EntryView.Added)),
            // Sorted by the server's own yyyy-MM-dd HH:mm:ss, which sorts as the times
            // do, not by the reader's form of it, in which 10/1/2026 comes before 9/28/2026.
            SortMemberPath = nameof(EntryView.Created),
            Width = DataGridLength.Auto,
            MinWidth = GridStyles.HeaderWidth(added)
         });
         entries_.Columns.Add(addedColumn_);
      }

      /// <summary>
      /// The Added column as wide as its header and the widest of every entry's dates,
      /// measured in the grid's font at the Windows text size, as the quarantine's Held
      /// is: fixed at 150 pixels, it read "9/28/2026 8:18 P…" at 150 per cent. Measured
      /// over every entry, not the ones a search leaves, so that a search does not move it.
      /// </summary>
      private void FloorAdded_()
      {
         double floor = GridStyles.HeaderWidth((string)addedColumn_.Header);
         foreach (EntryView entry in all_)
            floor = Math.Max(floor, GridStyles.CellWidth(entry.Added));
         addedColumn_.MinWidth = GridStyles.WholeDevicePixels(floor);
      }

      /// <summary>A column of names that shares the width the others leave, over a floor of so many pixels or its header's width where that is more.</summary>
      private static DataGridTextColumn Shared_(string header, string path, double floor)
         => GridStyles.Trimmed(new DataGridTextColumn
         {
            Header = header,
            Binding = new System.Windows.Data.Binding(path),
            Width = new DataGridLength(1, DataGridLengthUnitType.Star),
            MinWidth = Math.Max(floor, GridStyles.HeaderWidth(header))
         });

      /// <summary>A column of a few words, as wide as its widest and never narrower than its header.</summary>
      private static DataGridTextColumn Fitted_(string header, string path)
         => GridStyles.Trimmed(new DataGridTextColumn
         {
            Header = header,
            Binding = new System.Windows.Data.Binding(path),
            Width = DataGridLength.Auto,
            MinWidth = GridStyles.HeaderWidth(header)
         });

      /// <summary>The Action cell's mark's box, and the gap between it and its word.</summary>
      private const double ActionMarkWidth = 10;
      private const double ActionMarkGap = 6;

      /// <summary>
      /// Allowed or Blocked, led by its status mark, and in the cell's tool tip what
      /// that does to a session; the row tells a screen reader the same
      /// (EntryView.ToString). A blocked entry differed from an allowed one only in
      /// the word at the row's end (review finding F323): the Critical cross leads
      /// Blocked and the Normal circle Allowed, the shapes the status pills use, so
      /// that a blocked domain is found at a glance and the difference survives
      /// greyscale. The column is floored at its header and at either word with its
      /// mark, and sorts and is copied by the word: a template column copies nothing of
      /// its own, and Ctrl+C on an entry copied an empty field where the text column it
      /// replaced copied Allowed or Blocked.
      /// </summary>
      private static DataGridTemplateColumn ActionColumn_()
      {
         string header = L("Action");
         double floor = GridStyles.HeaderWidth(header);
         foreach (string action in new[] { "white", "black" })
            floor = Math.Max(floor, GridStyles.CellWidth(DomainPolicyWords.Action(action), ActionMarkWidth + ActionMarkGap));

         var panel = new FrameworkElementFactory(typeof(StackPanel));
         panel.SetValue(StackPanel.OrientationProperty, Orientation.Horizontal);
         panel.SetValue(VerticalAlignmentProperty, VerticalAlignment.Center);
         panel.SetBinding(ToolTipProperty, new System.Windows.Data.Binding(nameof(EntryView.ActionTip)));

         var mark = new FrameworkElementFactory(typeof(ContentPresenter));
         mark.SetBinding(ContentPresenter.ContentProperty, new System.Windows.Data.Binding(nameof(EntryView.ActionMark)));
         mark.SetValue(MarginProperty, new Thickness(0, 0, ActionMarkGap, 0));
         mark.SetValue(VerticalAlignmentProperty, VerticalAlignment.Center);
         panel.AppendChild(mark);

         var word = new FrameworkElementFactory(typeof(TextBlock));
         word.SetBinding(TextBlock.TextProperty, new System.Windows.Data.Binding(nameof(EntryView.Action)));
         word.SetValue(TextBlock.TextTrimmingProperty, TextTrimming.CharacterEllipsis);
         word.SetValue(VerticalAlignmentProperty, VerticalAlignment.Center);
         panel.AppendChild(word);

         return new DataGridTemplateColumn
         {
            Header = header,
            CellTemplate = new DataTemplate { VisualTree = panel },
            SortMemberPath = nameof(EntryView.Action),
            ClipboardContentBinding = new System.Windows.Data.Binding(nameof(EntryView.Action)),
            Width = DataGridLength.Auto,
            MinWidth = floor
         };
      }

      /// <summary>
      /// The mark for the server's action, white or black: its level's shape in its
      /// level's colour, by resource key so that a change of theme recolours it (as
      /// the TCP/IP ports page's Listening mark is drawn).
      /// </summary>
      private static FrameworkElement ActionMark_(string action)
      {
         StatusPresentation presentation = StatusSemantics.For(action == "black" ? StatusLevel.Critical : StatusLevel.Normal);
         var path = new System.Windows.Shapes.Path
         {
            Width = ActionMarkWidth,
            Height = ActionMarkWidth,
            Stretch = System.Windows.Media.Stretch.Fill,
            VerticalAlignment = VerticalAlignment.Center
         };
         ShapeMarkVisuals.ApplyMark(path, presentation.Shape, presentation.BrushKey);
         return path;
      }

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
   /// Puts one domain on the list, or changes one already there: the pattern,
   /// what it is matched on, allowed or blocked, and a note for whoever reads the
   /// list next. Its own class so that its Alt keys are its own scope and not the
   /// page's. The server's refusal - a pattern that is not a domain, a pair
   /// already listed - is shown under the pattern, in its words.
   ///
   /// A change is the REST API's (DomainPolicyWords), which keeps the entry's id
   /// and the date it was added; where the REST API cannot be used from here the
   /// page says why and the dialog is not opened (CanChange), and where it cannot
   /// be reached at Save the dialog stays open and says why above its fields,
   /// and nothing changes.
   /// </summary>
   internal static class AddDomainPolicyDialog
   {
      /// <summary>An entry as the list read it, for the dialog to change.</summary>
      private sealed record Existing(int Id, string Pattern, string MatchOn, string Action, string Note);

      /// <summary>The pattern as added, or null when nothing was.</summary>
      public static string Show(Window owner) => Open_(owner, null);

      /// <summary>The entry's pattern as saved, or null when nothing was changed.</summary>
      public static string Edit(Window owner, int id, string pattern, string matchOn, string action, string note)
         => Open_(owner, new Existing(id, pattern ?? "", matchOn, action, note ?? ""));

      private static string Open_(Window owner, Existing existing)
      {
         var dialog = new FramedDialog { Owner = owner };
         if (existing == null)
            dialog.Title = L("Add a domain to the list");
         else
            dialog.Title = L("Edit a domain on the list");

         var pattern = new Wpf.Ui.Controls.TextBox { Text = existing?.Pattern ?? "" };

         var matchOn = new ComboBox();
         matchOn.Items.Add(DialogFields.Combo(L("Either the envelope sender's domain or the HELO name"), 2));
         matchOn.Items.Add(DialogFields.Combo(L("The envelope sender's domain"), 0));
         matchOn.Items.Add(DialogFields.Combo(L("The HELO name"), 1));
         DialogFields.SelectCombo(matchOn, existing == null ? 2 : DomainPolicyWords.MatchOnChoice(existing.MatchOn), orFirst: true);

         var action = new ComboBox();
         action.Items.Add(DialogFields.Combo(L("Allowed: not greylisted, not refused on the spam score"), 0));
         action.Items.Add(DialogFields.Combo(L("Blocked: refused at MAIL FROM"), 1));
         DialogFields.SelectCombo(action, existing == null ? 0 : DomainPolicyWords.ActionChoice(existing.Action), orFirst: true);

         var note = new Wpf.Ui.Controls.TextBox { Text = existing?.Note ?? "" };

         // A refusal that is not the pattern's - the REST API off, out of reach,
         // or asking for a code that was not given - above the fields.
         var notice = DialogFields.Notice();

         var body = new StackPanel();
         body.Children.Add(notice);
         var patternRow = DialogFields.Field(L("_Domain or suffix"), pattern, L("example.com is that domain alone; .example.com is the domain and everything beneath it."));
         body.Children.Add(patternRow);
         body.Children.Add(DialogFields.Field(L("_Matched on"), matchOn, L("A domain with rotating outbound hosts is usually recognised by the HELO name; a domain that forges nothing by its envelope sender.")));
         body.Children.Add(DialogFields.Field(L("A_ction"), action, L("An allowed domain still runs every test and still marks at the mark threshold; a blocked one is refused before a single test runs.")));
         body.Children.Add(DialogFields.Field(L("_Note"), note, L("Kept with the entry, for whoever reads the list next.")));

         string result = null;

         // Add on an Add dialog, Save on an Edit one, as the list editor's are
         // (FieldDialog). The two are never on screen together, so each may share
         // a key with the other, never with a field's caption:
         // DomainPolicyEditTests holds that in every catalogue, which
         // check-mnemonics.py, reading both as captions of one scope, cannot.
         var ok = new Wpf.Ui.Controls.Button { Content = existing == null ? L("_Add") : L("_Save"), Appearance = Wpf.Ui.Controls.ControlAppearance.Primary, MinWidth = 88 };
         ok.Click += (_, _) =>
         {
            DialogFields.ClearErrors(patternRow);
            notice.Hide();

            string typed = pattern.Text?.Trim() ?? "";
            if (typed.Length == 0)
            {
               DialogFields.ShowError(patternRow, L("A domain is required."));
               return;
            }

            string matchWord = DomainPolicyWords.MatchOnWord(DialogFields.ComboValue(matchOn, 2));
            string actionWord = DomainPolicyWords.ActionWord(DialogFields.ComboValue(action, 0));

            if (existing != null)
            {
               if (Change_(dialog, existing.Id, typed, matchWord, actionWord, note.Text ?? "", patternRow, notice))
               {
                  result = typed;
                  dialog.DialogResult = true;
                  dialog.Close();
               }

               return;
            }

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

         dialog.Frame(L("Allow or block a domain"), body, ok, cancel,
            L("The entry applies to the next session from that domain. It is consulted for unauthenticated inbound sessions of an IP range that runs the spam tests, and never to override authentication."),
            width: DesignTokens.Dialog.DefaultWidth);
         // A refusal of the domain, typed or the server's, is said under its box
         // without moving Add and Cancel.
         dialog.KeepRoomForFieldErrors();

         // An entry being changed is named in the heading.
         if (existing != null && dialog.Content is DialogFrame frame)
            frame.Heading = F("Allow or block {0}", existing.Pattern);

         return dialog.ShowDialog() == true ? result : null;
      }

      /// <summary>
      /// Changes the entry over the REST API, as signed in to the server (the
      /// session the settings pages use, RestSettingsClient), and says a refusal
      /// in the dialog: the server's sentence about the pattern or the pair under
      /// the pattern, anything else - the listener off, out of reach, a code not
      /// given, the entry gone - above the fields. True when the entry was changed.
      /// </summary>
      private static bool Change_(Window dialog, int id, string pattern, string matchOn, string action, string note, FieldRow patternRow, InlineNotice notice)
      {
         // Save is a request of its own: the session made again where it has
         // gone, and the code asked for again where it was not given before,
         // where every Save after one cancelled prompt refused without asking
         // for the rest of the session, "Open the page again to be asked for the
         // code." - which reopening this page did not do (found re-checking
         // a0ceb4014).
         if (!CanChange(dialog, saving: true, out string why))
         {
            notice.Show(StatusLevel.Warning, why);
            return false;
         }

         RestSettingsClient client = RestSettingsClient.For(ServerSession.Current);
         RestSettingsClient.Reply reply = client.Put(DomainPolicyWords.EntryPath(id), DomainPolicyWords.ChangeBody(pattern, matchOn, action, note), () => AskForCode_(dialog));

         if (reply.Ok)
            return true;

         string sentence = reply.Status == 400 ? DiscoveryReport.ErrorOf(reply.Body) : "";
         if (sentence.Length > 0)
            DialogFields.ShowError(patternRow, sentence);
         else if (client.CodeDeclined)
            notice.Show(StatusLevel.Warning, DomainPolicyWords.CodeNotGiven(saving: true));
         else
            notice.Show(StatusLevel.Critical, client.RefusalFor(reply));
         return false;
      }

      /// <summary>
      /// Whether the REST API, which changes an entry in place, can be used from
      /// here now: the REST client's session made, or there already, with the
      /// one-time code asked for again (askAgain), since the press of Edit… or
      /// Save that asks is a request for it. False and why not, in the page's
      /// words (DomainPolicyWords.Unchangeable, CodeNotGiven), otherwise.
      /// </summary>
      internal static bool CanChange(Window owner, bool saving, out string why)
      {
         why = null;
         ServerSession session = ServerSession.Current;
         if (session == null)
         {
            why = L("Sign in to a server first.");
            return false;
         }

         RestSettingsClient client = RestSettingsClient.For(session);
         if (client.Open(() => AskForCode_(owner), askAgain: true, out string refusal))
            return true;

         if (client.CodeDeclined)
         {
            why = DomainPolicyWords.CodeNotGiven(saving);
            return false;
         }

         var ini = new IniFeatureStore();
         why = DomainPolicyWords.Unchangeable(session.RestAuthorization() != null,
            ini.Read("RestApiPort", "0"), ini.Read("RestApiBindAddress", "127.0.0.1"), // no-loc
            ServerSession.IsLocalSession, refusal);
         return false;
      }

      /// <summary>The administrator's one-time code, asked for over <paramref name="owner"/>; null when none was given.</summary>
      private static string AskForCode_(Window owner)
      {
         var prompt = new TotpPromptDialog(owner);
         return prompt.ShowDialog() == true ? prompt.Code : null;
      }
   }
}
