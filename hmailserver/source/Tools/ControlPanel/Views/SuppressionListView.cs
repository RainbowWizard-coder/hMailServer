// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
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
      private readonly ComboBox domainChoice_ = new() { MinWidth = 220 };
      private readonly Toolbar toolbar_ = new() { SearchPlaceholder = L("Address contains") };

      private readonly DataGrid entries_ = new()
      {
         AutoGenerateColumns = false,
         IsReadOnly = true,
         CanUserAddRows = false,
         CanUserDeleteRows = false,
         SelectionMode = DataGridSelectionMode.Single,
         MinHeight = 160
      };

      private readonly DataGrid counts_ = new()
      {
         AutoGenerateColumns = false,
         IsReadOnly = true,
         CanUserAddRows = false,
         CanUserDeleteRows = false,
         SelectionMode = DataGridSelectionMode.Single,
         MinHeight = 120
      };

      private readonly InlineNotice status_ = new() { Visibility = Visibility.Collapsed };
      private readonly InlineNotice processingOff_ = new() { Visibility = Visibility.Collapsed };
      private readonly EmptyState empty_ = new() { Icon = Wpf.Ui.Controls.SymbolRegular.MailWarning24, Visibility = Visibility.Collapsed };

      private const int PageSize = 200;

      public SuppressionListView() => Build();

      // Read on entry rather than in the constructor, because the page cache keeps
      // this instance for the session: an entry removed over REST since the page
      // was first opened must not still be shown when somebody comes back to it.
      public void OnEnter()
      {
         string selected = domainChoice_.SelectedItem as string;
         LoadDomains_();

         if (selected != null && domainChoice_.Items.Contains(selected))
            domainChoice_.SelectedItem = selected;

         Reload();
      }

      public void OnLeave()
      {
      }

      private string Domain => domainChoice_.SelectedItem as string ?? "";

      private void Status_(StatusLevel level, string text)
      {
         status_.Level = level;
         status_.Text = text;
         status_.Visibility = Visibility.Visible;
      }

      private void LoadDomains_()
      {
         domainChoice_.Items.Clear();

         dynamic domains = null;

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
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            Status_(StatusLevel.Critical, ServerSession.DescribeComError(ex));
         }
         finally
         {
            ServerSession.Release((object)domains);
         }

         if (domainChoice_.Items.Count > 0 && domainChoice_.SelectedIndex < 0)
            domainChoice_.SelectedIndex = 0;
      }

      private void Reload()
      {
         status_.Visibility = Visibility.Collapsed;
         processingOff_.Visibility = Visibility.Collapsed;

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
               Status_(StatusLevel.Critical, F("The server's answer could not be read: {0}", list.Error ?? counts.Error));
               return;
            }

            entries_.ItemsSource = Present_(list.Entries);
            counts_.ItemsSource = PresentCounts_(counts.Counts);

            StatusText.Show(empty_, null, list.Total, null,
               L("No address is on this domain's suppression list. An address is put here when mail to it bounces for good, or when you add one."));

            if (!list.ProcessingEnabled)
            {
               processingOff_.Level = StatusLevel.Warning;
               processingOff_.Text = L("Bounce processing is off, so nothing is being counted or listed and the entries here are not applied. The switch is on the Delivery of e-mail page.");
               processingOff_.Visibility = Visibility.Visible;
            }

            if (list.Total > list.Entries.Count)
               Status_(StatusLevel.Information, F("{0} addresses listed, showing the {1} most recent. Search to narrow them.", list.Total, list.Entries.Count));
            else if (list.Total > 0)
               Status_(StatusLevel.Information, list.Total == 1 ? L("1 address listed.") : F("{0} addresses listed.", list.Total));
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            Status_(StatusLevel.Critical, ServerSession.DescribeComError(ex));
         }
      }

      /// <summary>An entry as the grid shows it: the server's words turned into the reader's.</summary>
      private sealed class EntryView
      {
         public string Address { get; init; }
         public string Why { get; init; }
         public string Status { get; init; }
         public string Source { get; init; }
         public string Since { get; init; }
         public string Expires { get; init; }
         public string Detail { get; init; }
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
         public string LastBounce { get; init; }
         public string LastDelivered { get; init; }
         public string Listed { get; init; }
      }

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
               Since = row.Created,
               Expires = row.Expires.Length == 0 ? L("Never") : row.Expired ? F("{0} (expired)", row.Expires) : row.Expires,
               Detail = row.RemoteMta.Length == 0 ? row.Diagnostic : row.RemoteMta + ": " + row.Diagnostic // no-loc: a host name and the words it answered with
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
               LastBounce = row.LastBounce,
               LastDelivered = row.LastDelivered.Length == 0 ? L("Never") : row.LastDelivered,
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

         // YesNo, No on Enter, like every other confirmation that changes what the
         // server does: the next message to this address will be sent.
         if (MessageBox.Show(
               F("Take {0} off the suppression list of {1}?\n\nThe next message to it will be sent, and it is listed again only after as many hard bounces in a row as the threshold asks for.", selected.Address, Domain),
               L("Remove from the suppression list"), MessageBoxButton.YesNo, MessageBoxImage.Question,
               MessageBoxResult.No)
             != MessageBoxResult.Yes)
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
         root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
         root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
         root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
         root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(2, GridUnitType.Star) });
         root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });

         root.Children.Add(new PageHeader
         {
            Title = L("Suppression list"),
            Subtitle = L("Addresses a domain no longer sends to because mail to them bounced for good, or because somebody put them here - with the counts behind each one. The list is per sending domain.")
         });

         AutomationProperties.SetName(domainChoice_, L("Domain"));
         domainChoice_.SelectionChanged += (_, _) => Reload();
         // Enter in the search box searches, and Refresh reads again with whatever
         // it holds: a search is a query to the server, not a filter over rows
         // already here, so it does not run on every keystroke.
         toolbar_.PreviewKeyDown += (_, e) => { if (e.Key == System.Windows.Input.Key.Enter) Reload(); };

         var filters = new StackPanel { Orientation = Orientation.Horizontal };
         filters.Children.Add(domainChoice_);
         toolbar_.Filters = filters;

         var actions = new StackPanel { Orientation = Orientation.Horizontal };
         actions.Children.Add(MakeButton(L("_Add…"), Wpf.Ui.Controls.ControlAppearance.Primary, (_, _) => AddEntry()));
         var remove = MakeButton(L("_Remove"), Wpf.Ui.Controls.ControlAppearance.Danger, (_, _) => RemoveSelected());
         actions.Children.Add(remove);
         SelectionGate.Bind(entries_, remove);
         actions.Children.Add(MakeButton(L("Re_fresh"), Wpf.Ui.Controls.ControlAppearance.Secondary, (_, _) => Reload()));
         toolbar_.Actions = actions;
         Grid.SetRow(toolbar_, 1);
         root.Children.Add(toolbar_);

         Grid.SetRow(processingOff_, 2);
         root.Children.Add(processingOff_);

         Grid.SetRow(status_, 3);
         root.Children.Add(status_);

         BuildColumns_();
         AutomationProperties.SetName(entries_, L("Suppression list"));
         AutomationProperties.SetName(counts_, L("Bounce counts"));

         var host = new Grid();
         host.Children.Add(entries_);
         host.Children.Add(empty_);
         var listCard = new Card { Padding = new Thickness(8), Content = host };
         Grid.SetRow(listCard, 4);
         root.Children.Add(listCard);

         var countsPanel = new DockPanel { Margin = new Thickness(0, DesignTokens.Space.Md, 0, 0) };
         var countsTitle = DialogFields.Caption(L("Bounce counts"));
         DockPanel.SetDock(countsTitle, Dock.Top);
         countsPanel.Children.Add(countsTitle);
         countsPanel.Children.Add(new Card { Padding = new Thickness(8), Content = counts_ });
         Grid.SetRow(countsPanel, 5);
         root.Children.Add(countsPanel);

         Content = root;
      }

      private void BuildColumns_()
      {
         entries_.Columns.Add(Text_(L("Address"), nameof(EntryView.Address), new DataGridLength(2, DataGridLengthUnitType.Star)));
         entries_.Columns.Add(Text_(L("Why"), nameof(EntryView.Why), DataGridLength.Auto));
         entries_.Columns.Add(Text_(L("Status"), nameof(EntryView.Status), DataGridLength.Auto));
         entries_.Columns.Add(Text_(L("Source"), nameof(EntryView.Source), DataGridLength.Auto));
         entries_.Columns.Add(Text_(L("Since"), nameof(EntryView.Since), DataGridLength.Auto));
         entries_.Columns.Add(Text_(L("Expires"), nameof(EntryView.Expires), DataGridLength.Auto));

         // The remote's own words, or the note, on the row's tip rather than in a
         // column: they are sentences, and a column of sentences takes the width
         // the address needs.
         var rowStyle = new Style(typeof(DataGridRow));

         if (Application.Current?.TryFindResource(typeof(DataGridRow)) is Style appRowStyle)
            rowStyle.BasedOn = appRowStyle;

         rowStyle.Setters.Add(new Setter(ToolTipProperty, new System.Windows.Data.Binding(nameof(EntryView.Detail))));
         entries_.RowStyle = rowStyle;

         counts_.Columns.Add(Text_(L("Address"), nameof(CountView.Address), new DataGridLength(2, DataGridLengthUnitType.Star)));
         counts_.Columns.Add(Number_(L("Hard"), nameof(CountView.Hard)));
         counts_.Columns.Add(Number_(L("Soft"), nameof(CountView.Soft)));
         counts_.Columns.Add(Number_(L("Mailbox full"), nameof(CountView.MailboxFull)));
         counts_.Columns.Add(Number_(L("Policy"), nameof(CountView.Policy)));
         counts_.Columns.Add(Number_(L("Other"), nameof(CountView.Other)));
         counts_.Columns.Add(Number_(L("In a row"), nameof(CountView.InARow)));
         counts_.Columns.Add(Text_(L("Last status"), nameof(CountView.LastStatus), DataGridLength.Auto));
         counts_.Columns.Add(Text_(L("Last bounce"), nameof(CountView.LastBounce), DataGridLength.Auto));
         counts_.Columns.Add(Text_(L("Last delivered"), nameof(CountView.LastDelivered), DataGridLength.Auto));
         counts_.Columns.Add(Text_(L("Listed"), nameof(CountView.Listed), DataGridLength.Auto));
      }

      private static DataGridTextColumn Text_(string header, string path, DataGridLength width)
         => new() { Header = header, Binding = new System.Windows.Data.Binding(path), Width = width };

      private static DataGridTextColumn Number_(string header, string path)
      {
         var column = new DataGridTextColumn { Header = header, Binding = new System.Windows.Data.Binding(path), Width = DataGridLength.Auto };
         GridStyles.Number(column);
         return column;
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
         AutomationProperties.SetName(address, L("Address to stop sending to"));
         var note = new Wpf.Ui.Controls.TextBox();
         AutomationProperties.SetName(note, L("Why it is listed"));
         var expires = new Wpf.Ui.Controls.NumberBox { Minimum = 0, Maximum = 3650, Value = 0, SmallChange = 1, LargeChange = 10, MaxDecimalPlaces = 0 };
         AutomationProperties.SetName(expires, L("Days until the entry expires"));

         var body = new StackPanel();
         var addressRow = DialogFields.Field(L("A_ddress"), address, L("An address this server hosts is refused: the list only applies to mail leaving the server."));
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
            width: 480);

         return dialog.ShowDialog() == true ? result : null;
      }
   }
}
