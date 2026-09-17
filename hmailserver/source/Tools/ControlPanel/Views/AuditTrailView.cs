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
   /// The audit trail: who changed what, when, from where and with what
   /// authority, newest first, a page at a time, over Utilities.GetAuditTrail -
   /// the same rows the Deck's page reads over GET /api/v1/audit. The chain is
   /// verified from here, and a setting's change is put back to the value its
   /// row says it had, through Utilities.RevertAuditedChange, which records the
   /// put-back as one more row rather than editing history.
   /// </summary>
   public class AuditTrailView : UserControl, IPageLifecycle
   {
      private readonly Toolbar toolbar_ = new() { SearchPlaceholder = L("Administrator or key contains") };

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
      private readonly EmptyState empty_ = new() { Icon = Wpf.Ui.Controls.SymbolRegular.DocumentBulletList24, Visibility = Visibility.Collapsed };

      private const int PageSize = 100;
      private int offset_;
      private int total_;

      public AuditTrailView() => Build();

      // Read on entry rather than in the constructor: a change made since the
      // page was first opened must be here when somebody comes back to it.
      public void OnEnter() => Reload();

      public void OnLeave()
      {
      }

      private void Status_(StatusLevel level, string text)
      {
         status_.Level = level;
         status_.Text = text;
         status_.Visibility = Visibility.Visible;
      }

      private void Reload()
      {
         status_.Visibility = Visibility.Collapsed;

         try
         {
            dynamic utilities = ServerSession.Current.Application.Utilities;
            string actor = toolbar_.SearchText?.Trim() ?? "";

            AuditTrailReport report;
            try
            {
               report = AuditTrailReport.Parse((string) utilities.GetAuditTrail(actor, PageSize, offset_));
            }
            finally
            {
               ServerSession.Release((object) utilities);
            }

            if (report.Error != null)
            {
               Status_(StatusLevel.Critical, F("The server's answer could not be read: {0}", report.Error));
               return;
            }

            total_ = report.Total;
            rows_.ItemsSource = Present_(report.Entries);

            StatusText.Show(empty_, null, report.Total, null,
               L("Nothing has been recorded yet. Every administrative change appears here."));

            if (report.Total > 0)
               Status_(StatusLevel.Information, F("{0} to {1} of {2}", offset_ + 1, offset_ + report.Entries.Count, report.Total));
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            Status_(StatusLevel.Critical, ServerSession.DescribeComError(ex));
         }
      }

      /// <summary>A row as the grid shows it: the server's words turned into the reader's.</summary>
      private sealed class RowView
      {
         public long Id { get; init; }
         public string When { get; init; }
         public string Who { get; init; }
         public string Interface { get; init; }
         public string From { get; init; }
         public string Object { get; init; }
         public string Name { get; init; }
         public string Action { get; init; }
         public string Detail { get; init; }
         public bool CanPutBack { get; init; }
      }

      private static List<RowView> Present_(IReadOnlyList<AuditRow> rows)
      {
         var result = new List<RowView>(rows.Count);
         foreach (AuditRow row in rows)
         {
            result.Add(new RowView
            {
               Id = row.Id,
               When = row.Time > 0 ? DateTimeOffset.FromUnixTimeSeconds(row.Time).ToLocalTime().ToString("yyyy-MM-dd HH:mm:ss") : "",
               Who = row.Actor,
               Interface = row.Interface,
               From = row.Address.Length == 0 ? "—" : row.Address, // no-loc
               Object = ObjectWord_(row.ObjectType),
               Name = row.ObjectName,
               Action = ActionWord_(row.Action),
               Detail = row.Detail,
               CanPutBack = row.CanPutBack
            });
         }
         return result;
      }

      // The server's nouns and verbs, as the reader's. What is not known is shown
      // as the server wrote it rather than hidden.
      private static string ObjectWord_(string objectType) => objectType switch
      {
         "setting" => L("Setting"),
         "domain" => L("Domain"),
         "account" => L("Account"),
         "alias" => L("Alias address"),
         "rule" => L("Mail rule"),
         _ => objectType
      };

      private static string ActionWord_(string action) => action switch
      {
         "created" => L("Created"),
         "updated" => L("Updated"),
         "deleted" => L("Deleted"),
         "unlocked" => L("Unlocked"),
         "cleared" => L("Cleared"),
         _ => action
      };

      private void VerifyChain()
      {
         status_.Visibility = Visibility.Collapsed;

         try
         {
            dynamic utilities = ServerSession.Current.Application.Utilities;
            AuditTrailReport.Verification verification;
            try
            {
               verification = AuditTrailReport.ParseVerification((string) utilities.VerifyAuditTrail());
            }
            finally
            {
               ServerSession.Release((object) utilities);
            }

            if (verification.Error != null)
               Status_(StatusLevel.Critical, F("The server's answer could not be read: {0}", verification.Error));
            else if (verification.Intact)
               Status_(StatusLevel.Good, F("The chain is intact. {0} row(s) checked.", verification.RowsChecked));
            else
               Status_(StatusLevel.Critical, F("The chain is broken at row {0}. {1}", verification.FirstBrokenId, verification.Reason));
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            Status_(StatusLevel.Critical, ServerSession.DescribeComError(ex));
         }
      }

      private void PutBackSelected()
      {
         if (rows_.SelectedItem is not RowView row)
         {
            Status_(StatusLevel.Information, L("Select a setting's change first."));
            return;
         }

         if (!row.CanPutBack)
         {
            Status_(StatusLevel.Information, L("Only a setting's change can be put back: the row has to carry the value the setting had."));
            return;
         }

         // YesNo, No on Enter, like every other confirmation that changes what the
         // server does.
         if (MessageBox.Show(
               F("Put {0} back to the value it had before this change? The put-back is recorded as a change of its own.", row.Name),
               L("Put a setting back"), MessageBoxButton.YesNo, MessageBoxImage.Question,
               MessageBoxResult.No)
             != MessageBoxResult.Yes)
         {
            return;
         }

         try
         {
            dynamic utilities = ServerSession.Current.Application.Utilities;
            string restored;
            try
            {
               restored = (string) utilities.RevertAuditedChange((int) row.Id);
            }
            finally
            {
               ServerSession.Release((object) utilities);
            }

            Reload();
            Status_(StatusLevel.Good, F("{0} is {1} again.", row.Name, restored));
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
         root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });

         root.Children.Add(new PageHeader
         {
            Title = L("Audit trail"),
            Subtitle = L("Every administrative change as one row in a hash chain: who made it, when, over which interface, from where, and with what authority. A setting's change can be put back from its row.")
         });

         toolbar_.PreviewKeyDown += (_, e) => { if (e.Key == System.Windows.Input.Key.Enter) { offset_ = 0; Reload(); } };

         var actions = new StackPanel { Orientation = Orientation.Horizontal };
         var putBack = MakeButton(L("_Put back"), Wpf.Ui.Controls.ControlAppearance.Primary, (_, _) => PutBackSelected());
         actions.Children.Add(putBack);
         SelectionGate.Bind(rows_, putBack);
         actions.Children.Add(MakeButton(L("_Verify the chain"), Wpf.Ui.Controls.ControlAppearance.Secondary, (_, _) => VerifyChain()));
         actions.Children.Add(MakeButton(L("_Newer"), Wpf.Ui.Controls.ControlAppearance.Secondary, (_, _) => { offset_ = Math.Max(0, offset_ - PageSize); Reload(); }));
         actions.Children.Add(MakeButton(L("_Older"), Wpf.Ui.Controls.ControlAppearance.Secondary, (_, _) => { if (offset_ + PageSize < total_) { offset_ += PageSize; Reload(); } }));
         actions.Children.Add(MakeButton(L("Re_fresh"), Wpf.Ui.Controls.ControlAppearance.Secondary, (_, _) => Reload()));
         toolbar_.Actions = actions;
         Grid.SetRow(toolbar_, 1);
         root.Children.Add(toolbar_);

         Grid.SetRow(status_, 2);
         root.Children.Add(status_);

         rows_.Columns.Add(Text_(L("When"), nameof(RowView.When), DataGridLength.Auto));
         rows_.Columns.Add(Text_(L("Who"), nameof(RowView.Who), DataGridLength.Auto));
         rows_.Columns.Add(Text_(L("Interface"), nameof(RowView.Interface), DataGridLength.Auto));
         rows_.Columns.Add(Text_(L("From"), nameof(RowView.From), DataGridLength.Auto));
         rows_.Columns.Add(Text_(L("Object"), nameof(RowView.Object), DataGridLength.Auto));
         rows_.Columns.Add(Text_(L("Name"), nameof(RowView.Name), DataGridLength.Auto));
         rows_.Columns.Add(Text_(L("Action"), nameof(RowView.Action), DataGridLength.Auto));
         rows_.Columns.Add(Text_(L("Detail"), nameof(RowView.Detail), new DataGridLength(1, DataGridLengthUnitType.Star)));

         // The whole detail on the row's tip: a long value is cut in the column.
         var rowStyle = new Style(typeof(DataGridRow));
         if (Application.Current?.TryFindResource(typeof(DataGridRow)) is Style appRowStyle)
            rowStyle.BasedOn = appRowStyle;
         rowStyle.Setters.Add(new Setter(ToolTipProperty, new System.Windows.Data.Binding(nameof(RowView.Detail))));
         rows_.RowStyle = rowStyle;

         AutomationProperties.SetName(rows_, L("Audit trail"));

         var host = new Grid();
         host.Children.Add(rows_);
         host.Children.Add(empty_);
         var card = new Card { Padding = new Thickness(8), Content = host };
         Grid.SetRow(card, 3);
         root.Children.Add(card);

         Content = root;
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
}
