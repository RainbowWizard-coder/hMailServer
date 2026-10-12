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
using MessageBox = hMailServer.ControlPanel.Views.Dialogs;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Views
{
   /// <summary>
   /// The addresses a domain no longer sends to, and the bounce counts that put
   /// them there.
   ///
   /// A sender's reputation is spent by sending to addresses that are known to be
   /// dead, and until this page there was no way to see which ones a domain was
   /// still trying. The list is per sending domain, because a dead address is a
   /// fact about the mail one organisation sends, and one customer's old export
   /// must not stop another's mail.
   ///
   /// Both tables come from Utilities methods that return JSON on an interface that
   /// already exists - the same documents the REST API answers - so this page needed
   /// no new COM interface and no registration step. Removing an entry is the one
   /// thing here that changes what the server does next, so it asks.
   /// </summary>
   public class SuppressionListView : UserControl, IPageLifecycle
   {
      // Capped: the box sized itself to the longest domain name, and with one
      // of sixty characters it filled the toolbar in the narrowest window, its
      // own chevron cut off, and pushed Refresh onto a second line in a 1280
      // one. A name longer than the box ends in an ellipsis, whole in its tip.
      private readonly ComboBox domainChoice_ = new() { MinWidth = 220, MaxWidth = 320 };
      private readonly Toolbar toolbar_ = new() { SearchPlaceholder = L("Address contains") };

      // No MinHeight of the grids' own: the card around each is floored, and
      // measured at the height it is given, by the page's FillPanel.
      private readonly DataGrid entries_ = new()
      {
         AutoGenerateColumns = false,
         IsReadOnly = true,
         CanUserAddRows = false,
         CanUserDeleteRows = false,
         SelectionMode = DataGridSelectionMode.Single
      };

      // The address stays in view as the counts scroll sideways: under the list's
      // own category names (review finding F318) the grid's eleven columns need
      // about 1,200 pixels, more than a window of 1280 gives it, and scrolled to its
      // dates the row no longer said whose counts they were.
      private readonly DataGrid counts_ = new()
      {
         AutoGenerateColumns = false,
         IsReadOnly = true,
         CanUserAddRows = false,
         CanUserDeleteRows = false,
         SelectionMode = DataGridSelectionMode.Single,
         FrozenColumnCount = 1
      };

      // The list's two columns of dates, floored at their widest date whenever the rows are read (FloorAtCells_).
      private DataGridTextColumn sinceColumn_;
      private DataGridTextColumn expiresColumn_;

      private readonly InlineNotice status_ = new() { Visibility = Visibility.Collapsed };

      // How many addresses the domain's list holds, beside the title, where an
      // Information notice said "3 addresses listed." (review finding F288).
      private readonly StatusPill count_ = new() { Level = StatusLevel.Normal, Visibility = Visibility.Collapsed };

      private readonly InlineNotice processingOff_ = new() { Visibility = Visibility.Collapsed };
      private readonly EmptyState empty_ = new() { Icon = Wpf.Ui.Controls.SymbolRegular.MailWarning24, Visibility = Visibility.Collapsed };

      private const int PageSize = 200;

      // Add waits for a read that works (Services/ReadFailure).
      private Button addButton_;

      // The search asks the server once typing pauses (review finding F210).
      private readonly SearchDelay searchDelay_;

      public SuppressionListView()
      {
         searchDelay_ = new SearchDelay(Reload);
         Build();
      }

      // Read on entry rather than in the constructor, because the page cache keeps
      // this instance for the session: an entry removed over REST since the page
      // was first opened must not still be shown when somebody comes back to it.
      public void OnEnter()
      {
         string selected = domainChoice_.SelectedItem as string;

         // The domains could not be read: the page says so and nothing else. It
         // went on to say "There are no domains, so there is no list to show",
         // which is what a server with no domains looks like, not one that could
         // not be asked.
         if (!LoadDomains_())
            return;

         if (selected != null && domainChoice_.Items.Contains(selected))
            domainChoice_.SelectedItem = selected;

         Reload();
      }

      public void OnLeave() => searchDelay_.Cancel();

      private string Domain => domainChoice_.SelectedItem as string ?? "";

      private void Status_(StatusLevel level, string text)
      {
         ReadFailure.Clear(status_);
         status_.Level = level;
         status_.Text = text;
         status_.Visibility = Visibility.Visible;
      }

      /// <summary>Fills the domain list; false when the domains could not be read.</summary>
      private bool LoadDomains_()
      {
         domainChoice_.Items.Clear();

         dynamic domains = null;
         bool read = false;

         try
         {
            domains = ServerSession.Current.Application.Domains;
            int count = (int)domains.Count;

            var names = new List<string>();

            for (int i = 0; i < count; i++)
            {
               dynamic domain = null;

               try
               {
                  domain = domains.Item[i];
                  names.Add((string)domain.Name);
               }
               catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
               {
                  // One unreadable domain costs its row, not the list.
               }
               finally
               {
                  ServerSession.Release((object)domain);
               }
            }

            names.Sort(StringComparer.OrdinalIgnoreCase);
            foreach (string name in names)
               domainChoice_.Items.Add(name);
            read = true;
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            // The lists go with the domains, the notice says what happened, with
            // Try again, and Add waits for a read that works (Services/ReadFailure).
            ReadFailure.Show(status_, empty_, entries_, L("Could not read the domains: "), ex, OnEnter, addButton_);
            counts_.ItemsSource = null;
            processingOff_.Visibility = Visibility.Collapsed;
         }
         finally
         {
            ServerSession.Release((object)domains);
         }

         if (domainChoice_.Items.Count > 0 && domainChoice_.SelectedIndex < 0)
            domainChoice_.SelectedIndex = 0;

         return read;
      }

      private void Reload()
      {
         ReadFailure.Clear(status_, addButton_);
         status_.Visibility = Visibility.Collapsed;
         processingOff_.Visibility = Visibility.Collapsed;
         count_.Visibility = Visibility.Collapsed;

         if (Domain.Length == 0)
         {
            entries_.ItemsSource = null;
            counts_.ItemsSource = null;
            StatusText.Show(empty_, null, 0, null, L("There are no domains, so there is no list to show."));
            return;
         }

         try
         {
            dynamic utilities = ServerSession.Current.Application.Utilities;
            string search = toolbar_.SearchText?.Trim() ?? "";

            SuppressionListReport list;
            SuppressionListReport counts;

            try
            {
               list = SuppressionListReport.ParseSuppressions((string)utilities.GetSuppressionList(Domain, search, PageSize));
               counts = SuppressionListReport.ParseCounts((string)utilities.GetBounceCounts(Domain, search, PageSize));
            }
            finally
            {
               ServerSession.Release((object)utilities);
            }

            if (list.Error != null || counts.Error != null)
            {
               Unread_(null, F("The server's answer could not be read: {0}", list.Error ?? counts.Error));
               return;
            }

            List<EntryView> entries = Present_(list.Entries);
            List<CountView> countRows = PresentCounts_(counts.Counts);
            entries_.ItemsSource = entries;
            counts_.ItemsSource = countRows;
            FloorAtCells_(sinceColumn_, entries, e => e.Since);
            FloorAtCells_(expiresColumn_, entries, e => e.Expires);

            StatusText.Show(empty_, null, list.Total, null,
               L("No address is on this domain's suppression list. An address is put here when mail to it bounces for good, or when you add one."));

            if (!list.ProcessingEnabled)
            {
               processingOff_.Level = StatusLevel.Warning;
               processingOff_.Text = L("Bounce processing is off, so nothing is being counted or listed and the entries here are not applied. The switch is on the Delivery of e-mail page.");
               processingOff_.Visibility = Visibility.Visible;
            }

            count_.Text = Plural(list.Total, "{0} address", "{0} addresses");
            count_.Visibility = Visibility.Visible;

            // A list longer than the page reads is a state with a consequence -
            // what is not shown is still applied - so it keeps its notice.
            if (list.Total > list.Entries.Count)
               Status_(StatusLevel.Information, PluralF(list.Total,
                  "{0} address listed, showing the {1} most recent. Search to narrow them.",
                  "{0} addresses listed, showing the {1} most recent. Search to narrow them.",
                  list.Total, list.Entries.Count));
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            Unread_(L("Could not read the suppression list: "), ServerSession.DescribeComError(ex));
         }
      }

      /// <summary>
      /// A read that failed: neither the entries of the last read nor "No address
      /// is on this domain's suppression list" is a fact this page has now, so
      /// both go with the counts, the notice says what happened, with Try again,
      /// and Add waits for a read that works (Services/ReadFailure).
      /// </summary>
      private void Unread_(string prefix, string why)
      {
         ReadFailure.Show(status_, empty_, entries_, prefix, why, Reload, addButton_);
         counts_.ItemsSource = null;
      }

      /// <summary>An entry as the grid shows it: the server's words turned into the reader's.</summary>
      private sealed class EntryView
      {
         public string Address { get; init; }
         public string Why { get; init; }
         public string Status { get; init; }
         public string Source { get; init; }
         // The server's dates in the reader's short date and time, under headers that
         // name its clock, where they were its own "2026-10-08 18:30:00" (review finding
         // F321); each column sorts by the time, an entry that never expires after
         // every one that does.
         public string Since { get; init; }
         public DateTime? SinceAt { get; init; }
         public string Expires { get; init; }
         public DateTime? ExpiresAt { get; init; }
         public string Detail { get; init; }

         // An entry put here by hand, whose detail is the administrator's note
         // rather than a receiving server's answer.
         public bool ByHand { get; init; }

         // The address, why it is listed and the status, then the two dates with
         // their columns' names, as one statement.
         public override string ToString()
            => RowSpeech.Of(Address, Why, Status, Source, RowSpeech.Labelled(L("Since (server time)"), Since), RowSpeech.Labelled(L("Expires (server time)"), Expires));
      }

      private sealed class CountView
      {
         public string Address { get; init; }
         public int Hard { get; init; }
         public int Soft { get; init; }
         public int MailboxFull { get; init; }
         public int Policy { get; init; }
         public int Other { get; init; }
         public int InARow { get; init; }
         public string LastStatus { get; init; }
         // As the list's dates are: the reader's form, the server's clock, sorted by the
         // time, an address never delivered to before every one that was.
         public string LastBounce { get; init; }
         public DateTime? LastBounceAt { get; init; }
         public string LastDelivered { get; init; }
         public DateTime? LastDeliveredAt { get; init; }
         public string Listed { get; init; }

         // The selected address's last status code, last bounce and last delivery, in
         // the strip under the counts: as three columns of the grid they took it 317
         // pixels past its card at 1280 by 800 (559 in German), the dates off screen
         // on opening (final check of Tier 2).
         public string Strip => string.Join("\n", new[]
         {
            string.IsNullOrEmpty(LastStatus) ? null : F("Last status code: {0}", LastStatus),
            F("Last bounce: {0}", LastBounceAt is DateTime bounced ? ServerTimeText.Marked(bounced) : LastBounce),
            F("Last delivered: {0}", LastDeliveredAt is DateTime delivered ? ServerTimeText.Marked(delivered) : LastDelivered),
         }.Where(line => !string.IsNullOrEmpty(line)));

         // The address, then each count with its column's name: a row of bare
         // numbers says nothing without the columns. The columns are named by the
         // categories the list's Reason column says (CategoryText_).
         public override string ToString() => RowSpeech.Of(Address,
            RowSpeech.Labelled(CategoryText_("hard"), Count_(Hard)), RowSpeech.Labelled(CategoryText_("soft"), Count_(Soft)),
            RowSpeech.Labelled(CategoryText_("mailbox_full"), Count_(MailboxFull)), RowSpeech.Labelled(CategoryText_("policy"), Count_(Policy)),
            RowSpeech.Labelled(CategoryText_("other"), Count_(Other)), RowSpeech.Labelled(L("In a row"), Count_(InARow)),
            RowSpeech.Labelled(L("Last status code"), LastStatus), RowSpeech.Labelled(L("Last delivered (server time)"), LastDelivered), Listed);

         private static string Count_(int count) => count.ToString(System.Globalization.CultureInfo.CurrentCulture);
      }

      // One name per category in both grids: the list's Reason column and the
      // bounce counts' headers, which said "Soft" and "Policy" beside "Temporary
      // failure" and "Policy refusal" (review finding F318).
      private static string CategoryText_(string category) => category switch
      {
         "hard" => L("Hard bounce"),
         "mailbox_full" => L("Mailbox full"),
         "policy" => L("Policy refusal"),
         "soft" => L("Temporary failure"),
         "other" => L("Other failure"),
         _ => L("Added by hand")
      };

      private static string SourceText_(string source) => source switch
      {
         "bounce" => L("Refused while delivering"),
         "report" => L("A delivery report"),
         _ => L("An administrator")
      };

      private static List<EntryView> Present_(IReadOnlyList<SuppressionEntryRow> rows)
      {
         var views = new List<EntryView>();

         foreach (SuppressionEntryRow row in rows)
         {
            views.Add(new EntryView
            {
               Address = row.Address,
               Why = row.Source == "manual" ? CategoryText_("") : CategoryText_(row.Category),
               Status = row.Status,
               Source = SourceText_(row.Source),
               Since = ServerTimeText.Cell(row.Created),
               SinceAt = ServerTimeText.Parse(row.Created),
               Expires = row.Expires.Length == 0 ? L("Never") : row.Expired ? F("{0} (expired)", ServerTimeText.Cell(row.Expires)) : ServerTimeText.Cell(row.Expires),
               ExpiresAt = row.Expires.Length == 0 ? DateTime.MaxValue : ServerTimeText.Parse(row.Expires),
               Detail = row.RemoteMta.Length == 0 ? row.Diagnostic : row.RemoteMta + ": " + row.Diagnostic, // no-loc: a host name and the words it answered with
               ByHand = row.Source == "manual"
            });
         }

         return views;
      }

      private static List<CountView> PresentCounts_(IReadOnlyList<BounceCountRow> rows)
      {
         var views = new List<CountView>();

         foreach (BounceCountRow row in rows)
         {
            views.Add(new CountView
            {
               Address = row.Address,
               Hard = row.Hard,
               Soft = row.Soft,
               MailboxFull = row.MailboxFull,
               Policy = row.Policy,
               Other = row.Other,
               InARow = row.ConsecutiveHard,
               LastStatus = row.LastStatus,
               LastBounce = ServerTimeText.Cell(row.LastBounce),
               LastBounceAt = ServerTimeText.Parse(row.LastBounce),
               LastDelivered = row.LastDelivered.Length == 0 ? L("Never") : ServerTimeText.Cell(row.LastDelivered),
               LastDeliveredAt = ServerTimeText.Parse(row.LastDelivered),
               Listed = row.Suppressed ? L("Listed") : ""
            });
         }

         return views;
      }

      private void AddEntry()
      {
         if (Domain.Length == 0)
            return;

         string added = AddSuppressionDialog.Show(Window.GetWindow(this), Domain);

         if (added != null)
         {
            Reload();
            Status_(StatusLevel.Good, F("{0} is on the suppression list of {1}.", added, Domain));
         }
      }

      private void RemoveSelected()
      {
         if (entries_.SelectedItem is not EntryView selected)
         {
            Status_(StatusLevel.Information, L("Select an address first."));
            return;
         }

         // Cancel on Enter, like every other confirmation that changes what the
         // server does: the next message to this address will be sent.
         if (!MessageBox.Confirm(
               F("Remove {0} from the suppression list of {1}?\n\nThe next message to it will be sent, and it is listed again only after as many hard bounces in a row as the threshold asks for.", selected.Address, Domain),
               L("Remove from the suppression list"), L("Re_move"), destructive: true))
         {
            return;
         }

         try
         {
            bool removed;
            dynamic utilities = ServerSession.Current.Application.Utilities;
            try
            {
               removed = (bool)utilities.RemoveSuppression(Domain, selected.Address);
            }
            finally
            {
               ServerSession.Release((object)utilities);
            }

            Reload();
            Status_(removed ? StatusLevel.Good : StatusLevel.Information,
               removed ? F("{0} was removed from the list.", selected.Address) : F("{0} was not on the list any more.", selected.Address));
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

         // The list page pattern (docs/ControlPanelDesign.md, List pages; review
         // finding F288): the count and Refresh in the header, Remove and then
         // Add…, the Primary, in the toolbar over the list. They were in the
         // header because beside the domain box they had no width left in the
         // narrowest window and were not drawn at all; the toolbar puts them on a
         // row of their own there now (Services/ToolbarRows).
         var actions = new WrapPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Right };
         var remove = MakeButton(L("Re_move"), Wpf.Ui.Controls.ControlAppearance.Secondary, (_, _) => RemoveSelected());
         ButtonStyles.DangerText(remove);
         actions.Children.Add(remove);
         SelectionGate.Bind(entries_, remove);
         addButton_ = MakeButton(L("_Add…"), Wpf.Ui.Controls.ControlAppearance.Primary, (_, _) => AddEntry());
         actions.Children.Add(addButton_);
         // Delete removes the selected address after asking; Insert adds one.
         GridKeys.Wire(entries_, null, remove, addButton_);
         foreach (FrameworkElement button in actions.Children)
            button.Margin = new Thickness(button.Margin.Left, 2, button.Margin.Right, 2);
         toolbar_.Actions = actions;

         // The count is a polite live region, so that a screen reader hears it as
         // it changes, as the notice it replaced was heard (Scaffold/StatusPill).
         AutomationProperties.SetLiveSetting(count_, AutomationLiveSetting.Polite);
         root.Children.Add(new PageHeader
         {
            PageKey = "suppressions",
            Subtitle = L("Addresses a domain no longer sends to because mail to them bounced for good, or because somebody put them here - with the counts behind each one. The list is per sending domain."),
            Status = count_,
            Actions = MakeButton(L("_Refresh"), Wpf.Ui.Controls.ControlAppearance.Secondary, (_, _) => searchDelay_.Now())
         });

         // The IP ranges shape, with two lists: the list and the bounce counts
         // share the height the toolbar and the notices leave in the window, two
         // to one, each down to a floor of three rows, and below that the page
         // scrolls. The two were a grid's star rows under rows sized to their
         // content, which in the narrowest window (760 by 520) left the bounce
         // counts a sliver.
         ScrollViewer scroll = FillPanel.ScrollingPage(out FillPanel parts);
         Grid.SetRow(scroll, 1);
         root.Children.Add(scroll);

         // The box captioned, with an Alt key, as the quarantine page's Group by
         // is: a bare box of domain names beside the search said what it chose
         // to nobody. The caption's words are the box's accessible name.
         AutomationProperties.SetName(domainChoice_, MnemonicText.Strip(L("Sending _domain")));
         domainChoice_.SelectionChanged += (_, _) => Reload();
         domainChoice_.ItemTemplate = TrimmedItem_();
         // The search is a query to the server, not a filter over rows already
         // here, so it runs once typing pauses rather than on every keystroke -
         // and no longer on Enter alone, which nothing on the page said (review
         // finding F210). Enter runs it at once, and Refresh reads again with
         // whatever the box holds.
         toolbar_.SearchTextChanged += (_, _) => searchDelay_.Restart();
         toolbar_.PreviewKeyDown += (_, e) => { if (e.Key == System.Windows.Input.Key.Enter) searchDelay_.Now(); };

         var filters = new StackPanel { Orientation = Orientation.Horizontal };
         filters.Children.Add(new Label { Content = L("Sending _domain"), Target = domainChoice_, VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(0, 0, 6, 0) });
         filters.Children.Add(domainChoice_);
         toolbar_.Filters = filters;
         parts.Children.Add(toolbar_);

         parts.Children.Add(processingOff_);

         parts.Children.Add(status_);

         BuildColumns_();
         // Each grid named as its card is titled, where the list's was the page's name
         // under a card titled otherwise (review finding F318).
         AutomationProperties.SetName(entries_, L("Suppressed addresses"));
         AutomationProperties.SetName(counts_, L("Bounce counts"));

         var host = new Grid();
         host.Children.Add(entries_);
         host.Children.Add(empty_);
         // Titled, as the bounce counts' card under it is: the list's card was the
         // page's one untitled card (review finding F318).
         var listCard = new Card { Title = L("Suppressed addresses"), Padding = new Thickness(DesignTokens.ListCardPadding), Content = host };
         parts.Children.Add(FillPanel.Filling(listCard, FillPanel.TitledListCardFloor, 2));

         // An address selected in the list selects its row of the bounce counts,
         // where the two grids were not linked.
         entries_.SelectionChanged += (_, _) => SelectCountsOf_(entries_.SelectedItem as EntryView);
         // The row brought into view inside the counts' own grid and no further: by
         // the time the request reaches the grid its viewer has scrolled the row in,
         // and passed on it scrolled the page down to the counts in a short window,
         // taking the row just clicked, the search and Remove off screen. While the
         // keyboard is in the counts the page follows it there, as it does any row.
         counts_.RequestBringIntoView += (_, e) =>
         {
            if (!counts_.IsKeyboardFocusWithin)
               e.Handled = true;
         };

         // What the receiving server answered for the selected address - "550 5.1.1
         // user unknown" - or the note of one put here by hand, which was the row's
         // tool tip alone (Views/RowDetailStrip).
         parts.Children.Add(RowDetailStrip.For(entries_,
            row => ((EntryView)row).ByHand ? L("Note") : L("What the receiving server answered"),
            row => ((EntryView)row).Detail));

         // A titled card, as the discovery page's lines are, in place of a
         // caption over a bare one.
         var countsCard = new Card
         {
            Title = L("Bounce counts"),
            Padding = new Thickness(8),
            Margin = new Thickness(0, DesignTokens.Space.Md, 0, 0),
            Content = counts_
         };
         parts.Children.Add(FillPanel.Filling(countsCard, FillPanel.TitledListCardFloor, 1));

         // The selected address's last status code, last bounce and last delivery,
         // under the counts, titled by the address (CountView.Strip).
         parts.Children.Add(RowDetailStrip.For(counts_, row => ((CountView)row).Address, row => ((CountView)row).Strip));

         Content = root;
      }

      /// <summary>The bounce counts' row of the address selected in the list, selected and brought into view; none when it has none.</summary>
      private void SelectCountsOf_(EntryView entry)
      {
         CountView row = null;
         if (entry != null && counts_.ItemsSource is IEnumerable<CountView> rows)
         {
            foreach (CountView candidate in rows)
            {
               if (string.Equals(candidate.Address, entry.Address, StringComparison.OrdinalIgnoreCase))
               {
                  row = candidate;
                  break;
               }
            }
         }

         counts_.SelectedItem = row;
         if (row != null)
            counts_.ScrollIntoView(row);
      }

      /// <summary>A domain name in the box, and in its list: cut short with an ellipsis where it is wider than the box, and whole in its tool tip.</summary>
      private static DataTemplate TrimmedItem_()
      {
         var text = new FrameworkElementFactory(typeof(TextBlock));
         text.SetBinding(TextBlock.TextProperty, new System.Windows.Data.Binding());
         text.SetBinding(ToolTipProperty, new System.Windows.Data.Binding());
         text.SetValue(TextBlock.TextTrimmingProperty, TextTrimming.CharacterEllipsis);
         return new DataTemplate { VisualTree = text };
      }

      // Every column keeps a width, and the address and the source take what is
      // left between them, so that a narrow window scrolls the grid sideways
      // rather than squeezing a column out: with the address alone at two
      // stars, Source read "Refuse" and "An ad" in a 1280 window and Since and
      // Expires were off the card altogether, so when an entry lapses could not
      // be seen; and the counts' address was cut to "gone@old-provider.exampl"
      // with no ellipsis while its date columns had room to spare. Status's floor
      // holds its widest header, ステータス, on its own: an Auto column's header is
      // not always counted once the grid is narrower than its columns and scrolls
      // sideways, and floored at 64 the column read "ステー…" in the smallest window.
      // The same cut the counts' Mailbox full to "メール…" over its 64, and Last
      // status, fixed at 90, to "最後のステ…": no column of either grid is narrower
      // than its own header now, measured in the header's font (Floor_). And no
      // column of words or dates is a fixed number of pixels, which larger text cuts
      // short - at 150 per cent the dates read "2026-10-08 18:3" and Why "Added by
      // h…" (review finding F317): each is as wide as its words, over a floor of its
      // widest, measured in the grid's font at the Windows text size.
      private void BuildColumns_()
      {
         // Reason and Status code, where Why and Status named the same two things
         // the bounce counts call by other words (review finding F318); the code is
         // the enhanced status code the receiving server answered with, 5.1.1.
         entries_.Columns.Add(Trimmed_(L("Address"), nameof(EntryView.Address), new DataGridLength(1, DataGridLengthUnitType.Star), 200));
         entries_.Columns.Add(Trimmed_(L("Reason"), nameof(EntryView.Why), DataGridLength.Auto, WidestCategory_()));
         entries_.Columns.Add(Fitted_(L("Status code"), nameof(EntryView.Status), 84));
         entries_.Columns.Add(Trimmed_(L("Source"), nameof(EntryView.Source), new DataGridLength(1, DataGridLengthUnitType.Star), 160));
         sinceColumn_ = Dated_(L("Since (server time)"), nameof(EntryView.Since), nameof(EntryView.SinceAt));
         expiresColumn_ = Dated_(L("Expires (server time)"), nameof(EntryView.Expires), nameof(EntryView.ExpiresAt));
         entries_.Columns.Add(sinceColumn_);
         entries_.Columns.Add(expiresColumn_);

         // The remote's own words, or the note, on the row's tip rather than in a
         // column: they are sentences, and a column of sentences takes the width
         // the address needs.
         var rowStyle = new Style(typeof(DataGridRow));

         if (Application.Current?.TryFindResource(typeof(DataGridRow)) is Style appRowStyle)
            rowStyle.BasedOn = appRowStyle;

         rowStyle.Setters.Add(new Setter(ToolTipProperty, new System.Windows.Data.Binding(nameof(EntryView.Detail))));
         entries_.RowStyle = rowStyle;

         counts_.Columns.Add(Trimmed_(L("Address"), nameof(CountView.Address), new DataGridLength(1, DataGridLengthUnitType.Star), 170));
         // Each count under its category's name in the list's Reason column, the name
         // on two lines where it is two words (Number_). The last status code, bounce
         // and delivery are the selected row's, in the strip under the card
         // (CountView.Strip): the grid fits its card at 1280 by 800 in English, German
         // and Japanese, where it scrolled 317, 559 and 332 pixels sideways.
         counts_.Columns.Add(Number_(CategoryText_("hard"), nameof(CountView.Hard)));
         counts_.Columns.Add(Number_(CategoryText_("soft"), nameof(CountView.Soft)));
         counts_.Columns.Add(Number_(CategoryText_("mailbox_full"), nameof(CountView.MailboxFull)));
         counts_.Columns.Add(Number_(CategoryText_("policy"), nameof(CountView.Policy)));
         counts_.Columns.Add(Number_(CategoryText_("other"), nameof(CountView.Other)));
         counts_.Columns.Add(Number_(L("In a row"), nameof(CountView.InARow)));
         counts_.Columns.Add(Fitted_(L("Listed"), nameof(CountView.Listed), GridStyles.WholeDevicePixels(GridStyles.CellWidth(L("Listed")))));
      }

      /// <summary>
      /// A column of dates: as wide as its dates over a floor of its header, which the rows
      /// raise to their widest when they are read (FloorAtCells_), sorted by the time itself,
      /// not by the words the reader's culture writes it in.
      /// </summary>
      private static DataGridTextColumn Dated_(string header, string path, string sortPath)
         => new() { Header = header, Binding = new System.Windows.Data.Binding(path), SortMemberPath = sortPath, Width = DataGridLength.Auto, MinWidth = Floor_(header, 0) };

      /// <summary>
      /// A column's floor raised to its widest cell, measured in the grid's font at the
      /// Windows text size, from its header's: given less than its columns ask for, a
      /// DataGrid takes the difference from an Auto column down to its floor, and a date
      /// wider than the floor was cut mid-glyph.
      /// </summary>
      private static void FloorAtCells_<T>(DataGridTextColumn column, IEnumerable<T> rows, Func<T, string> cell)
      {
         double floor = Floor_((string)column.Header, 0);
         foreach (T row in rows)
            floor = Math.Max(floor, GridStyles.CellWidth(cell(row)));
         column.MinWidth = GridStyles.WholeDevicePixels(floor);
      }

      /// <summary>The widest of the words the Why column says, measured in the grid's font at the Windows text size.</summary>
      private static double WidestCategory_()
      {
         double widest = 0;
         foreach (string category in new[] { "hard", "mailbox_full", "policy", "soft", "other", "" })
            widest = Math.Max(widest, GridStyles.CellWidth(CategoryText_(category)));
         return GridStyles.WholeDevicePixels(widest);
      }

      /// <summary>A column of names: a star's share of the width over a floor, each cut short with an ellipsis and whole in its tool tip.</summary>
      private static DataGridTextColumn Trimmed_(string header, string path, DataGridLength width, double minWidth)
         => GridStyles.Trimmed(new DataGridTextColumn { Header = header, Binding = new System.Windows.Data.Binding(path), Width = width, MinWidth = Floor_(header, minWidth) });

      /// <summary>A column of short codes: its cells' width over a floor of so many pixels, or its header's width where that is more.</summary>
      private static DataGridTextColumn Fitted_(string header, string path, double minWidth)
         => new() { Header = header, Binding = new System.Windows.Data.Binding(path), Width = DataGridLength.Auto, MinWidth = Floor_(header, minWidth) };

      /// <summary>A column's floor: so many pixels, or its header's width where that is more, so that no header of either grid is cut in any language - an Auto column's header is not always counted once the grid scrolls sideways.</summary>
      private static double Floor_(string header, double floor) => Math.Max(floor, GridStyles.HeaderWidth(header));

      /// <summary>
      /// A column of counts: 64 pixels, or the widest word of its header where that is
      /// more, the header wrapping onto a second line rather than widening the column
      /// for a count of a few digits; the digits and the header right-aligned.
      /// </summary>
      private static DataGridTextColumn Number_(string header, string path)
      {
         DataGridTextColumn column = Fitted_(header, path, 64);
         GridStyles.Number(column);
         double width = Math.Max(64, GridStyles.WidestHeaderWord(header));
         column.Width = width;
         column.MinWidth = width;
         GridStyles.WrappedHeader(column);
         return column;
      }

      private static Button MakeButton(string text, Wpf.Ui.Controls.ControlAppearance appearance, RoutedEventHandler onClick)
      {
         var button = new Wpf.Ui.Controls.Button
         {
            Content = text,
            Appearance = appearance,
            Margin = new Thickness(8, 0, 0, 0),
            MinWidth = 88
         };
         button.Click += onClick;
         return button;
      }
   }
   /// <summary>
   /// Puts one address on a domain's list by hand: the address, a note for whoever
   /// reads the list next, and an expiry. Its own class so that its Alt keys are
   /// its own scope and not the page's. The server's refusal - an address it hosts,
   /// an address that is not one - is shown under the address, in its words.
   /// </summary>
   internal static class AddSuppressionDialog
   {
      /// <summary>The address as added, or null when nothing was.</summary>
      public static string Show(Window owner, string domain)
      {
         var dialog = new FramedDialog { Owner = owner, Title = L("Add to the suppression list") };

         var address = new Wpf.Ui.Controls.TextBox();
         var note = new Wpf.Ui.Controls.TextBox();
         // As wide as four digits and the box's buttons, where it spanned the dialog's 432 pixels (review finding F318).
         var expires = new Wpf.Ui.Controls.NumberBox { Minimum = 0, Maximum = 3650, Value = 0, SmallChange = 1, LargeChange = 10, MaxDecimalPlaces = 0, Width = 160, HorizontalAlignment = HorizontalAlignment.Left };

         var body = new StackPanel();
         // Says what cannot be added, where "An address this server hosts is
         // refused" read as if the address would be refused mail.
         var addressRow = DialogFields.Field(L("A_ddress"), address, L("Addresses on this server's own domains cannot be added - the list applies only to mail leaving the server."));
         body.Children.Add(addressRow);
         body.Children.Add(DialogFields.Field(L("_Note"), note, L("Kept with the entry, for whoever reads the list next.")));
         body.Children.Add(DialogFields.Field(L("_Expires after (days)"), expires, L("0 keeps the entry until it is removed.")));

         string result = null;

         var ok = new Wpf.Ui.Controls.Button { Content = L("_Add"), Appearance = Wpf.Ui.Controls.ControlAppearance.Primary, MinWidth = 88 };
         ok.Click += (_, _) =>
         {
            DialogFields.ClearErrors(addressRow);

            string typed = address.Text?.Trim() ?? "";
            if (typed.Length == 0)
            {
               DialogFields.ShowError(addressRow, L("An address is required."));
               return;
            }

            try
            {
               bool added;
               dynamic utilities = ServerSession.Current.Application.Utilities;
               try
               {
                  added = (bool)utilities.AddSuppression(domain, typed, note.Text ?? "", (int)(expires.Value ?? 0));
               }
               finally
               {
                  ServerSession.Release((object)utilities);
               }

               if (!added)
               {
                  DialogFields.ShowError(addressRow, L("That address is already on this domain's list."));
                  return;
               }

               result = typed;
               dialog.DialogResult = true;
               dialog.Close();
            }
            catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
            {
               DialogFields.ShowError(addressRow, ServerSession.DescribeComError(ex));
            }
         };

         var cancel = new Wpf.Ui.Controls.Button { Content = L("Cancel"), MinWidth = 88 };
         cancel.Click += (_, _) => dialog.Close();

         dialog.Frame(F("Add to the suppression list of {0}", domain), body, ok, cancel,
            L("The next message from this domain to the address is refused, dropped or delivered and recorded, as the bounce settings say. Mail already queued to it meets the list on its next delivery attempt."),
            width: DesignTokens.Dialog.DefaultWidth);
         // A refusal of the address, typed or the server's, is said under its box
         // without moving Add and Cancel.
         dialog.KeepRoomForFieldErrors();

         return dialog.ShowDialog() == true ? result : null;
      }
   }
}
