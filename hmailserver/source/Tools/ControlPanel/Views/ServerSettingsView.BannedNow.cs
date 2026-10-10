// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Linq;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Data;
using hMailServer.ControlPanel.Services;
using MessageBox = hMailServer.ControlPanel.Views.Dialogs;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Views
{
   public partial class ServerSettingsView
   {
      /// <summary>
      /// The bans in force, first on the Auto-ban page (review finding F231): each
      /// banned address, why the server banned it and when the ban lifts, with Lift
      /// ban on its row and under the list. The page listed no ban at all, "Clear
      /// logon-failure list" said outright that it lifted none, the IP ranges page
      /// drew the bans among the permanent ranges with no column to tell them apart,
      /// and lifting one took four steps across the two pages: open IP ranges, search,
      /// pick the row by its address, Delete.
      ///
      /// A row of the page that writes on its own button, not on Save: a ban is an
      /// IP range the server wrote (Services/AutoBanList), and lifting it deletes the
      /// range, as the IP ranges page's Delete does, which the server answers by
      /// letting the address in from its next connection and by taking its firewall
      /// rule away where AutoBanFirewall put one.
      /// </summary>
      private sealed class BannedNowRows : ComSetting, INotPersisted
      {
         // The grid, its empty state, the status notice and the button of the editor
         // last built. Made anew by every BuildEditor, as the never-ban list's are: the
         // definition is built once, with the page, and BuildUi builds the editors
         // again on every visit and every Reload.
         private DataGrid grid_;
         private Scaffold.EmptyState empty_;
         private Scaffold.InlineNotice status_;
         private Wpf.Ui.Controls.Button lift_;

         public override bool WantsInitialValue => false;

         protected override bool CaptionOnRow => false;

         /// <summary>One ban in force, as the grid shows it.</summary>
         public sealed class BanRow
         {
            public int Id { get; init; }
            public string Address { get; init; }
            public string Cause { get; init; }
            public string Ends { get; init; }

            /// <summary>The row's Lift ban, named for its row (the owner's decision 15).</summary>
            public string LiftName => F("Lift the ban on {0}", Address);

            public override string ToString() => RowSpeech.Of(Address, Cause, RowSpeech.Labelled(L("Ends"), Ends));
         }

         protected override FrameworkElement BuildEditor(object value)
         {
            grid_ = new DataGrid
            {
               AutoGenerateColumns = false,
               IsReadOnly = true,
               CanUserAddRows = false,
               CanUserDeleteRows = false,
               SelectionMode = DataGridSelectionMode.Single,
               MinHeight = 120,
               MaxHeight = 280
            };
            ScrollViewer.SetHorizontalScrollBarVisibility(grid_, ScrollBarVisibility.Disabled);
            AutomationProperties.SetName(grid_, L("Banned now"));
            SetAid(grid_, "banned-now");

            grid_.Columns.Add(Column_(L("Address"), nameof(BanRow.Address), 1.2));
            grid_.Columns.Add(Column_(L("Why"), nameof(BanRow.Cause), 2));
            grid_.Columns.Add(Column_(L("Ends"), nameof(BanRow.Ends), 1));
            grid_.Columns.Add(new DataGridTemplateColumn
            {
               Header = "",
               CellTemplate = new DataTemplate { VisualTree = LiftIcon_() },
               Width = new DataGridLength(40),
               CanUserSort = false,
               CanUserResize = false
            });

            // The row's icon is the mouse's; the keyboard has Lift ban under the
            // list, which Delete on a row presses, as on every list page.
            grid_.AddHandler(ButtonBase.ClickEvent, new RoutedEventHandler((s, e) =>
            {
               if (e.OriginalSource is FrameworkElement { Tag: "lift", DataContext: BanRow row })
               {
                  e.Handled = true;
                  Lift_(row);
               }
            }));

            status_ = new Scaffold.InlineNotice
            {
               Margin = new Thickness(0, DesignTokens.Space.Sm, 0, 0),
               Visibility = Visibility.Collapsed
            };

            lift_ = new Wpf.Ui.Controls.Button
            {
               Content = L("_Lift ban"),
               Appearance = Wpf.Ui.Controls.ControlAppearance.Secondary,
               Margin = new Thickness(0, 0, DesignTokens.Space.Sm, DesignTokens.Space.Sm)
            };
            SetAid(lift_, "banned-now-lift");
            ControlHelp.Explain(lift_, L("Lift the selected ban: the address may connect again at once"));
            lift_.Click += (_, _) =>
            {
               if (grid_.SelectedItem is BanRow row)
                  Lift_(row);
            };
            SelectionGate.Bind(grid_, lift_);
            GridKeys.Wire(grid_, null, GridKeys.Pressing(lift_), null);

            var buttons = new WrapPanel { Margin = new Thickness(0, DesignTokens.Space.Sm, 0, 0) };
            buttons.Children.Add(lift_);

            // What no ban means, under the column headers, with no glyph: the grid
            // is 120 pixels at its floor, as the never-ban list's is.
            empty_ = new Scaffold.EmptyState { Padding = new Thickness(12), Margin = new Thickness(0, 33, 0, 0), Visibility = Visibility.Collapsed };
            var gridHost = new Grid();
            gridHost.Children.Add(grid_);
            gridHost.Children.Add(empty_);

            var panel = new StackPanel();
            panel.Children.Add(gridHost);
            panel.Children.Add(buttons);
            panel.Children.Add(status_);

            Reload_();
            return panel;
         }

         public override object ReadEditor() => null;

         public override void Write(object owner, string property)
         {
         }

         private static DataGridTextColumn Column_(string header, string path, double stars)
            => GridStyles.Trimmed(new DataGridTextColumn
            {
               Header = header,
               Binding = new Binding(path),
               Width = new DataGridLength(stars, DataGridLengthUnitType.Star)
            });

         /// <summary>The row's 24-pixel Lift ban, named for its row; not a tab stop, as the quarantine's row icons are not.</summary>
         private static FrameworkElementFactory LiftIcon_()
         {
            var button = new FrameworkElementFactory(typeof(Wpf.Ui.Controls.Button));
            button.SetValue(FrameworkElement.TagProperty, "lift");
            button.SetValue(Control.IsTabStopProperty, false);
            button.SetValue(Wpf.Ui.Controls.Button.AppearanceProperty, Wpf.Ui.Controls.ControlAppearance.Transparent);
            button.SetValue(Control.PaddingProperty, new Thickness(4));
            button.SetValue(FrameworkElement.WidthProperty, 24.0);
            button.SetValue(FrameworkElement.HeightProperty, 24.0);
            button.SetValue(Control.BorderThicknessProperty, new Thickness(0));
            button.SetValue(FrameworkElement.VerticalAlignmentProperty, VerticalAlignment.Center);
            button.SetBinding(FrameworkElement.ToolTipProperty, new Binding(nameof(BanRow.LiftName)));
            button.SetBinding(AutomationProperties.NameProperty, new Binding(nameof(BanRow.LiftName)));

            var icon = new FrameworkElementFactory(typeof(Wpf.Ui.Controls.SymbolIcon));
            icon.SetValue(Wpf.Ui.Controls.SymbolIcon.SymbolProperty, Wpf.Ui.Controls.SymbolRegular.LockOpen24);
            icon.SetValue(Wpf.Ui.Controls.SymbolIcon.FontSizeProperty, 16.0);
            button.AppendChild(icon);
            return button;
         }

         private void Say_(StatusLevel level, string text)
         {
            status_.Level = level;
            status_.Text = text;
            status_.Visibility = Visibility.Visible;
            Announce.Request(status_);
         }

         /// <summary>The bans in force, read from the IP ranges; a read that failed says so in their place.</summary>
         private void Reload_()
         {
            var rows = new List<BanRow>();
            try
            {
               bool sameClock = ServerSession.IsLocalSession;
               DateTime now = DateTime.Now;
               dynamic ranges = ServerSession.Current.Application.Settings.SecurityRanges;
               try
               {
                  int count = (int)ranges.Count;
                  for (int i = 0; i < count; i++)
                  {
                     dynamic range = ranges.Item[i];
                     try
                     {
                        string name = (string)range.Name;
                        if (!AutoBanList.IsBan(name, (bool)range.Expires))
                           continue;

                        rows.Add(new BanRow
                        {
                           Id = (int)range.ID,
                           Address = AutoBanList.Address((string)range.LowerIP, (string)range.UpperIP),
                           Cause = AutoBanList.Cause(name),
                           Ends = AutoBanList.Ends(ExpiresAt_(range), now, sameClock)
                        });
                     }
                     finally
                     {
                        ServerSession.Release((object)range);
                     }
                  }
               }
               finally
               {
                  ServerSession.Release((object)ranges);
               }
            }
            catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
            {
               grid_.ItemsSource = Array.Empty<BanRow>();
               empty_.Visibility = Visibility.Collapsed;
               Say_(StatusLevel.Critical, F("Could not read the bans: {0}", ServerSession.DescribeComError(ex)));
               return;
            }

            grid_.ItemsSource = rows.OrderBy(row => row.Address, StringComparer.OrdinalIgnoreCase).ToList();
            StatusText.Show(empty_, null, rows.Count, null, L("No address is banned now."));
         }

         // A VT_DATE in the server's local time, cast as the range's dialog casts it;
         // a value that is not one is taken for a time that has passed, as the server takes it.
         private static DateTime ExpiresAt_(dynamic range)
         {
            try
            {
               return (DateTime)range.ExpiresTime;
            }
            catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
            {
               return DateTime.MinValue;
            }
         }

         /// <summary>
         /// Lifts one ban after asking: the range is deleted by its id, as the IP
         /// ranges page deletes one. Cancel takes Enter, as before every act that
         /// cannot be taken back (docs/ControlPanelDesign.md): the Control Panel
         /// cannot put a ban back.
         /// </summary>
         private void Lift_(BanRow row)
         {
            if (!MessageBox.Confirm(
                  F("Lift the ban on {0}? It may connect again at once, and is banned again only when it earns a new ban.", row.Address),
                  L("Lift ban"), L("_Lift ban"), cancelByDefault: true))
            {
               return;
            }

            bool found = false;
            try
            {
               dynamic ranges = ServerSession.Current.Application.Settings.SecurityRanges;
               try
               {
                  int count = (int)ranges.Count;
                  for (int i = 0; i < count && !found; i++)
                  {
                     dynamic range = ranges.Item[i];
                     try
                     {
                        if ((int)range.ID == row.Id)
                        {
                           range.Delete();
                           found = true;
                        }
                     }
                     finally
                     {
                        ServerSession.Release((object)range);
                     }
                  }
               }
               finally
               {
                  ServerSession.Release((object)ranges);
               }
            }
            catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
            {
               Say_(StatusLevel.Critical, F("Could not lift the ban: {0}", ServerSession.DescribeComError(ex)));
               return;
            }

            Reload_();
            if (found)
               Say_(StatusLevel.Good, F("The ban on {0} is lifted.", row.Address));
            else
               Say_(StatusLevel.Information, F("{0} was no longer banned: the ban had lifted already.", row.Address));
         }
      }
   }
}
