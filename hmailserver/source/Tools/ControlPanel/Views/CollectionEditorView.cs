// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Globalization;
using System.Reflection;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using hMailServer.ControlPanel.Services;
using Wpf.Ui.Controls;
using Button = Wpf.Ui.Controls.Button;
using TextBox = Wpf.Ui.Controls.TextBox;
using TextBlock = System.Windows.Controls.TextBlock;
using DataGrid = System.Windows.Controls.DataGrid;
using System.Linq;
using MessageBox = hMailServer.ControlPanel.Views.Dialogs;

// Wpf.Ui.Controls has a Card of its own, so the scaffold's components are named
// explicitly here rather than imported wholesale.
using Card = hMailServer.ControlPanel.Views.Scaffold.Card;
using EmptyState = hMailServer.ControlPanel.Views.Scaffold.EmptyState;
using FillPanel = hMailServer.ControlPanel.Views.Scaffold.FillPanel;
using InlineNotice = hMailServer.ControlPanel.Views.Scaffold.InlineNotice;
using PageHeader = hMailServer.ControlPanel.Views.Scaffold.PageHeader;
using StatusPill = hMailServer.ControlPanel.Views.Scaffold.StatusPill;
using Toolbar = hMailServer.ControlPanel.Views.Scaffold.Toolbar;
using StatusLevel = hMailServer.ControlPanel.Services.StatusLevel;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Views
{
   /// <summary>
   /// Generic, data-driven editor for any hMailServer COM collection
   /// (SURBL servers, DNS blacklists, white-list addresses, blocked
   /// attachments, groups, server messages, ...). A <see cref="CollectionSpec"/>
   /// describes the fields; this view renders a polished list with a live
   /// count badge, add / edit / delete and a generated property dialog.
   /// </summary>
   public class CollectionEditorView : UserControl, IPageLifecycle
   {
      public enum FieldKind { Text, Multiline, Number, Bool, Combo, Password }

      public class FieldSpec
      {
         public string Prop;
         public string Label;
         public FieldKind Kind = FieldKind.Text;
         public (int Value, string Label)[] Options;   // for Combo
         public bool ShowInGrid = true;
         public double GridWidth = double.NaN;          // NaN => *
         public object Default;

         // The column's header, where the dialog's caption is a sentence: the
         // caption of a tick box, "Requires a verified DKIM signature (decided
         // after DATA)", headed an 80-pixel column, in which it read "Requir…"
         // three columns in a row, and the allow and block list's "Block rather
         // than allow" read "Block rath…". The caption stays the header's tool
         // tip and its help text. Null heads the column with the caption.
         public string GridLabel;

         // A Bool's words in the grid, in place of Yes and No, which say nothing
         // without the sentence they answer: "Required" and "—" under SPF,
         // "Block" and "Allow" under Action.
         public string GridTrue, GridFalse;

         // What the grid shows for an empty value: "Never" for an expiry.
         public string GridEmpty;

         // The Bool property that gates the value: while it is false the grid
         // shows GridEmpty, as a date that does not apply is no date. Null
         // shows the value as it is.
         public string GridOnlyWhen;

         // Shown, never written: a property the server answers about the
         // object and does not take (a key's fingerprint and expiry, say).
         public bool ReadOnly;

         // Written, never read back: the server answers it empty, so the box
         // opens empty, and what is left empty is not sent - which keeps what
         // the server holds rather than replacing it with nothing.
         public bool WriteOnly;

         // What the field wants, in one line under its editor in the dialog: an
         // example host, the forms the server reads (review finding F174).
         public string Hint;

         // Refused empty, on the field, by the dialog: a value the entry is
         // useless without, such as a DNS blacklist's expected result, which
         // empty matches no answer at all. Text fields only.
         public bool Required;
      }

      public class CollectionSpec
      {
         public string Title;
         public string Subtitle;
         public Func<dynamic> GetCollection;            // returns the COM collection (caller releases)
         public List<FieldSpec> Fields = new();
         public bool CanAdd = true;
         public bool CanDelete = true;
         public string ItemNoun = L("item");

         // The add and edit dialogs' titles, each one whole catalogued text
         // marked with N() and translated where it is shown: "Add a SURBL
         // server", "Edit the SURBL server". "Add {0}" with the noun put in
         // gave Japanese "SURBL サーバー を追加" with the noun's own spacing,
         // and a language that marks the object of the verb - German "einen",
         // Polish's accusative - had nothing to mark (review finding F283).
         public string AddTitle = N("Add an entry");
         public string EditTitle = N("Edit the entry");

         // The dialog lays the tick boxes that lead the spec - Active, Enabled,
         // the grid's first column - after the fields that say what the entry
         // is, so that an Add dialog opens on the host and not on a tick
         // (review finding F174). Asked for by the short specs only: on a spec
         // of twenty fields the leading box is the entry's master switch, and
         // laid last it is below the fold of an Edit dialog opened to switch
         // the entry off. The grid keeps the spec's order either way.
         public bool TickBoxesLast;

         // The Bool property that switches an entry on, for a list whose pill
         // counts the entries that are on rather than all of them: "Active: 2
         // of 3", at Warning when none is (review finding F268). Null counts
         // every entry.
         public string ActiveProp;

         // The count beside the title, one whole catalogued text for one entry
         // and one for any other number, each marked with N() and translated
         // where it is shown: "1 security group" and "{0} security groups". A
         // noun with an English plural ending put on it read "3 Sicherheitsgruppes"
         // in German and "5 ルールs" in Japanese; a whole text lets each language
         // count its own way (Russian puts the noun first, "Групп безопасности: 3").
         public string CountOne = N("1 item");
         public string CountMany = N("{0} items");

         // The editor sits on a tab of a dialog whose Save and Cancel do not reach
         // it: Add, Edit and Delete write to the server as they are pressed, and a
         // Cancel afterwards keeps them. Said under the hint (SavedAsMadeNote), as
         // the Levels tab says it of its own Save (review findings F196 and F108).
         public bool SavesAsMadeInDialog;

         // The question Delete asks, one whole catalogued sentence marked with N()
         // and translated where it is asked, with the entry's name for {0}: "Delete
         // the SURBL server {0}?" (review findings F066, F185). The question was
         // "Delete this {0}?" with the noun put in, which named no entry - on a grid
         // whose selected row was hard to tell from the others - and answered Enter
         // with Yes. The name is the first of DeleteNameProps the row has a value
         // for, or, without them, the first text the grid shows.
         public string DeletePrompt = N("Delete the entry {0}?");
         public string[] DeleteNameProps;
      }

      internal sealed class Row
      {
         // These must stay properties: the generated grid columns bind to
         // "Values[<prop>]", and WPF data binding resolves properties only -
         // a public field silently binds to nothing and every cell renders blank.
         public int Id { get; set; }
         public Dictionary<string, object> Values { get; } = new();
         public string Display(string prop) =>
            Values.TryGetValue(prop, out object v) ? FormatCell(v) : "";

         // What a screen reader says for the row: its cells, not the name of
         // this class, which is what a grid row is announced as otherwise.
         public override string ToString()
            => string.Join(", ", Values.Values.Select(FormatCell).Where(s => !string.IsNullOrWhiteSpace(s)).Take(3));
      }

      // The floor of a star column - a description, a reject message, a note -
      // below which the grid scrolls sideways rather than squeezing it.
      private const double StarColumnFloor = 120;

      private readonly CollectionSpec spec_;
      private readonly bool embedded_;
      private readonly DataGrid grid_ = new();
      private readonly ObservableCollection<Row> rows_ = new();

      // How many entries there are, as a pill beside the page title - a state,
      // so it is drawn the way every other state on every other page is. Normal,
      // because a count says nothing on its own.
      private readonly StatusPill countBadge_ = new() { Level = StatusLevel.Normal };

      // What the last action said, at its level, and what an empty list means.
      private readonly InlineNotice status_ = new() { Visibility = Visibility.Collapsed };
      private readonly EmptyState empty_ = new() { Icon = SymbolRegular.DocumentBulletList24, Visibility = Visibility.Collapsed };

      // Add waits for a read that works (Services/ReadFailure); null for a
      // collection that cannot be added to.
      private Button addButton_;

      public CollectionEditorView(CollectionSpec spec) : this(spec, false)
      {
      }

      /// <summary>Creates the editor for one collection, optionally without page chrome.</summary>
      /// <param name="embedded">
      /// When true the page chrome (large title/subtitle and outer page margins)
      /// is dropped so the editor can be hosted inside a dialog tab. Only a
      /// compact one-line hint is shown above the grid.
      /// </param>
      public CollectionEditorView(CollectionSpec spec, bool embedded)
      {
         spec_ = spec;
         embedded_ = embedded;
         Build();
      }

      public void OnEnter() => Reload();
      public void OnLeave() { }

      /// <summary>
      /// The line a dialog's tab carries when what is on it is written as it is made -
      /// an embedded list's Add, Edit and Delete, the account's rules - so that Cancel
      /// is not read as undoing it. One sentence for every such tab, in the caption
      /// style of the hint above it.
      /// </summary>
      public static TextBlock SavedAsMadeNote()
      {
         var note = new TextBlock
         {
            Text = L("Changes here are saved as you make them, not with the dialog's Save, so Cancel does not undo them."),
            TextWrapping = TextWrapping.Wrap
         };
         note.SetResourceReference(StyleProperty, "TextCaption");
         System.Windows.Automation.AutomationProperties.SetAutomationId(note, "saved-as-made");
         return note;
      }

      /// <summary>Loads (or reloads) the collection. Used when embedded in a dialog.</summary>
      public void Refresh() => Reload();

      // ---- UI scaffolding ----------------------------------------------------

      private void Build()
      {
         var root = new Grid();
         if (embedded_)
            root.Margin = new Thickness(0, 8, 0, 0);
         else
            root.SetResourceReference(MarginProperty, "AppPagePadding");

         // The header or the hint, the toolbar (embedded), then the notice and
         // the grid in a FillPanel with the rest of the height. The notice was
         // under the grid, where a failed read put "Could not load" below three
         // hundred pixels of blank card; it is above it now, under the header,
         // as on every other list page.
         root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
         root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
         root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });

         var actions = new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Right };
         if (spec_.CanAdd)
         {
            addButton_ = MakeButton(L("_Add"), ControlAppearance.Primary, SymbolRegular.Add24, (_, _) => OpenDialog(null));
            actions.Children.Add(addButton_);
         }
         var edit = MakeButton(L("_Edit"), ControlAppearance.Secondary, SymbolRegular.Edit24, (_, _) => EditSelected());
         actions.Children.Add(edit);
         Wpf.Ui.Controls.Button del = null;
         if (spec_.CanDelete)
         {
            del = MakeButton(L("_Delete"), ControlAppearance.Secondary, SymbolRegular.Delete24, (_, _) => DeleteSelected());
            // Red words only while it can be pressed (App.xaml says why a
            // style and not a local Foreground).
            if (Application.Current?.TryFindResource("DangerTextButton") is Style danger)
               del.Style = danger;
            actions.Children.Add(del);
         }
         actions.Children.Add(MakeButton(L("_Refresh"), ControlAppearance.Secondary, SymbolRegular.ArrowSync24, (_, _) => Reload()));
         // Edit and Delete act on the selected row, so they are enabled only
         // while there is one; with nothing selected they used to answer a click
         // with "Select a row first." in the status line.
         if (del != null)
            Services.SelectionGate.Bind(grid_, edit, del);
         else
            Services.SelectionGate.Bind(grid_, edit);

         // Page mode puts the title, the count and the actions in one header;
         // embedded mode has no page title to hang them on, so the hint goes
         // above a toolbar that carries the count and the same actions.
         if (embedded_)
         {
            var head = new StackPanel { Margin = new Thickness(0, 0, 0, 10) };
            if (!string.IsNullOrEmpty(spec_.Subtitle))
            {
               var hint = new TextBlock { Text = spec_.Subtitle, TextWrapping = TextWrapping.Wrap };
               hint.SetResourceReference(StyleProperty, "TextCaption");
               head.Children.Add(hint);
            }

            if (spec_.SavesAsMadeInDialog)
            {
               TextBlock note = SavedAsMadeNote();
               if (head.Children.Count > 0)
                  note.Margin = new Thickness(0, DesignTokens.Space.Xs, 0, 0);
               head.Children.Add(note);
            }

            if (head.Children.Count > 0)
               root.Children.Add(head);

            var toolbar = new Toolbar { ShowSearch = false, Filters = countBadge_, Actions = actions };
            Grid.SetRow(toolbar, 1);
            root.Children.Add(toolbar);
         }
         else
         {
            root.Children.Add(new PageHeader
            {
               Title = spec_.Title,
               Subtitle = spec_.Subtitle,
               Status = countBadge_,
               Actions = actions
            });
         }

         // Grid in a card. On a page, the IP ranges shape: the card takes the
         // height the window has under the header and the notice, down to a
         // floor of three rows, and below that the page scrolls; the notice
         // above it keeps its own height. The card was a grid's star row, which
         // in the narrowest window (760 by 520) left the greylisting bypass list
         // its header row alone. Embedded in a dialog's tab, whose height the
         // dialog fixes, the FillPanel is measured at the tab's height and
         // shares it out the same way.
         var card = new Card { Padding = new Thickness(6) };
         FillPanel parts;
         if (embedded_)
         {
            parts = new FillPanel();
            Grid.SetRow(parts, 2);
            root.Children.Add(parts);
         }
         else
         {
            ScrollViewer scroll = FillPanel.ScrollingPage(out parts);
            Grid.SetRow(scroll, 2);
            root.Children.Add(scroll);
         }
         // The notice's own style gives it the card gap below, as on the
         // quarantine page, so it carries no margin of its own here.
         parts.Children.Add(status_);
         parts.Children.Add(FillPanel.Filling(card, FillPanel.ListCardFloor));

         grid_.AutoGenerateColumns = false;
         grid_.IsReadOnly = true;
         grid_.SelectionMode = DataGridSelectionMode.Single;
         grid_.ItemsSource = rows_;
         // A grid whose columns keep their widths and scroll sideways keeps its
         // vertical bar, as the rules page's list does, so that neither bar can
         // turn the other on and off for ever (ControlPanelDesign.md, Grids).
         ScrollViewer.SetVerticalScrollBarVisibility(grid_, ScrollBarVisibility.Visible);
         // Enter and F2 edit the selected row, Delete deletes it after asking and
         // Insert adds one, each through its button; a double-click edits a row,
         // where it was the grid's and a double-click on a header's divider, to
         // fit a column, opened Edit as well (Views/GridKeys).
         GridKeys.Wire(grid_, edit, del, addButton_);
         System.Windows.Automation.AutomationProperties.SetName(grid_, spec_.Title);

         foreach (FieldSpec f in spec_.Fields.Where(f => f.ShowInGrid))
         {
            string prop = f.Prop;
            var col = new DataGridTextColumn
            {
               Header = f.GridLabel ?? f.Label,
               // A Combo column stores the enum's number, and a grid cell reading
               // "1" where the dialog above it reads "SSL/TLS" is not a display
               // detail - it is the difference between a list an administrator can
               // scan for the unencrypted row and one they cannot.
               Binding = new System.Windows.Data.Binding($"Values[{prop}]")
               {
                  Converter = f.Kind == FieldKind.Combo && f.Options != null
                     ? new CellConverter(f.Options)
                     : new CellConverter(f.GridTrue, f.GridFalse, f.GridEmpty)
               },
               Width = double.IsNaN(f.GridWidth)
                  ? new DataGridLength(1, DataGridLengthUnitType.Star)
                  : new DataGridLength(f.GridWidth),
               // A floor under every column, as the IP ranges page's columns
               // have, so that a grid wider than its card scrolls sideways
               // rather than squeezing its columns: given less than its
               // columns ask for, a DataGrid takes the difference from them,
               // down to 20 pixels each, and the remote domains grid's
               // headers were cut at every width their fields named.
               MinWidth = double.IsNaN(f.GridWidth) ? StarColumnFloor : f.GridWidth
            };

            // A value that applies only while another is true - an expiry
            // date while "Expires" is ticked - reads both, and shows what an
            // empty value shows while the other is false.
            if (f.GridOnlyWhen != null)
            {
               var gated = new System.Windows.Data.MultiBinding { Converter = new GatedCellConverter(f.GridTrue, f.GridFalse, f.GridEmpty) };
               gated.Bindings.Add(new System.Windows.Data.Binding($"Values[{prop}]"));
               gated.Bindings.Add(new System.Windows.Data.Binding($"Values[{f.GridOnlyWhen}]"));
               col.Binding = gated;
            }

            // A number column is right-aligned, header and all, so the digits
            // line up and a column of scores can be read down.
            if (f.Kind == FieldKind.Number)
               GridStyles.Number(col);

            // A short header keeps the caption it stands for as its tool tip
            // and its help text, so the sentence is a hover or a screen
            // reader's question away.
            if (f.GridLabel != null)
               GridStyles.HeaderCaption(col, f.Label);

            // A value of several lines - a server message, a signature - is one
            // line in its cell, its line breaks read as spaces: printed as it is,
            // one message filled the grid's height with lines each cut at the
            // column's edge. Edit shows it whole.
            if (f.Kind == FieldKind.Multiline)
               col.Binding = new System.Windows.Data.Binding($"Values[{prop}]") { Converter = OneLineConverter.Instance };

            // A value wider than its column ends in an ellipsis and is whole in
            // the cell's tool tip, as the header is.
            GridStyles.Trimmed(col);

            // ... and the tool tip of a value of several lines keeps its line breaks.
            if (f.Kind == FieldKind.Multiline)
            {
               var whole = new Style(typeof(TextBlock), col.ElementStyle);
               whole.Setters.Add(new Setter(FrameworkElement.ToolTipProperty,
                  new System.Windows.Data.Binding($"Values[{prop}]") { Converter = CellConverter.Instance }));
               col.ElementStyle = whole;
            }

            grid_.Columns.Add(col);
         }

         var host = new Grid();
         host.Children.Add(grid_);
         host.Children.Add(empty_);
         card.Content = host;

         Content = root;
      }

      private static Button MakeButton(string text, ControlAppearance appearance, SymbolRegular icon, RoutedEventHandler onClick)
      {
         var b = new Button
         {
            Content = text,
            Appearance = appearance,
            Icon = new SymbolIcon { Symbol = icon },
            Margin = new Thickness(8, 0, 0, 0),
            MinWidth = 92
         };
         b.Click += onClick;
         return b;
      }

      // ---- Data --------------------------------------------------------------

      private void Reload()
      {
         rows_.Clear();
         dynamic collection = null;
         try
         {
            collection = spec_.GetCollection();
            int count = (int)collection.Count;
            for (int i = 0; i < count; i++)
            {
               dynamic item = collection.Item[i];
               var row = new Row { Id = TryGetId(item) };
               foreach (FieldSpec f in spec_.Fields)
               {
                  try { row.Values[f.Prop] = GetProp(item, f.Prop); }
                  catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck)) { row.Values[f.Prop] = null; }
               }
               rows_.Add(row);
               ServerSession.Release(item);
            }
            // An empty list is a state to name, not a blank to stare at: the
            // empty state says so in the grid's own place.
            // The empty state carries the sentence that was in the status line.
            // Only a list that can be added to has one: the one collection that
            // cannot (the fixed set of server messages) is never empty, and
            // inventing a sentence for it would be a new text for a state that
            // does not occur.
            if (spec_.CanAdd)
               StatusText.Show(empty_, null, rows_.Count, null, L("Nothing here yet - Add creates the first entry."));

            // A load that worked says nothing: the count beside the title and
            // the empty state in the grid's place already say what there is, and
            // a notice on every visit to every list page would be noise.
            status_.Visibility = Visibility.Collapsed;
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            // Neither the rows of the last read nor "Nothing here yet" nor a count
            // of "0 SURBL servers" is a fact this page has now: the rows are
            // cleared above, the empty state and the pill go, the notice says what
            // happened, with Try again, and Add waits for a read that works
            // (Services/ReadFailure).
            ReadFailure.Show(status_, empty_, null, L("Could not read the list: "), ex, Reload, addButton_);
            countBadge_.Visibility = Visibility.Collapsed;
            return;
         }
         finally
         {
            ServerSession.Release((object)collection);
         }

         ReadFailure.Clear(status_, addButton_);
         int activeCount = ActiveCount_();
         countBadge_.Level = ActiveEntries.PillLevel(activeCount, rows_.Count);
         countBadge_.Visibility = Visibility.Visible;
         countBadge_.Text = activeCount >= 0 ? ActiveEntries.PillText(activeCount, rows_.Count) : rows_.Count == 1
            ? L(spec_.CountOne)
            : F(spec_.CountMany, rows_.Count);
      }

      /// <summary>What the last action said, at its level.</summary>
      private void Say_(StatusLevel level, string text)
      {
         ReadFailure.Clear(status_);
         status_.Level = level;
         status_.Text = text;
         status_.Visibility = Visibility.Visible;
      }

      private void EditSelected()
      {
         if (grid_.SelectedItem is Row row)
            OpenDialog(row);
         else
            Say_(StatusLevel.Information, L("Select a row first."));
      }

      private void OpenDialog(Row existing)
      {
         var dlg = new FieldDialog(spec_, existing, Window.GetWindow(this));
         if (dlg.ShowDialog() != true)
            return;

         dynamic collection = null;
         dynamic item = null;
         try
         {
            collection = spec_.GetCollection();
            item = existing == null ? collection.Add() : FindById(collection, existing.Id);
            if (item == null)
            {
               Say_(StatusLevel.Warning, L("The item no longer exists."));
               return;
            }

            foreach (KeyValuePair<string, object> kv in dlg.Result)
               SetProp(item, kv.Key, kv.Value);

            item.Save();
            Say_(StatusLevel.Good, existing == null ? F("Added at {0}.", DateTime.Now.ToLongTimeString()) : F("Saved at {0}.", DateTime.Now.ToLongTimeString()));
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            MessageBox.Show(F("Could not save: {0}", ServerSession.DescribeComError(ex)), L("Control Panel"));
         }
         finally
         {
            ServerSession.Release((object)item);
            ServerSession.Release((object)collection);
         }

         Reload();
      }

      private void DeleteSelected()
      {
         if (grid_.SelectedItem is not Row row)
         {
            Say_(StatusLevel.Information, L("Select a row first."));
            return;
         }

         if (!MessageBox.Confirm(F(spec_.DeletePrompt, DeleteName_(row)), L("Control Panel"), L("_Delete"), destructive: true))
            return;

         dynamic collection = null;
         dynamic item = null;
         try
         {
            collection = spec_.GetCollection();
            item = FindById(collection, row.Id);
            if (item != null)
               item.Delete();
            Say_(StatusLevel.Good, L("Deleted."));
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            MessageBox.Show(F("Could not delete: {0}", ServerSession.DescribeComError(ex)), L("Control Panel"));
         }
         finally
         {
            ServerSession.Release((object)item);
            ServerSession.Release((object)collection);
         }

         Reload();
      }

      /// <summary>
      /// What the delete question calls the row: the first of the spec's
      /// DeleteNameProps with a value, or the first text the grid shows - never a
      /// tick box's Yes or a number - or, failing both, the row's first cells.
      /// </summary>
      private string DeleteName_(Row row)
      {
         IEnumerable<string> props = spec_.DeleteNameProps
            ?? spec_.Fields.Where(f => f.ShowInGrid && f.Kind is FieldKind.Text or FieldKind.Multiline).Select(f => f.Prop);

         foreach (string prop in props)
         {
            string value = row.Display(prop);
            if (!string.IsNullOrWhiteSpace(value))
               return value.Trim();
         }

         return row.ToString();
      }

      private dynamic FindById(dynamic collection, int id)
      {
         int count = (int)collection.Count;
         for (int i = 0; i < count; i++)
         {
            dynamic item = collection.Item[i];
            if (TryGetId(item) == id)
               return item;
            ServerSession.Release(item);
         }
         return null;
      }

      /// <summary>
      /// How many of the rows have the spec's ActiveProp on, or -1 for a spec
      /// that counts every entry and for an empty list, whose count stays the
      /// spec's own beside the empty state.
      /// </summary>
      private int ActiveCount_() => ActiveEntries.Count(rows_.Select(row => row.Values), spec_.ActiveProp);

      // ---- COM reflection helpers -------------------------------------------

      private static int TryGetId(dynamic item)
      {
         try { return (int)GetProp(item, "ID"); }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck)) { return -1; }
      }

      internal static object GetProp(object owner, string name)
         => owner.GetType().InvokeMember(name, BindingFlags.GetProperty, null, owner, null);

      internal static void SetProp(object owner, string name, object value)
         => owner.GetType().InvokeMember(name, BindingFlags.SetProperty, null, owner, new[] { value });

      internal static string FormatCell(object v)
      {
         if (v == null) return "";
         if (v is bool b) return b ? L("Yes") : L("No");
         return Convert.ToString(v, CultureInfo.CurrentCulture);
      }

      /// <summary>A cell of several lines as one: every run of line breaks and spaces is one space.</summary>
      private sealed class OneLineConverter : System.Windows.Data.IValueConverter
      {
         public static readonly OneLineConverter Instance = new();

         public object Convert(object value, Type t, object p, CultureInfo c)
            => string.Join(" ", FormatCell(value).Split((char[])null, StringSplitOptions.RemoveEmptyEntries));

         public object ConvertBack(object value, Type t, object p, CultureInfo c) => value;
      }

      /// <summary>A cell's words for a value: a Bool's own words where the field names them, an empty value's word, and otherwise the value as text.</summary>
      internal static string Words(object value, string whenTrue, string whenFalse, string whenEmpty)
      {
         if (value is bool b && (whenTrue != null || whenFalse != null))
            return b ? whenTrue ?? L("Yes") : whenFalse ?? L("No");

         string text = FormatCell(value);
         return text.Length == 0 && whenEmpty != null ? whenEmpty : text;
      }

      private sealed class CellConverter : System.Windows.Data.IValueConverter
      {
         /// <summary>Option labels for a Combo column, or null for every other kind.</summary>
         private readonly (int Value, string Label)[] options_;

         private readonly string whenTrue_, whenFalse_, whenEmpty_;

         private CellConverter()
         {
         }

         public CellConverter((int Value, string Label)[] options) => options_ = options;

         public CellConverter(string whenTrue, string whenFalse, string whenEmpty)
         {
            whenTrue_ = whenTrue;
            whenFalse_ = whenFalse;
            whenEmpty_ = whenEmpty;
         }

         public static readonly CellConverter Instance = new();

         public object Convert(object value, Type t, object p, CultureInfo c)
         {
            if (options_ == null)
               return Words(value, whenTrue_, whenFalse_, whenEmpty_);

            if (options_ != null && value != null)
            {
               // A value with no matching option falls through to the number
               // rather than being shown as blank or as the first option: an
               // enum the GUI does not know about is a real thing to notice,
               // and blanking it would hide it.
               try
               {
                  int number = System.Convert.ToInt32(value, CultureInfo.InvariantCulture);
                  foreach ((int Value, string Label) option in options_.Where(o => o.Value == number))
                     return option.Label;
               }
               catch (FormatException)
               {
                  // Deliberately ignored: best effort only, and the outcome of the surrounding operation does not depend on this succeeding.
               }
               catch (InvalidCastException)
               {
                  // Deliberately ignored: best effort only, and the outcome of the surrounding operation does not depend on this succeeding.
               }
               catch (OverflowException)
               {
                  // Deliberately ignored: best effort only, and the outcome of the surrounding operation does not depend on this succeeding.
               }
            }

            return FormatCell(value);
         }

         public object ConvertBack(object value, Type t, object p, CultureInfo c) => value;
      }

      /// <summary>
      /// A cell whose value applies only while another Bool is true: the value's
      /// words while it is, and the empty value's word - "Never" - while it is not.
      /// </summary>
      private sealed class GatedCellConverter : System.Windows.Data.IMultiValueConverter
      {
         private readonly string whenTrue_, whenFalse_, whenEmpty_;

         public GatedCellConverter(string whenTrue, string whenFalse, string whenEmpty)
         {
            whenTrue_ = whenTrue;
            whenFalse_ = whenFalse;
            whenEmpty_ = whenEmpty;
         }

         public object Convert(object[] values, Type t, object p, CultureInfo c)
         {
            object value = values != null && values.Length > 0 ? values[0] : null;
            object gate = values != null && values.Length > 1 ? values[1] : null;
            if (gate is bool applies && !applies)
               return whenEmpty_ ?? "";
            return Words(value, whenTrue_, whenFalse_, whenEmpty_);
         }

         public object[] ConvertBack(object value, Type[] t, object p, CultureInfo c) => throw new NotSupportedException();
      }
   }
}
