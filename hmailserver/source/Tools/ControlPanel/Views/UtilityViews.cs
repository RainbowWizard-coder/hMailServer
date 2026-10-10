// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Linq;
using System.Diagnostics;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using hMailServer.ControlPanel.Services;
using hMailServer.ControlPanel.Views.Scaffold;
using MessageBox = hMailServer.ControlPanel.Views.Dialogs;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Views
{
   /// <summary>Incoming relays: trusted upstream servers whose IPs are skipped in spam host checks.</summary>
   public class IncomingRelaysView : UserControl, IPageLifecycle, IRefreshablePage
   {
      // A grid with a column each for the name and the two ends of the range,
      // each row the relay's own, by its id (IncomingRelayRow), where it was a
      // list of strings - "Front-end spam filter   (203.0.113.40 - 203.0.113.41)",
      // 23 pixels tall, no headers, an outline for a selection - from which
      // Delete selected recovered the name by splitting at "   (" and then
      // deleted the first relay of that name (review finding F221). No
      // MinHeight of its own: its card is floored by the page's FillPanel.
      private readonly DataGrid relays_ = new()
      {
         AutoGenerateColumns = false,
         IsReadOnly = true,
         CanUserAddRows = false,
         CanUserDeleteRows = false,
         SelectionMode = DataGridSelectionMode.Single
      };

      private readonly Toolbar toolbar_ = new() { SearchPlaceholder = L("Name or address contains") };

      // How many relays there are, beside the title, as the list pages say it.
      private readonly StatusPill count_ = new() { Level = StatusLevel.Normal, Visibility = Visibility.Collapsed };

      // What Edit… or Delete selected did, or the server's refusal of it,
      // above the list: the two said nothing on success, and a refusal came in a
      // message box.
      private readonly InlineNotice outcome_ = new() { Visibility = Visibility.Collapsed };
      // The three boxes are captioned by their FieldRows now, so the caption is
      // the accessible name and the placeholder would only repeat it.
      private readonly Wpf.Ui.Controls.TextBox name_ = new();
      private readonly Wpf.Ui.Controls.TextBox lower_ = new();
      private readonly Wpf.Ui.Controls.TextBox upper_ = new();

      // The three boxes' rows, which say a value refused on its box (review
      // finding F212), on one line whose captions share one height.
      private readonly FieldRow nameRow_ = new FieldRow { Label = L("_Name"), Margin = new Thickness(0, 0, 8, 0), LabelGroup = "RelayLine" };
      private readonly FieldRow lowerRow_ = new FieldRow { Label = L("_Lower IP"), Margin = new Thickness(0, 0, 8, 0), LabelGroup = "RelayLine" };
      private readonly FieldRow upperRow_ = new FieldRow { Label = L("_Upper IP"), Margin = new Thickness(0, 0, 8, 0), LabelGroup = "RelayLine" };
      // Router24, not ArrowRouting24: that one's code point is past U+FFFF,
      // which Wpf.Ui's SymbolIcon draws as a g with a caron (see RoutesView.xaml).
      private readonly EmptyState empty_ = new() { Icon = Wpf.Ui.Controls.SymbolRegular.Router24, Visibility = Visibility.Collapsed };

      // A read that failed, above the list (Services/ReadFailure).
      private readonly InlineNotice notice_ = new() { Visibility = Visibility.Collapsed };

      private readonly Wpf.Ui.Controls.Button add_ = new()
      {
         Content = L("_Add"),
         Appearance = Wpf.Ui.Controls.ControlAppearance.Primary,
         VerticalAlignment = VerticalAlignment.Top,
         Margin = new Thickness(0)
      };

      public IncomingRelaysView()
      {
         var grid = new Grid();
         grid.SetResourceReference(MarginProperty, "AppPagePadding");
         grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
         grid.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });

         // Edit… and Delete selected act on the selected relay, in the toolbar
         // over the list as the list pages' verbs are; Delete selected was the
         // page header's only button, beside the title. Edit…, as the list
         // editor's pages say it, opens the relay's dialog.
         var edit = new Wpf.Ui.Controls.Button { Content = L("_Edit…"), Margin = new Thickness(0, 0, 8, 0) };
         System.Windows.Automation.AutomationProperties.SetAutomationId(edit, "relays-edit");
         edit.Click += (s, e) => EditSelected();
         var del = new Wpf.Ui.Controls.Button { Content = L("_Delete selected") };
         ButtonStyles.DangerText(del);
         System.Windows.Automation.AutomationProperties.SetAutomationId(del, "relays-delete");
         del.Click += (s, e) => DeleteSelected();
         var refresh = new Wpf.Ui.Controls.Button { Content = L("_Refresh") };
         System.Windows.Automation.AutomationProperties.SetAutomationId(refresh, "relays-refresh");
         refresh.Click += (s, e) => Reload();

         grid.Children.Add(new PageHeader
         {
            PageKey = "relays",
            Subtitle = L("Upstream gateways (spam filters, load balancers) whose IP addresses should not count as the connecting client in anti-spam host checks."),
            Status = count_,
            Actions = refresh
         });

         // The IP ranges shape: the toolbar, the notices, the list down to a
         // floor of three rows and the add form, in a scroll viewer; the list
         // was a star row of the page's grid.
         ScrollViewer scroll = FillPanel.ScrollingPage(out FillPanel parts);
         Grid.SetRow(scroll, 1);
         grid.Children.Add(scroll);

         var actions = new StackPanel { Orientation = Orientation.Horizontal };
         actions.Children.Add(edit);
         actions.Children.Add(del);
         toolbar_.Actions = actions;
         // Filtered as it is typed in, over the name and both addresses.
         toolbar_.SearchTextChanged += (s, e) => ListSearch.Apply(relays_, toolbar_.SearchText);
         parts.Children.Add(toolbar_);

         relays_.Columns.Add(GridStyles.Trimmed(new DataGridTextColumn
         {
            Header = L("Name"),
            Binding = new System.Windows.Data.Binding(nameof(IncomingRelayRow.Name)),
            Width = new DataGridLength(1, DataGridLengthUnitType.Star),
            MinWidth = Math.Max(160, GridStyles.HeaderWidth(L("Name")))
         }));
         relays_.Columns.Add(AddressColumn_(L("Lower IP"), nameof(IncomingRelayRow.LowerIP)));
         relays_.Columns.Add(AddressColumn_(L("Upper IP"), nameof(IncomingRelayRow.UpperIP)));
         System.Windows.Automation.AutomationProperties.SetName(relays_, L("Incoming relays"));

         // Edit… and Delete selected act on the selected relay; Delete selected
         // was a live red button with an empty list, the only one of its kind on
         // a list page.
         SelectionGate.Bind(relays_, edit, del);

         parts.Children.Add(notice_);
         parts.Children.Add(outcome_);

         // The grid's three rows on a FillPanel of their own and the card floored
         // at what it holds, so that the empty state laid over them raises the
         // card's floor where it needs more.
         var rows = new FillPanel();
         rows.Children.Add(FillPanel.Filling(relays_, FillPanel.ListRowsFloor));
         var listHost = new Grid();
         listHost.Children.Add(rows);
         listHost.Children.Add(empty_);
         EmptyStateUnderHeaders.Keep(empty_, relays_);
         var listCard = new Card { Padding = new Thickness(8), Content = listHost };
         listCard.SetResourceReference(MarginProperty, "AppCardGap");
         parts.Children.Add(FillPanel.Filling(listCard, 0));

         var row = new Grid();
         Grid.SetIsSharedSizeScope(row, true);
         for (int i = 0; i < 3; i++)
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
         row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });

         nameRow_.Content = name_;
         row.Children.Add(nameRow_);
         lowerRow_.Content = lower_;
         Grid.SetColumn(lowerRow_, 1);
         row.Children.Add(lowerRow_);
         upperRow_.Content = upper_;
         Grid.SetColumn(upperRow_, 2);
         row.Children.Add(upperRow_);

         // Add under a row as tall as the captions beside it, so that it stays
         // beside the boxes when a message appears under one.
         add_.Click += (s, e) => Add();
         var addLine = new Grid();
         addLine.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto, SharedSizeGroup = "RelayLine" });
         addLine.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
         Grid.SetRow(add_, 1);
         addLine.Children.Add(add_);
         Grid.SetColumn(addLine, 3);
         row.Children.Add(addLine);

         // Enter, F2 and a double-click open the selected relay's dialog,
         // Delete deletes it after asking, Insert goes to the form, and Enter in
         // the form adds; Escape empties it (Views/GridKeys, FormSubmit).
         GridKeys.Wire(relays_, GridKeys.Pressing(edit), GridKeys.Pressing(del), GridKeys.Focusing(name_));
         FormSubmit.Bind(row, add_, name_, lower_, upper_);

         var addCard = new Card { Title = L("Add relay"), Content = row };
         parts.Children.Add(addCard);

         Content = grid;
      }

      /// <summary>One end of a relay's range: 170 pixels, or its header's width where that is more.</summary>
      private static DataGridTextColumn AddressColumn_(string header, string path)
      {
         double width = Math.Max(170, GridStyles.HeaderWidth(header));
         return GridStyles.Trimmed(new DataGridTextColumn
         {
            Header = header,
            Binding = new System.Windows.Data.Binding(path),
            Width = new DataGridLength(width),
            MinWidth = width
         });
      }

      public void OnEnter() => Reload();

      void IRefreshablePage.Refresh() => Reload();

      public void OnLeave()
      {
      }

      private void Reload()
      {
         var rows = new List<IncomingRelayRow>();
         outcome_.Visibility = Visibility.Collapsed;
         try
         {
            dynamic relays = ServerSession.Current.Application.Settings.IncomingRelays;
            try
            {
               int count = (int)relays.Count;
               for (int i = 0; i < count; i++)
               {
                  dynamic relay = relays.Item[i];
                  rows.Add(new IncomingRelayRow
                  {
                     Id = (int)relay.ID,
                     Name = (string)relay.Name ?? "",
                     LowerIP = (string)relay.LowerIP ?? "",
                     UpperIP = (string)relay.UpperIP ?? ""
                  });
                  ServerSession.Release(relay);
               }
            }
            finally
            {
               ServerSession.Release(relays);
            }
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            // Neither the rows of the last read nor "No incoming relays." is a fact
            // this page has now: both go, the notice says what happened, with Try
            // again, and Add waits for a read that works (Services/ReadFailure).
            ReadFailure.Show(notice_, empty_, relays_, L("Could not read the incoming relays: "), ex, Reload, add_);
            count_.Visibility = Visibility.Collapsed;
            return;
         }

         ReadFailure.Clear(notice_, add_);
         relays_.ItemsSource = rows;
         ListSearch.Apply(relays_, toolbar_.SearchText);
         StatusText.Show(empty_, null, rows.Count, null,
            L("No incoming relays. Add one only for a gateway in front of this server whose address you trust."));
         count_.Text = Plural(rows.Count, "{0} incoming relay", "{0} incoming relays");
         count_.Visibility = Visibility.Visible;
      }

      /// <summary>What an act on a relay did, above the list, at its level.</summary>
      private void Say_(StatusLevel level, string text)
      {
         outcome_.Level = level;
         outcome_.Text = text;
         outcome_.Visibility = Visibility.Visible;
      }

      /// <summary>Selects the relay with this id after a read, and brings it into view.</summary>
      private void Select_(int id)
      {
         if (relays_.ItemsSource is not List<IncomingRelayRow> rows || rows.Find(r => r.Id == id) is not IncomingRelayRow row)
            return;

         relays_.SelectedItem = row;
         relays_.ScrollIntoView(row);
      }

      /// <summary>
      /// Edit…, Enter and a double-click: the relay's three fields in a
      /// dialog that saves them in place (IncomingRelayDialog), where a wrong
      /// address meant deleting the relay and adding it again.
      /// </summary>
      private void EditSelected()
      {
         if (relays_.SelectedItem is not IncomingRelayRow row)
            return;

         string saved = IncomingRelayDialog.Edit(Window.GetWindow(this), row.Id);
         if (saved == null)
            return;

         Reload();
         Select_(row.Id);
         Say_(StatusLevel.Good, ListOutcome.Saved(saved, DateTime.Now));
      }

      private void Add()
      {
         // Each value refused is said on its box, all on one press, with the
         // keyboard in the first, where one message box named three boxes and
         // "abc" to "xyz" went to the server as a range (review finding F212).
         string lower = lower_.Text;
         string upper = upper_.Text;
         DialogFields.ClearErrors(nameRow_, lowerRow_, upperRow_);
         if (name_.Text.Trim().Length == 0)
            nameRow_.Error = L("Enter a name for the relay.");
         if (!InlineFormChecks.IsAddress(lower))
            lowerRow_.Error = L("Enter the lower address of the range, such as 10.0.0.0.");
         if (!InlineFormChecks.IsAddress(upper))
            upperRow_.Error = L("Enter the upper address of the range, such as 10.0.0.255.");
         else if (InlineFormChecks.OrderOf(lower, upper) is InlineFormChecks.RangeOrder order && order != InlineFormChecks.RangeOrder.InOrder)
            upperRow_.Error = order == InlineFormChecks.RangeOrder.MixedFamilies
               ? L("The upper address must be of the same kind as the lower: both IPv4, or both IPv6.")
               : L("The upper address must not be below the lower.");
         if (DialogFields.ShowFirstError(nameRow_, lowerRow_, upperRow_))
            return;

         dynamic relays = ServerSession.Current.Application.Settings.IncomingRelays;
         try
         {
            dynamic relay = relays.Add();
            relay.Name = name_.Text.Trim();
            relay.LowerIP = lower_.Text.Trim();
            relay.UpperIP = upper_.Text.Trim();
            relay.Save();
            ServerSession.Release(relay);
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            MessageBox.Show(F("Could not add the relay: {0}", ServerSession.DescribeComError(ex)), L("Control Panel"));
            return;
         }
         finally
         {
            ServerSession.Release(relays);
         }

         name_.Text = lower_.Text = upper_.Text = "";
         Reload();
      }

      /// <summary>
      /// Deletes the selected relay by its id, after asking, and says so above the
      /// list with the row that took its place selected; the server's refusal is
      /// said there too. The name the row showed is never matched: two relays may
      /// share one, and the first of them was the one deleted.
      /// </summary>
      private void DeleteSelected()
      {
         if (relays_.SelectedItem is not IncomingRelayRow row)
            return;

         if (!MessageBox.Confirm(F("Delete the incoming relay {0}?", row.Name), L("Control Panel"), L("_Delete relay"), destructive: true))
            return;

         int at = relays_.SelectedIndex;
         dynamic relays = ServerSession.Current.Application.Settings.IncomingRelays;
         dynamic relay = null;
         try
         {
            // The relay itself, by its id, deletes itself - the path the page
            // always took, which holds the session to a server administrator's.
            relay = relays.ItemByDBID[row.Id];
            relay.Delete();
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            Say_(StatusLevel.Critical, F("Could not delete the relay: {0}", ServerSession.DescribeComError(ex)));
            return;
         }
         finally
         {
            ServerSession.Release((object)relay);
            ServerSession.Release(relays);
         }

         // The row that took its place among those shown: SelectedIndex counts
         // the rows the search leaves (ListSearch filters the grid's view), so
         // the next is found in the view as well, never in the whole list, where
         // the same index was a row the search hid and nothing was selected, or
         // another than the one that took the deleted row's place (found
         // re-checking 69a26ce17).
         Reload();
         int next = ListOutcome.IndexAfterDelete(relays_.Items.Count, at);
         if (next >= 0 && relays_.Items[next] is IncomingRelayRow taken)
            Select_(taken.Id);

         Say_(StatusLevel.Good, ListOutcome.Deleted(row.Name));
      }
   }

   /// <summary>MX query utility (same as the classic Utilities > MX-query).</summary>
   public class MxQueryView : UserControl, IPageLifecycle
   {
      // The caption names the field and the box shows an example, marked as one,
      // where the caption said "Domain (e.g. gmail.com)" (review finding F303). A
      // domain that publishes mail servers, not one of example.com's, which
      // publishes a null MX (RFC 7505) and so answers none.
      private readonly Wpf.Ui.Controls.TextBox domain_ = new() { PlaceholderText = F("e.g. {0}", "gmail.com") };

      // The answer at its level, over the servers it names (review finding F252):
      // a summary a notice says - found, none found, the query failed - and the
      // details as text, which is selectable and copied whole by Copy. The answer
      // was a read-only text box with an input's underline, which looked like a
      // field waiting to be typed in, and "Query failed" was plain text in it.
      private readonly InlineNotice summary_ = new() { Visibility = Visibility.Collapsed };
      private readonly TextBlock details_ = new()
      {
         FontFamily = new System.Windows.Media.FontFamily(Typography.MonoFontFamily),
         FontSize = Typography.Label,
         TextWrapping = TextWrapping.Wrap
      };
      private readonly LoadingState loading_ = new() { Visibility = Visibility.Collapsed };
      private readonly TextBlock hint_ = new()
      {
         Text = L("Type a domain and press Query. The answer lists the servers that receive that domain's mail, most preferred first, as this server's own resolver sees them."),
         TextWrapping = TextWrapping.Wrap,
         Margin = new Thickness(4, 2, 4, 0)
      };
      private readonly Wpf.Ui.Controls.Button run_ = new() { Content = L("_Query"), Appearance = Wpf.Ui.Controls.ControlAppearance.Primary };
      private readonly Wpf.Ui.Controls.Button copy_ = new() { Content = L("_Copy results"), Margin = new Thickness(8, 0, 0, 0), IsEnabled = false };

      public MxQueryView()
      {
         var grid = new Grid();
         grid.SetResourceReference(MarginProperty, "AppPagePadding");
         grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
         grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
         grid.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });

         grid.Children.Add(new PageHeader
         {
            PageKey = "mxquery"
         });

         var inputRow = new Grid();
         inputRow.SetResourceReference(MarginProperty, "AppCardGap");
         inputRow.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
         inputRow.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
         inputRow.Children.Add(new FieldRow
         {
            Label = L("_Domain"),
            Content = domain_,
            Margin = new Thickness(0, 0, 8, 0)
         });
         var actions = new StackPanel
         {
            Orientation = Orientation.Horizontal,
            VerticalAlignment = VerticalAlignment.Bottom,
            Margin = new Thickness(0)
         };
         run_.Click += async (s, e) => await RunQuery();
         actions.Children.Add(run_);
         // Copy has something to copy once there is an answer.
         copy_.Click += (s, e) => ClipboardWriter.TrySetText(CopyText_());   // best effort: the answer stays on screen to select
         actions.Children.Add(copy_);
         Grid.SetColumn(actions, 1);
         inputRow.Children.Add(actions);
         Grid.SetRow(inputRow, 1);
         grid.Children.Add(inputRow);
         // Enter in the domain box queries; Escape empties it.
         FormSubmit.Bind(inputRow, run_, domain_);

         hint_.SetResourceReference(FrameworkElement.StyleProperty, "TextSecondary");
         System.Windows.Automation.AutomationProperties.SetAutomationId(summary_, "mx-summary");
         var answer = new StackPanel();
         answer.Children.Add(summary_);
         answer.Children.Add(details_);
         var host = new Grid();
         host.Children.Add(new ScrollViewer { Content = answer, VerticalScrollBarVisibility = ScrollBarVisibility.Auto });
         host.Children.Add(hint_);
         host.Children.Add(loading_);
         var card = new Card { Padding = new Thickness(12), Content = host };
         Grid.SetRow(card, 2);
         grid.Children.Add(card);

         Content = grid;
      }

      private async Task RunQuery()
      {
         string domain = domain_.Text.Trim();
         if (domain.Length == 0)
            return;

         // One query at a time: Query waits while one runs, the loading state in
         // the answer's place.
         run_.IsEnabled = false;
         copy_.IsEnabled = false;
         hint_.Visibility = Visibility.Collapsed;
         summary_.Visibility = Visibility.Collapsed;
         details_.Text = "";
         loading_.Text = F("Querying MX records for {0}\u2026", domain);
         loading_.Visibility = Visibility.Visible;

         try
         {
            // Through the server, deliberately - issue #29. This tool used to shell
            // out to nslookup, which asks the OPERATING SYSTEM's resolver; the server
            // resolves through its own, honouring the DNSServer setting in
            // hMailServer.ini. The two disagree exactly when it matters - a custom
            // internal DNS server - and it is the server's answer that decides where
            // mail actually goes. Utilities.ResolveMXRecords runs the same resolver
            // path SMTP delivery uses.
            string result = await Task.Run(() =>
            {
               dynamic utilities = null;

               try
               {
                  utilities = ServerSession.Current.Application.Utilities;
                  return (string)utilities.ResolveMXRecords(domain);
               }
               finally
               {
                  ServerSession.Release((object)utilities);
               }
            });

            if (string.IsNullOrEmpty(result))
            {
               Say_(StatusLevel.Warning, F("The server found no mail servers for {0}, so mail sent to that domain from this server would not be deliverable right now.", domain));
               details_.Text = L("This is the server's own resolver answering - the same one SMTP delivery uses, including any 'Override DNS server' set on the DNS resolver page.");
               return;
            }

            string[] servers = result.Split(new[] { "\r\n" }, StringSplitOptions.RemoveEmptyEntries);
            var report = new System.Text.StringBuilder();
            report.AppendLine(F("Mail servers for {0}, in the order this server would try them:", domain));
            report.AppendLine();

            foreach (string line in servers)
            {
               int tab = line.IndexOf('\t');
               if (tab > 0)
                  report.AppendLine("  " + line.Substring(0, tab) + "  (" + line.Substring(tab + 1) + ")");
               else
                  report.AppendLine("  " + line);
            }

            report.AppendLine();
            report.Append(L("Resolved by the server itself, so an 'Override DNS server' set on the DNS resolver page is honoured - this is where mail actually goes, which an nslookup from this workstation cannot promise."));

            Say_(StatusLevel.Good, PluralF(servers.Length,
               "{0} mail server receives mail for {1}.",
               "{0} mail servers receive mail for {1}.",
               servers.Length, domain));
            details_.Text = report.ToString();
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            Say_(StatusLevel.Critical, F("Query failed: {0}", ServerSession.DescribeComError(ex)));
         }
         finally
         {
            loading_.Visibility = Visibility.Collapsed;
            run_.IsEnabled = true;
            copy_.IsEnabled = summary_.Visibility == Visibility.Visible;
         }
      }

      /// <summary>The answer's summary at its level.</summary>
      private void Say_(StatusLevel level, string text)
      {
         summary_.Level = level;
         summary_.Text = text;
         summary_.Visibility = Visibility.Visible;
      }

      /// <summary>What Copy takes: the summary, then the details.</summary>
      private string CopyText_()
         => string.IsNullOrEmpty(details_.Text) ? summary_.Text : summary_.Text + "\r\n\r\n" + details_.Text;

      public void OnEnter()
      {
      }

      public void OnLeave()
      {
      }
   }

   /// <summary>Server sendout: e-mail every account on the server (classic Utilities > Server sendout).</summary>
   public class SendoutView : UserControl, IPageLifecycle
   {
      private readonly Wpf.Ui.Controls.TextBox wildcard_ = new() { Text = "*" };
      private readonly Wpf.Ui.Controls.TextBox fromAddress_ = new() { PlaceholderText = F("e.g. {0}", "postmaster@example.com") };
      // The display name recipients see, not an account name - the hint used to
      // be "Administrator", which read as though the account name were a word
      // that translates. It is not: see the comment in ConnectView.xaml. What
      // the name is goes in the row's hint, under the box, where it was the
      // box's placeholder and went once a name was typed (review finding F303).
      private readonly Wpf.Ui.Controls.TextBox fromName_ = new();
      // No placeholder: the field's caption is the same word, and a placeholder
      // that repeats the caption is a second copy of it inside the editor.
      private readonly Wpf.Ui.Controls.TextBox subject_ = new();
      private readonly TextBox body_ = new()
      {
         AcceptsReturn = true,
         Height = 140,
         TextWrapping = TextWrapping.Wrap,
         VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
         FontSize = Typography.Body,
         Padding = new Thickness(6)
      };
      private readonly InlineNotice status_ = new() { Visibility = Visibility.Collapsed };

      // The message's row, which says on the field that an empty message is not
      // sent; the sender's and the subject's, which say an empty box on the box
      // (review finding F212).
      private readonly FieldRow bodyRow_ = new FieldRow { Label = L("_Message") };
      private readonly FieldRow fromRow_ = new FieldRow { Label = L("_From address") };
      private readonly FieldRow subjectRow_ = new FieldRow { Label = L("Su_bject") };

      public SendoutView()
      {
         var panel = new StackPanel { MaxWidth = 680, HorizontalAlignment = HorizontalAlignment.Left };

         panel.Children.Add(new PageHeader
         {
            PageKey = "sendout"
         });

         var form = new StackPanel();

         form.Children.Add(new FieldRow { Label = L("_Recipient wildcard (* = everyone)"), Content = wildcard_ });
         fromRow_.Content = fromAddress_;
         form.Children.Add(fromRow_);
         form.Children.Add(new FieldRow { Label = L("From _name"), Content = fromName_, Hint = L("Sender name, as recipients see it") });
         subjectRow_.Content = subject_;
         form.Children.Add(subjectRow_);
         body_.SetResourceReference(Control.ForegroundProperty, "TextFillColorPrimaryBrush");
         body_.Background = System.Windows.Media.Brushes.Transparent;
         bodyRow_.Content = body_;
         form.Children.Add(bodyRow_);

         var send = new Wpf.Ui.Controls.Button
         {
            Content = L("_Send to all matching accounts"),
            Appearance = Wpf.Ui.Controls.ControlAppearance.Primary,
            Margin = new Thickness(0, 4, 0, 0)
         };
         send.Click += (s, e) => Send();
         form.Children.Add(send);
         status_.Margin = new Thickness(0, 12, 0, 0);
         form.Children.Add(status_);

         panel.Children.Add(new Card { Content = form });

         var scroller = new ScrollViewer { Content = panel, VerticalScrollBarVisibility = ScrollBarVisibility.Auto };
         scroller.SetResourceReference(PaddingProperty, "AppPagePadding");
         Content = scroller;
      }

      private void Send()
      {
         // Each press starts with no outcome on the page: a warning that the wildcard
         // reached nobody, or the last sendout's result, would otherwise stand under a
         // form that may no longer say it, through a question cancelled - and so does
         // the message field's refusal, which a press that stopped at the From address
         // or the subject left beside the message typed since.
         status_.Visibility = Visibility.Collapsed;
         bodyRow_.Error = null;

         // Each box refused is said on that box, all on one press, with the
         // keyboard in the first, where one message box named the sender and the
         // subject (review finding F212). An empty message is refused on its field:
         // it would reach every matching account with nothing in it (review
         // finding F184).
         string from = fromAddress_.Text.Trim();
         DialogFields.ClearErrors(fromRow_, subjectRow_, bodyRow_);
         int at = from.IndexOf('@');
         if (at <= 0 || at == from.Length - 1 || from.Any(char.IsWhiteSpace))
            fromRow_.Error = L("Enter the address the message comes from, such as postmaster@example.com.");
         if (subject_.Text.Trim().Length == 0)
            subjectRow_.Error = L("Enter a subject.");
         if (!SendoutAudience.HasBody(body_.Text))
            bodyRow_.Error = L("Write the message first: an empty one is not sent.");
         if (DialogFields.ShowFirstError(fromRow_, subjectRow_, bodyRow_))
            return;

         // The question says how many accounts the message reaches, counted as the
         // server counts them, and asks nothing where it would reach none.
         string wildcard = wildcard_.Text;
         int? recipients;
         System.Windows.Input.Mouse.OverrideCursor = System.Windows.Input.Cursors.Wait;
         try
         {
            // A read of every account over COM: a large server takes a moment.
            recipients = CountRecipients_(wildcard);
         }
         finally
         {
            System.Windows.Input.Mouse.OverrideCursor = null;
         }
         if (recipients == 0)
         {
            status_.Level = StatusLevel.Warning;
            status_.Text = Confirmations.SendoutToNobody(wildcard);
            status_.Visibility = Visibility.Visible;
            return;
         }

         if (!MessageBox.Confirm(Confirmations.Sendout(subject_.Text.Trim(), recipients, wildcard), L("Server sendout"), L("_Send"), cancelByDefault: true))
            return;

         try
         {
            dynamic utilities = ServerSession.Current.Application.Utilities;
            bool queued = (bool)utilities.EmailAllAccounts(wildcard, fromAddress_.Text.Trim(), fromName_.Text.Trim(),
               subject_.Text, body_.Text);
            ServerSession.Release(utilities);
            // The server reports failure through the return value rather than an
            // error, so don't claim success when it declined the sendout.
            status_.Level = queued ? StatusLevel.Good : StatusLevel.Critical;
            status_.Text = queued
               ? F("Sendout queued {0}.", DateTime.Now.ToLongTimeString())
               : L("The server did not queue the sendout. Check the address wildcard and the hMailServer error log.");
            status_.Visibility = Visibility.Visible;
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            MessageBox.Show(F("Sendout failed: {0}", ServerSession.DescribeComError(ex)), L("Control Panel"));
         }
      }

      /// <summary>
      /// How many active accounts of active domains the wildcard matches - the
      /// accounts EmailAllUsers::Start sends to (Services/SendoutAudience) - or null
      /// when they could not be counted, which the question then says nothing of.
      /// </summary>
      private static int? CountRecipients_(string wildcard)
      {
         dynamic domains = null;
         try
         {
            domains = ServerSession.Current.Application.Domains;
            int total = 0;
            int domainCount = (int)domains.Count;
            for (int i = 0; i < domainCount; i++)
            {
               dynamic domain = domains.Item[i];
               dynamic accounts = null;
               try
               {
                  if (!(bool)domain.Active)
                     continue;

                  accounts = domain.Accounts;
                  int accountCount = (int)accounts.Count;
                  for (int j = 0; j < accountCount; j++)
                  {
                     dynamic account = accounts.Item[j];
                     try
                     {
                        if (SendoutAudience.Reaches(wildcard, (string)account.Address, (bool)account.Active, true))
                           total++;
                     }
                     finally
                     {
                        ServerSession.Release((object)account);
                     }
                  }
               }
               finally
               {
                  ServerSession.Release((object)accounts);
                  ServerSession.Release((object)domain);
               }
            }

            return total;
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            return null;
         }
         finally
         {
            ServerSession.Release((object)domains);
         }
      }

      public void OnEnter()
      {
      }

      public void OnLeave()
      {
      }
   }

   /// <summary>Server diagnostics (classic Utilities > Diagnostics).</summary>
   public class DiagnosticsView : UserControl, IPageLifecycle
   {
      private readonly Wpf.Ui.Controls.TextBox localDomain_ = new() { PlaceholderText = L("A domain hosted on this server") };
      private readonly Wpf.Ui.Controls.TextBox testDomain_ = new() { Text = "gmail.com" };

      // The checks' results as rows, each its pill (Passed, Failed), its name and
      // what it found, under a summary at the level of the worst (review finding
      // F252). They were "[ OK ]" and "[FAIL]" lines in a read-only text box with
      // an input's underline, which looked like an empty field, with no summary,
      // and "Diagnostics failed" landed in the same box.
      private readonly InlineNotice summary_ = new() { Visibility = Visibility.Collapsed };
      private readonly StackPanel rows_ = new();
      private readonly LoadingState loading_ = new() { Text = L("Running diagnostics…"), Visibility = Visibility.Collapsed };
      private readonly TextBlock hint_ = new()
      {
         Text = L("Nothing has been run yet. Press Run diagnostics and the result of each check appears here, with what to do about any that fails."),
         TextWrapping = TextWrapping.Wrap,
         Margin = new Thickness(4, 2, 4, 0)
      };
      private readonly Wpf.Ui.Controls.Button run_ = new() { Content = L("Run _diagnostics"), Appearance = Wpf.Ui.Controls.ControlAppearance.Primary };
      private readonly Wpf.Ui.Controls.Button copy_ = new() { Content = L("_Copy results"), Margin = new Thickness(8, 0, 0, 0), IsEnabled = false };

      // What Copy takes: the summary and each check as text, in the reader's language.
      private string copyText_ = "";

      // Message-store consistency. The server has no COM surface for this scan -
      // it is a background task gated on hMailServer.INI MessageStoreConsistencyCheck
      // that rewrites a recovery report in the log folder on every run - so the
      // Control Panel shows the result by reading that report.
      private readonly InlineNotice consistencyStatus_ = new();
      // A DataGrid rather than a ListView with a GridView: the application's
      // implicit DataGrid styles theme it, and nothing themes a GridView, whose
      // white header and system-hyperlink text were unreadable on the dark theme.
      private readonly DataGrid consistencyList_ = new()
      {
         AutoGenerateColumns = false,
         IsReadOnly = true,
         SelectionMode = DataGridSelectionMode.Single,
         // A badly damaged store can list thousands of messages; cap the section
         // so it scrolls internally instead of pushing the page around.
         MaxHeight = 260,
         Visibility = Visibility.Collapsed
      };
      private readonly Wpf.Ui.Controls.Button consistencyRefresh_ = new() { Content = L("_Refresh") };
      private readonly Wpf.Ui.Controls.Button consistencyOpen_ = new()
      {
         Content = L("_Open report"),
         Margin = new Thickness(8, 0, 0, 0),
         IsEnabled = false
      };
      private string reportPath_;
      private bool loadingReport_;

      public DiagnosticsView()
      {
         var grid = new Grid();
         grid.SetResourceReference(MarginProperty, "AppPagePadding");
         grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
         grid.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });

         grid.Children.Add(new PageHeader
         {
            PageKey = "diagnostics",
            Subtitle = L("Runs the server's built-in connectivity and configuration checks (outbound port 25, MX resolution, backup directory, IP configuration). The message-store consistency scan below is a separate read-only background task - the server runs it at start-up and hourly and records what it found in a recovery report.")
         });

         // The IP ranges shape: the results card takes the height the inputs
         // above it and the consistency card under it leave in the window, down
         // to a floor, and below that the page scrolls; the consistency card is
         // only as tall as what it has to say - a clean scan is one line. The
         // results card was a grid's star row between rows sized to their
         // content, which in the narrowest window (760 by 520) gave it no height
         // and cut the consistency footer off.
         ScrollViewer scroll = FillPanel.ScrollingPage(out FillPanel parts);
         Grid.SetRow(scroll, 1);
         grid.Children.Add(scroll);

         var inputRow = new Grid();
         inputRow.SetResourceReference(MarginProperty, "AppCardGap");
         inputRow.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
         inputRow.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
         inputRow.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
         localDomain_.Margin = new Thickness(0, 0, 8, 0);
         // Two unlabelled boxes read as one question; a caption over each says
         // which domain is which. The captions double as the accessible names.
         inputRow.Children.Add(Captioned(L("_Hosted domain"), localDomain_));
         testDomain_.Margin = new Thickness(0, 0, 8, 0);
         Grid.SetColumn(testDomain_, 1);
         inputRow.Children.Add(Captioned(L("R_emote domain"), testDomain_));
         // Bottom-aligned, level with the two captioned inputs' boxes.
         var actions = new StackPanel { Orientation = Orientation.Horizontal, VerticalAlignment = VerticalAlignment.Bottom };
         run_.Click += async (s, e) => await Run();
         actions.Children.Add(run_);
         // Copy has something to copy once a run has answered.
         copy_.Click += (s, e) => ClipboardWriter.TrySetText(copyText_);   // best effort: the results stay on screen to select
         actions.Children.Add(copy_);
         Grid.SetColumn(actions, 2);
         inputRow.Children.Add(actions);
         parts.Children.Add(inputRow);
         // Enter in either domain box runs the checks.
         FormSubmit.Bind(inputRow, run_);

         hint_.SetResourceReference(FrameworkElement.StyleProperty, "TextSecondary");
         System.Windows.Automation.AutomationProperties.SetAutomationId(summary_, "diagnostics-summary");
         System.Windows.Automation.AutomationProperties.SetName(rows_, L("Diagnostics"));
         Grid.SetIsSharedSizeScope(rows_, true);
         var results = new StackPanel();
         results.Children.Add(summary_);
         results.Children.Add(rows_);
         var host = new Grid();
         host.Children.Add(new ScrollViewer { Content = results, VerticalScrollBarVisibility = ScrollBarVisibility.Auto });
         host.Children.Add(hint_);
         host.Children.Add(loading_);
         var card = new Card
         {
            Padding = new Thickness(12),
            Content = host
         };
         card.SetResourceReference(MarginProperty, "AppCardGap");
         parts.Children.Add(FillPanel.Filling(card, FillPanel.ListCardFloor));

         parts.Children.Add(BuildConsistencySection());

         Content = grid;
      }

      private Card BuildConsistencySection()
      {
         var section = new Grid();
         section.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
         section.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });

         consistencyStatus_.Margin = new Thickness(0);
         section.Children.Add(consistencyStatus_);

         var consistencyActions = new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Right };
         consistencyRefresh_.Click += async (s, e) => await LoadConsistencyReport();
         consistencyActions.Children.Add(consistencyRefresh_);
         consistencyOpen_.Click += (s, e) => OpenReport();
         consistencyActions.Children.Add(consistencyOpen_);

         consistencyList_.Columns.Add(new DataGridTextColumn
         {
            Header = L("Message ID"),
            Binding = new System.Windows.Data.Binding(nameof(Services.MessageStoreConsistencyEntry.MessageId)),
            Width = 110
         });
         consistencyList_.Columns.Add(new DataGridTextColumn
         {
            Header = L("Account"),
            Binding = new System.Windows.Data.Binding(nameof(Services.MessageStoreConsistencyEntry.Account)),
            Width = 220
         });
         consistencyList_.Columns.Add(new DataGridTextColumn
         {
            Header = L("Expected file"),
            Binding = new System.Windows.Data.Binding(nameof(Services.MessageStoreConsistencyEntry.ExpectedPath)),
            Width = new DataGridLength(1, DataGridLengthUnitType.Star)
         });
         Grid.SetRow(consistencyList_, 1);
         System.Windows.Automation.AutomationProperties.SetName(consistencyList_, L("Message-store consistency"));
         section.Children.Add(consistencyList_);

         return new Card
         {
            Title = L("Message-store consistency"),
            Content = section,
            Footer = consistencyActions
         };
      }

      /// <summary>One captioned input, as the form row the design system draws.</summary>
      private static FieldRow Captioned(string caption, FrameworkElement input)
      {
         // The row takes the input's place in its grid: its column, and its margin.
         var field = new FieldRow { Label = caption, Content = input, Margin = input.Margin };
         Grid.SetColumn(field, Grid.GetColumn(input));
         Grid.SetRow(field, Grid.GetRow(input));
         input.Margin = new Thickness(0);
         return field;
      }

      public void OnEnter()
      {
         // The report is a local file, so read it off the UI thread the same way
         // the diagnostics run does; nothing else on the page depends on it.
         _ = LoadConsistencyReport();

         // Suggest the first hosted domain as the local domain.
         if (localDomain_.Text.Length > 0)
            return;
         try
         {
            dynamic domains = ServerSession.Current.Application.Domains;
            if ((int)domains.Count > 0)
            {
               dynamic first = domains.Item[0];
               localDomain_.Text = (string)first.Name;
               ServerSession.Release(first);
            }
            ServerSession.Release(domains);
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            // Deliberately ignored: best effort only, and the outcome of the surrounding operation does not depend on this succeeding.
         }
      }

      public void OnLeave()
      {
      }

      /// <summary>One check's outcome as the server answered it.</summary>
      private sealed class CheckResult
      {
         public string Name = "";
         public bool? Passed;
         public string Details = "";
      }

      private async Task Run()
      {
         // One run at a time: Run waits while one is in progress, the loading state
         // in the results' place, where a second click ran the checks twice.
         run_.IsEnabled = false;
         copy_.IsEnabled = false;
         hint_.Visibility = Visibility.Collapsed;
         summary_.Visibility = Visibility.Collapsed;
         rows_.Children.Clear();
         copyText_ = "";
         loading_.Visibility = Visibility.Visible;

         string local = localDomain_.Text.Trim();
         string remote = testDomain_.Text.Trim();
         try
         {
            List<CheckResult> checks = await Task.Run(() => PerformTests_(local, remote));
            ShowResults_(checks);
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            Say_(StatusLevel.Critical, F("Diagnostics failed: {0}", ServerSession.DescribeComError(ex)));
            copyText_ = summary_.Text;
         }
         finally
         {
            loading_.Visibility = Visibility.Collapsed;
            run_.IsEnabled = true;
            copy_.IsEnabled = copyText_.Length > 0;
         }
      }

      /// <summary>The server's checks, run off the UI thread; throws what the server throws.</summary>
      private static List<CheckResult> PerformTests_(string localDomain, string remoteDomain)
      {
         dynamic diagnostics = null;
         dynamic results = null;
         try
         {
            diagnostics = ServerSession.Current.Application.Diagnostics;
            diagnostics.LocalDomainName = localDomain;
            diagnostics.TestDomainName = remoteDomain;
            results = diagnostics.PerformTests();

            var checks = new List<CheckResult>();
            int count = (int)results.Count;
            for (int i = 0; i < count; i++)
            {
               dynamic result = results.Item[i];
               var check = new CheckResult();
               try { check.Name = (string)result.Name; } catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck)) { /* Deliberately ignored: best effort only, and the outcome of the surrounding operation does not depend on this succeeding. */ }
               try { check.Passed = (bool)result.Result; } catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck)) { /* Deliberately ignored: best effort only, and the outcome of the surrounding operation does not depend on this succeeding. */ }
               try { check.Details = (string)result.ExecutionDetails ?? ""; } catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck)) { /* Deliberately ignored: best effort only, and the outcome of the surrounding operation does not depend on this succeeding. */ }
               checks.Add(check);
               ServerSession.Release(result);
            }

            return checks;
         }
         finally
         {
            ServerSession.Release((object)results);
            ServerSession.Release((object)diagnostics);
         }
      }

      /// <summary>
      /// The summary at the level of the worst check - Good when every one passed,
      /// Critical when one failed, naming it, the level of its row's pill (LevelOf_) -
      /// and a row per check; and the same as text for Copy.
      /// </summary>
      private void ShowResults_(List<CheckResult> checks)
      {
         if (checks.Count == 0)
         {
            Say_(StatusLevel.Information, L("No diagnostic results returned."));
            copyText_ = summary_.Text;
            return;
         }

         int passed = checks.Count(c => c.Passed == true);
         List<string> failed = checks.Where(c => c.Passed == false).Select(c => c.Name).ToList();
         if (failed.Count > 0)
            Say_(LevelOf_(false), F("{0} of {1} checks passed. Failed: {2}.", passed, checks.Count, string.Join(", ", failed)));
         else if (passed < checks.Count)
            Say_(StatusLevel.Information, F("{0} of {1} checks passed; the server gave no result for the others.", passed, checks.Count));
         else
            Say_(StatusLevel.Good, checks.Count == 1 ? L("The check passed.") : Plural(checks.Count, "All {0} check passed.", "All {0} checks passed."));

         var copy = new System.Text.StringBuilder(summary_.Text);
         copy.AppendLine();
         foreach (CheckResult check in checks)
         {
            rows_.Children.Add(CheckRow_(check));
            copy.AppendLine();
            copy.AppendLine(CheckWord_(check) + ": " + check.Name);
            if (!string.IsNullOrWhiteSpace(check.Details))
               copy.AppendLine("   " + check.Details.Trim().Replace("\r\n", "\r\n   "));
         }

         copyText_ = copy.ToString().TrimEnd();
      }

      /// <summary>The run's summary at its level.</summary>
      private void Say_(StatusLevel level, string text)
      {
         summary_.Level = level;
         summary_.Text = text;
         summary_.Visibility = Visibility.Visible;
      }

      private static string CheckWord_(CheckResult check)
         => check.Passed == true ? L("Passed") : check.Passed == false ? L("Did not pass") : L("Unknown");

      /// <summary>
      /// A check's level: a check that did not pass is a failure, Critical, in its row's
      /// pill and in the summary over the rows alike (the design system's "Critical: a
      /// failure"), where the summary said Warning over a red pill.
      /// </summary>
      private static StatusLevel LevelOf_(bool? passed)
         => passed == true ? StatusLevel.Good : passed == false ? StatusLevel.Critical : StatusLevel.Normal;

      /// <summary>One check: its pill and name, what the server found under them.</summary>
      private static FrameworkElement CheckRow_(CheckResult check)
      {
         var pill = new StatusPill
         {
            Level = LevelOf_(check.Passed),
            Text = CheckWord_(check),
            HorizontalAlignment = HorizontalAlignment.Left,
            VerticalAlignment = VerticalAlignment.Center
         };
         var name = new TextBlock
         {
            Text = check.Name,
            TextWrapping = TextWrapping.Wrap,
            VerticalAlignment = VerticalAlignment.Center,
            Margin = new Thickness(DesignTokens.Space.Sm, 0, 0, 0)
         };
         name.SetResourceReference(FrameworkElement.StyleProperty, "TextBodyStrong");

         // The pills share a column, so that every name starts at one edge.
         var head = new Grid();
         head.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto, SharedSizeGroup = "DiagnosticsPill" });
         head.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
         Grid.SetColumn(name, 1);
         head.Children.Add(pill);
         head.Children.Add(name);

         var row = new StackPanel { Margin = new Thickness(0, DesignTokens.Space.Md, 0, 0) };
         row.Children.Add(head);
         if (!string.IsNullOrWhiteSpace(check.Details))
         {
            var details = new TextBlock
            {
               Text = check.Details.Trim(),
               TextWrapping = TextWrapping.Wrap,
               FontFamily = new System.Windows.Media.FontFamily(Typography.MonoFontFamily),
               FontSize = Typography.Label,
               Margin = new Thickness(0, DesignTokens.Space.Xs, 0, 0)
            };
            details.SetResourceReference(TextBlock.ForegroundProperty, "TextFillColorSecondaryBrush");
            row.Children.Add(details);
         }

         return row;
      }

      /// <summary>
      /// What the last consistency scan found, ready for the UI thread. The
      /// server never returns this over COM, so everything here comes from
      /// hMailServer.INI and the recovery report the scan writes.
      /// </summary>
      private sealed class ConsistencyResult
      {
         public string Message;
         public Severity Level;
         public string ReportPath;

         // While the periodic check is off, the page that turns it on.
         public bool OfferSettings;
         public IReadOnlyList<Services.MessageStoreConsistencyEntry> Entries;
      }

      private enum Severity
      {
         Neutral,
         Good,
         Bad,

         // A clean report of a check that is on but has not scanned for longer
         // than its hourly run explains.
         Stale
      }

      private async Task LoadConsistencyReport()
      {
         // OnEnter starts a load without awaiting it, so guard against a second
         // one overlapping when the page is re-entered while the first is
         // running.
         if (loadingReport_)
            return;
         loadingReport_ = true;

         consistencyRefresh_.IsEnabled = false;
         consistencyOpen_.IsEnabled = false;
         consistencyList_.ItemsSource = null;
         consistencyList_.Visibility = Visibility.Collapsed;
         consistencyStatus_.Level = StatusLevel.Information;
         consistencyStatus_.Text = L("Reading the recovery report…");
         reportPath_ = null;

         try
         {
            ConsistencyResult result = await Task.Run(ReadConsistencyReport);

            reportPath_ = result.ReportPath;
            consistencyStatus_.Text = result.Message;
            consistencyStatus_.Level = result.Level switch
            {
               Severity.Good => StatusLevel.Good,
               Severity.Bad => StatusLevel.Critical,
               Severity.Stale => StatusLevel.Warning,
               _ => StatusLevel.Information
            };
            consistencyStatus_.Content = result.OfferSettings ? OpenHardening_() : null;

            if (result.Entries != null && result.Entries.Count > 0)
            {
               consistencyList_.ItemsSource = result.Entries;
               consistencyList_.Visibility = Visibility.Visible;
            }
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            // Nothing awaits the load started from OnEnter, so report the failure
            // on the page rather than losing it in an unobserved task.
            consistencyStatus_.Level = StatusLevel.Critical;
            consistencyStatus_.Text = F("Could not read the consistency report: {0}", ex.Message);
         }
         finally
         {
            consistencyOpen_.IsEnabled = reportPath_ != null;
            consistencyRefresh_.IsEnabled = true;
            loadingReport_ = false;
         }
      }

      private static ConsistencyResult ReadConsistencyReport()
      {
         var store = new IniFeatureStore();
         if (!store.IsAvailable)
         {
            return new ConsistencyResult
            {
               Level = Severity.Neutral,
               Message = L("The scan result is only readable on the server machine - hMailServer.ini was not found here. The server publishes the same number as the hmailserver_messagestore_missing_files metric.")
            };
         }

         // hMailServer.INI is the administrators' alone from 6.3.6: without it the
         // log folder would read as not configured.
         string denied = store.ReadDenied();
         if (denied != null)
         {
            return new ConsistencyResult
            {
               Level = Severity.Neutral,
               Message = F("Could not read the file: {0}", denied)
            };
         }

         bool enabled = store.ReadBool("MessageStoreConsistencyCheck", false);
         string logFolder = store.GetLogFolder();

         if (string.IsNullOrWhiteSpace(logFolder))
         {
            return new ConsistencyResult
            {
               Level = Severity.Neutral,
               Message = L("No log folder is configured in hMailServer.ini, so the server has nowhere to write the recovery report.")
            };
         }

         string path = System.IO.Path.Join(logFolder, Services.MessageStoreConsistencyReport.FileName);
         string enabledNote = enabled
            ? null
            // Asked for by nav key rather than spelled out, so that renaming the
            // page cannot leave this pointing at a page title that no longer exists.
            : F("The periodic check is currently switched off ('Periodically cross-check message rows against files on disk' on the {0} page), so this will not be refreshed.", L(Services.NavigationMap.TitleOf("hardening")));

         string text;
         try
         {
            if (!System.IO.File.Exists(path))
            {
               return new ConsistencyResult
               {
                  Level = Severity.Neutral,
                  Message = enabled
                     ? F("The consistency check is enabled but has not written a report yet. The server scans at start-up and then hourly, and writes {0}.", path)
                     : F("The consistency check is switched off, so no scan has run. Turn on 'Periodically cross-check message rows against files on disk' on the {0} page; the server then scans at start-up and hourly and writes {1}.", L(Services.NavigationMap.TitleOf("hardening")), path)
               };
            }

            using var stream = new System.IO.FileStream(path, System.IO.FileMode.Open, System.IO.FileAccess.Read,
               System.IO.FileShare.ReadWrite | System.IO.FileShare.Delete);
            using var reader = new System.IO.StreamReader(stream);
            text = reader.ReadToEnd();
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            return new ConsistencyResult
            {
               Level = Severity.Neutral,
               ReportPath = path,
               Message = F("Could not read {0}: {1}", path, ex.Message)
            };
         }

         Services.MessageStoreConsistencyReport report = Services.MessageStoreConsistencyReport.Parse(text);
         string when = string.IsNullOrEmpty(report.Generated) ? null : F("Last scan: {0}.", report.Generated);
         int? reportedMissing = report.ReportedMissingCount;
         string truncated = report.IsTruncated && reportedMissing.HasValue
            ? F("The report header says {0} but lists {1} - it was probably read while the server was rewriting it, so refresh.", reportedMissing.Value, report.Entries.Count)
            : null;

         // Good only for a scan that says how the store is now: the check on, and
         // the scan under two hours old. A report thirteen days old, or one the
         // switched-off check will never rewrite, read as a green "No problems
         // found" (review finding F252); it says how old it is instead.
         TimeSpan? age = Services.MessageStoreConsistencyReport.AgeOf(report.Generated, DateTime.Now);
         bool fresh = enabled && Services.MessageStoreConsistencyReport.IsFresh(age);
         if (!fresh && age is TimeSpan old)
         {
            when = Sentences(when, enabled
               ? F("That scan was {0}, although the check runs hourly: the server has not scanned since, so the store may have changed.", AgeWords.Ago(old))
               : F("That scan was {0}.", AgeWords.Ago(old)));
         }

         if (report.MissingCount == 0)
         {
            return new ConsistencyResult
            {
               Level = fresh ? Severity.Good : enabled ? Severity.Stale : Severity.Neutral,
               OfferSettings = !enabled,
               ReportPath = path,
               Message = Sentences(L("No problems found - every message row has its file on disk."), when, truncated, enabledNote)
            };
         }

         return new ConsistencyResult
         {
            Level = Severity.Bad,
            OfferSettings = !enabled,
            ReportPath = path,
            Entries = report.Entries,
            Message = Sentences(Plural(report.MissingCount, "{0} message references a file that is missing on disk.", "{0} messages reference a file that is missing on disk."),
               when, truncated, enabledNote,
               L("The check is read-only: the server does not delete or repair anything."))
         };
      }

      /// <summary>The page that turns the periodic check on, while it is off.</summary>
      private static Wpf.Ui.Controls.Button OpenHardening_()
      {
         var button = new Wpf.Ui.Controls.Button
         {
            Content = F("O_pen {0}", L(Services.NavigationMap.TitleOf("hardening"))),
            VerticalAlignment = VerticalAlignment.Center
         };
         System.Windows.Automation.AutomationProperties.SetAutomationId(button, "consistency-open-hardening");
         button.Click += (_, _) => (Application.Current?.MainWindow as MainWindow)?.NavigateTo("hardening");
         return button;
      }

      private void OpenReport()
      {
         if (reportPath_ == null)
            return;

         try
         {
            // .report has no shell association on a stock Windows install, so
            // fall back to revealing the file in Explorer instead of leaving the
            // administrator with an "Open with" dialog and no report.
            Process.Start(new ProcessStartInfo(reportPath_) { UseShellExecute = true });
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            try
            {
               // Explorer by its full path: a bare name is looked for in the
               // current directory first.
               string explorer = System.IO.Path.Join(Environment.GetFolderPath(Environment.SpecialFolder.Windows), "explorer.exe"); // no-loc
               Process.Start(new ProcessStartInfo(explorer, "/select,\"" + reportPath_ + "\"") { UseShellExecute = true });
            }
            catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
            {
               MessageBox.Show(F("Could not open {0}: {1}", reportPath_, ex.Message), L("Control Panel"));
            }
         }
      }
   }
}
