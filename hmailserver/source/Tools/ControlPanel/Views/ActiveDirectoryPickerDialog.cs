// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using System.Windows.Input;
using hMailServer.ControlPanel.Services;
using hMailServer.ControlPanel.Views.Scaffold;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Views
{
   /// <summary>
   /// Modal Active Directory account browser. Lists the forest's domains, searches their
   /// users and returns the selected account(s). Used to fill the AD fields on an account
   /// and to bulk-import members into groups / distribution lists. On the standard
   /// frame: the query row above the list - the domain and the name, each captioned
   /// above its box - the status under the list as a notice at its level, Select and
   /// Cancel in the footer.
   ///
   /// The results are a read-only DataGrid on the application's implicit grid
   /// styles, as every other list of rows in the Control Panel is. It was the one
   /// ListView with a GridView in the application, and that took WPF's stock
   /// look rather than the theme's: a white list body with light column headers,
   /// separators and an empty filler column inside the dark theme's dialog, and a
   /// dark one-pixel outline with centred headers in the light theme.
   /// </summary>
   public class ActiveDirectoryPickerDialog : FluentDialogWindow
   {
      private readonly bool multiSelect_;
      private readonly ComboBox domainBox_ = new() { MinWidth = 240 };

      // What an empty box does is said in the box (review finding F124), where only
      // the box's accessible name and a status line said it.
      private readonly Wpf.Ui.Controls.TextBox searchBox_ = new() { PlaceholderText = L("Part of a name (empty lists everyone)") };

      private readonly Wpf.Ui.Controls.Button searchButton_ = new() { Content = L("_Search") };

      // Seven rows at least, and it grows with the results to eleven or so
      // before it scrolls, so that the query row above it and the status line
      // under it stay in view however many accounts a search finds; the frame
      // sizes the dialog to it within the work area. It was fixed at 300.
      private readonly DataGrid list_ = new()
      {
         MinHeight = 240,
         MaxHeight = 400,
         AutoGenerateColumns = false,
         IsReadOnly = true,
         CanUserAddRows = false,
         CanUserDeleteRows = false,
         CanUserReorderColumns = false,
         SelectionUnit = DataGridSelectionUnit.FullRow
      };

      // Where the picker stands, at the level of what it says (review finding F124):
      // Warning where Active Directory cannot be reached or has no domain, Critical
      // for a list or a search that failed, Information while searching and for what
      // a search found. It was one line of grey caption text for all of them, with no
      // mark and no next step. A polite live region, as every notice is.
      private readonly InlineNotice status_ = new() { Visibility = Visibility.Collapsed };

      private readonly Wpf.Ui.Controls.Button okButton_ = new()
      {
         Content = L("Se_lect"),
         Appearance = Wpf.Ui.Controls.ControlAppearance.Primary,
         MinWidth = 88,
         IsEnabled = false
      };

      /// <summary>The accounts the user chose (empty when cancelled).</summary>
      public List<AdUser> SelectedUsers { get; } = new();

      /// <summary>The domain the accounts were chosen from.</summary>
      public string SelectedDomain { get; private set; }

      public ActiveDirectoryPickerDialog(Window owner, bool multiSelect)
      {
         multiSelect_ = multiSelect;

         Owner = owner;
         Title = L("Browse Active Directory");

         var body = new StackPanel();
         body.Children.Add(BuildQueryRow());

         BuildList();
         body.Children.Add(list_);

         status_.Margin = new Thickness(0, DesignTokens.Space.Sm, 0, 0);
         body.Children.Add(status_);

         okButton_.Click += (s, e) => Accept();

         // Escape closes. Select is deliberately NOT the default button, which
         // the frame would otherwise make it: Enter in the search box means
         // "search" and always has (see BuildQueryRow), and a default button
         // would fire on the same keystroke - so Enter would search and then try
         // to accept a selection the search had just cleared. Enter on the
         // results list is the natural "select" gesture and is reached by tabbing
         // to the list, which is why the list's key handling is left to the
         // ListView.
         var cancel = new Wpf.Ui.Controls.Button { Content = L("Cancel"), MinWidth = 88 };
         cancel.Click += (s, e) => { DialogResult = false; Close(); };

         UseFrame(multiSelect ? L("Select Active Directory accounts") : L("Select an Active Directory account"),
            body, okButton_, cancel, width: 680);
         okButton_.IsDefault = false;

         // After the frame's own handler, which lands on the domain list: the
         // search box is where the typing happens.
         Loaded += (s, e) => { LoadDomains(); searchBox_.Focus(); };
      }

      private FrameworkElement BuildQueryRow()
      {
         // The domain and the name each a field with its caption above the box, the
         // captions of the line one height (FieldRow.LabelGroup) and Search under a
         // row of that height, so that the boxes and the button line up (review
         // finding F124). Domain was a 12-pixel caption to the left of its list, and
         // the search box had no caption at all - a screen reader announced "edit".
         var grid = new Grid { Margin = new Thickness(0, 0, 0, DesignTokens.Space.Md) };
         Grid.SetIsSharedSizeScope(grid, true);
         grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
         grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
         grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });

         FieldRow domainRow = DialogFields.Field(L("Active Directory _domain"), domainBox_);
         domainRow.LabelGroup = "AdQueryLine";
         domainRow.Margin = new Thickness(0, 0, DesignTokens.Space.Md, 0);
         Grid.SetColumn(domainRow, 0);
         grid.Children.Add(domainRow);

         FieldRow searchRow = DialogFields.Field(L("_Name or account"), searchBox_);
         searchRow.LabelGroup = "AdQueryLine";
         searchRow.Margin = new Thickness(0);
         Grid.SetColumn(searchRow, 1);
         grid.Children.Add(searchRow);

         // Handled, so the keystroke stops here. Without that, a default button
         // anywhere in this dialog would make one Enter both search and accept.
         searchBox_.KeyDown += (s, e) =>
         {
            if (e.Key != Key.Enter)
               return;

            Search();
            e.Handled = true;
         };
         searchButton_.Margin = new Thickness(DesignTokens.Space.Sm, 0, 0, 0);
         searchButton_.VerticalAlignment = VerticalAlignment.Top;
         searchButton_.Click += (s, e) => Search();
         var buttonLine = new Grid();
         buttonLine.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto, SharedSizeGroup = "AdQueryLine" });
         buttonLine.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
         Grid.SetRow(searchButton_, 1);
         buttonLine.Children.Add(searchButton_);
         Grid.SetColumn(buttonLine, 2);
         grid.Children.Add(buttonLine);

         return grid;
      }

      private void BuildList()
      {
         list_.SelectionMode = multiSelect_ ? DataGridSelectionMode.Extended : DataGridSelectionMode.Single;

         // The columns share the width, so there is nothing to scroll sideways
         // to, and a value wider than its column ends in an ellipsis and is whole
         // in its tool tip. A grid of shared columns and a horizontal bar that
         // can appear is the shape whose two bars took turns for ever at some
         // scalings (see the grids' note in docs/ControlPanelDesign.md).
         list_.Columns.Add(Column(L("Account"), nameof(AdUser.SamAccountName), 1));
         list_.Columns.Add(Column(L("Name"), nameof(AdUser.DisplayName), 1.2));
         list_.Columns.Add(Column(L("Email"), nameof(AdUser.Email), 1.4));
         ScrollViewer.SetHorizontalScrollBarVisibility(list_, ScrollBarVisibility.Disabled);

         AutomationProperties.SetName(list_, multiSelect_
            ? L("Matching Active Directory accounts. Select one or more.")
            : L("Matching Active Directory accounts. Select one."));

         list_.SelectionChanged += (s, e) => okButton_.IsEnabled = list_.SelectedItems.Count > 0;
         list_.MouseDoubleClick += (s, e) => { if (!multiSelect_ && list_.SelectedItem is AdUser) Accept(); };

         // The keyboard equivalent of the double-click above. Without it the only
         // way to commit a highlighted row was to tab out of the list and back to
         // the Select button - and Enter is what every list in Windows does. On
         // the preview, because the grid itself takes Enter first, to move to the
         // next row, and marks it handled before a handler on KeyDown sees it.
         list_.PreviewKeyDown += (s, e) =>
         {
            if (e.Key != Key.Enter || list_.SelectedItems.Count == 0)
               return;

            Accept();
            e.Handled = true;
         };
      }

      /// <summary>A column of the results, taking <paramref name="stars"/> shares of the width.</summary>
      private static DataGridTextColumn Column(string header, string property, double stars)
         => GridStyles.Trimmed(new DataGridTextColumn
         {
            Header = header,
            Binding = new System.Windows.Data.Binding(property),
            Width = new DataGridLength(stars, DataGridLengthUnitType.Star)
         });

      /// <summary>Where the picker stands, at the level of what it says; null hides it.</summary>
      private void Say_(StatusLevel level, string text)
      {
         status_.Level = level;
         status_.Text = text;
         status_.Visibility = string.IsNullOrEmpty(text) ? Visibility.Collapsed : Visibility.Visible;
      }

      /// <summary>
      /// The domain, the name and Search, which can be used only where there is a
      /// domain to search: Search stayed live where Active Directory could not be
      /// reached, and pressing it said "Select a domain first." (review finding F124).
      /// </summary>
      private void QueryCanRun_(bool canRun)
      {
         domainBox_.IsEnabled = canRun;
         searchBox_.IsEnabled = canRun;
         searchButton_.IsEnabled = canRun;
      }

      private void LoadDomains()
      {
         if (!ActiveDirectoryService.IsAvailable(out string reason))
         {
            Say_(StatusLevel.Warning, reason);
            QueryCanRun_(false);
            return;
         }

         try
         {
            var domains = ActiveDirectoryService.ListDomains();
            if (domains.Count == 0)
            {
               Say_(StatusLevel.Warning, L("No Active Directory domains could be found in the current forest, so there is nothing to search."));
               QueryCanRun_(false);
               return;
            }

            domainBox_.ItemsSource = domains;
            domainBox_.SelectedIndex = 0;
            QueryCanRun_(true);
            Say_(StatusLevel.Information, L("Enter part of a name and press Search, or search with an empty box to list all users."));
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            Say_(StatusLevel.Critical, F("Could not list domains: {0}", ServerSession.DescribeComError(ex)));
            QueryCanRun_(false);
         }
      }

      private async void Search()
      {
         string domain = domainBox_.SelectedItem as string;
         if (string.IsNullOrEmpty(domain) || !searchButton_.IsEnabled)
            return;

         string filter = searchBox_.Text;
         Say_(StatusLevel.Information, F("Searching {0}…", domain));
         Mouse.OverrideCursor = Cursors.Wait;
         list_.ItemsSource = null;
         okButton_.IsEnabled = false;

         try
         {
            List<AdUser> users = await Task.Run(() => ActiveDirectoryService.QueryUsers(domain, filter, 1000));
            list_.ItemsSource = users;
            // The search stops at a thousand, so a thousand found is the first
            // thousand of more, which is said whole rather than as a fragment
            // after the count.
            Say_(StatusLevel.Information, users.Count == 0
               ? F("No matching accounts in {0}.", domain)
               : users.Count >= 1000
                  ? F("The first {0} matching accounts are shown; refine the search to see the rest.", users.Count)
                  : Plural(users.Count, "{0} account found.", "{0} accounts found."));
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            Say_(StatusLevel.Critical, F("Search failed: {0}", ServerSession.DescribeComError(ex)));
         }
         finally
         {
            Mouse.OverrideCursor = null;
         }
      }

      private void Accept()
      {
         SelectedUsers.Clear();
         foreach (object item in list_.SelectedItems)
            if (item is AdUser u)
               SelectedUsers.Add(u);

         if (SelectedUsers.Count == 0)
            return;

         SelectedDomain = domainBox_.SelectedItem as string;
         DialogResult = true;
         Close();
      }
   }
}
