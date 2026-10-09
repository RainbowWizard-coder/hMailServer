// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Data;
using System.Windows.Input;
using System.Windows.Media;
using hMailServer.ControlPanel.Services;
using hMailServer.ControlPanel.Views.Scaffold;
using MessageBox = hMailServer.ControlPanel.Views.Dialogs;
using static hMailServer.ControlPanel.Services.Loc;
using ControlAppearance = Wpf.Ui.Controls.ControlAppearance;
using IconElement = Wpf.Ui.Controls.IconElement;
using SymbolIcon = Wpf.Ui.Controls.SymbolIcon;
using SymbolRegular = Wpf.Ui.Controls.SymbolRegular;

namespace hMailServer.ControlPanel.Views
{
   /// <summary>
   /// The spam quarantine review queue: what is being held, grouped the way a burst
   /// arrives, and what can be done about each message, each group, or every
   /// checked one at once.
   ///
   /// This page exists because of what quarantining actually does. A refused message
   /// is answered 550 during the SMTP conversation and the sender is told; a
   /// quarantined one is answered 250, so the sender believes it arrived and will
   /// never retry. That makes this list the ONLY place those messages exist - which
   /// is the point when the filter was wrong, and the reason nothing here happens on
   /// a timer. Release delivers; Delete is final.
   ///
   /// One ESP or one HELO name that dumps forty messages makes forty rows, so the rows
   /// are grouped - by the organization's domain of the sender, of the HELO name, or
   /// not at all - and every row, every group and the header carry the same four
   /// actions: release, delete, whitelist the sender and release, blacklist the
   /// sender and delete. The header's act on every checked message. Whitelist and
   /// blacklist ask which list: the sender's address on the server's allow and block
   /// list, or the domain on the domain white and black list - never a third one.
   /// The logic is QuarantineReview's (Services), where the tests hold it; this is
   /// the drawing.
   ///
   /// Read-heavy on purpose: the columns are the ones somebody needs in order to
   /// decide without opening anything - when, the HELO name, from whom, to whom, how
   /// badly, and what it says it is - with the headers and a plain-text preview one
   /// click away. If those are not enough, the message is a judgement call rather
   /// than an obvious one, and it is better to release it to the person it was
   /// addressed to than to guess here.
   /// </summary>
   public class QuarantineView : UserControl, IPageLifecycle
   {
      // The two leading columns have fixed widths, because a group's header row is
      // drawn by a template of its own and lines its box and its four icons up
      // with the message rows' by these numbers.
      private const double CheckWidth = 44;
      private const double ActionsWidth = 136;
      private const double BoxWidth = 22;

      // The action glyphs' colours, by resource key (IconButton_): the success
      // colour for what keeps a message, the danger colour for what removes it.
      private const string KeepColourKey = "AppSuccessBrush";
      private const string RemoveColourKey = "AppDangerBrush";

      // What the server answers COM with for a member it does not have: a Control
      // Panel newer than the server it is connected to.
      private const int UnknownName = unchecked((int)0x80020006);

      private readonly QuarantineReview review_ = new();

      // QuarantineEnabled, read as the Anti-spam settings page reads it: over
      // COM when there is a session, from hMailServer.ini otherwise. What an
      // empty list means depends on it.
      private readonly IniFeatureStore iniStore_ = new();

      private readonly DataGrid list_ = new()
      {
         AutoGenerateColumns = false,
         IsReadOnly = true,
         CanUserAddRows = false,
         CanUserDeleteRows = false,
         CanUserSortColumns = true,
         SelectionMode = DataGridSelectionMode.Single,
         HeadersVisibility = DataGridHeadersVisibility.Column
      };

      // MinWidth 0: the Fluent check box is a caption's control and reserves room
      // for one, which a box with no caption in a narrow column does not have.
      private readonly CheckBox selectAll_ = new() { MinWidth = 0, Width = BoxWidth, Padding = new Thickness(0), VerticalAlignment = VerticalAlignment.Center, HorizontalAlignment = HorizontalAlignment.Left };
      private readonly ComboBox grouping_ = new() { MinWidth = 150, VerticalAlignment = VerticalAlignment.Center };

      private readonly InlineNotice status_ = new() { Visibility = Visibility.Collapsed };

      // Remove expired, which deletes everything held longer than the retention
      // window and so can be pressed only while there is one: 0 keeps everything.
      private Button removeExpired_;

      // Why the selected message was held, under the list (review finding F261):
      // the row's tool tip was the one place it was said, which no keyboard
      // reaches. For the eye alone: a screen reader hears the reason in the row's
      // accessible name as the selection moves, and a live region here said it a
      // second time on every move.
      private readonly TextBlock why_ = new() { TextWrapping = TextWrapping.Wrap };
      private readonly Card whyCard_ = new() { Visibility = Visibility.Collapsed };

      // The search over what the page holds: sender, recipients, subject and why
      // each was held (QuarantineReview.Filter).
      private readonly Toolbar search_ = new();

      // What the empty state says with no search, and how many are held and were
      // listed, so that a search that matches nothing can say so and give the
      // state back when it is cleared.
      private string emptyText_ = "";
      private object emptyAction_;
      private int total_;

      // The text columns, which share what the fixed ones leave of the grid's width
      // (LayOutTextColumns_): the HELO name only where the grid is wide, and the
      // recipients where the sender's, their own and the subject's floors fit.
      private DataGridColumn heloColumn_;
      private DataGridColumn senderColumn_;
      private DataGridColumn recipientsColumn_;
      private DataGridColumn subjectColumn_;

      // What the widest sender, recipients and subject of the rows bound need,
      // measured as they are bound (MeasureTextColumns_).
      private double senderNeed_;
      private double recipientsNeed_;
      private double subjectNeed_;

      // An icon button in a cell: a 16-pixel glyph, its padding and its margin.
      private const double CellIconWidth = 16 + 4 + 4 + 1 + 1;

      // What the text columns' floors keep clear of the room, for layout rounding
      // (LayOutTextColumns_).
      private const double RoundingSlack = 2;

      // Held and Score at the width of their widest word, set as the rows are bound
      // (SizeFixedColumns_). As Auto columns they grew after the star columns had
      // shared the width out, and the grid cut Subject, its last column, to a few
      // letters.
      private DataGridColumn heldColumn_;
      private DataGridColumn scoreColumn_;
      // A neutral glyph: an empty quarantine is not a warning, and the warning
      // glyph it had read as one.
      private readonly EmptyState empty_ = new() { Icon = SymbolRegular.MailInbox24, Visibility = Visibility.Collapsed };

      // The headers and the preview open here, beside the icon that asked.
      private readonly Popup flyout_ = new() { StaysOpen = false, Placement = PlacementMode.Bottom, AllowsTransparency = true };
      private readonly TextBlock flyoutTitle_ = new() { FontWeight = FontWeights.SemiBold, VerticalAlignment = VerticalAlignment.Center, TextTrimming = TextTrimming.CharacterEllipsis };
      private readonly TextBlock flyoutNote_ = new() { TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 4, 0, 0), Visibility = Visibility.Collapsed };
      private readonly TextBox flyoutText_ = new()
      {
         IsReadOnly = true,
         AcceptsReturn = true,
         VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
         HorizontalScrollBarVisibility = ScrollBarVisibility.Auto,
         Width = 680,
         Height = 380,
         Margin = new Thickness(0, 8, 0, 0)
      };

      public QuarantineView() => Build();

      // Loading on entry rather than in the constructor, because the page cache
      // keeps this instance alive for the rest of the session: a constructor-only
      // load meant leaving and returning showed the list as it stood the FIRST
      // time the page was opened - on the one page whose list is the only copy
      // of the messages it shows.
      public void OnEnter() => Reload();

      public void OnLeave()
      {
         flyout_.IsOpen = false;
      }

      private dynamic OpenQuarantine()
      {
         dynamic settings = ServerSession.Current.Application.Settings;
         dynamic antiSpam = settings.AntiSpam;
         dynamic quarantine = antiSpam.Quarantine;

         ServerSession.Release(antiSpam);
         ServerSession.Release(settings);

         return quarantine;
      }

      /// <summary>What the last action said, at its level.</summary>
      private void Status_(StatusLevel level, string text)
      {
         ReadFailure.Clear(status_);
         status_.Level = level;
         status_.Text = text;
         status_.Visibility = Visibility.Visible;
      }

      /// <summary>A member this server may not have yet: the HELO name and the groups' domains came with schema 6105.</summary>
      private static string Optional_(Func<string> read)
      {
         try
         {
            return read() ?? "";
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            return "";
         }
      }

      private void Reload()
      {
         var messages = new List<HeldMessage>();

         try
         {
            dynamic quarantine = OpenQuarantine();
            try
            {
               quarantine.Refresh();

               int total = (int)quarantine.Count;

               for (int i = 0; ; i++)
               {
                  dynamic item;

                  try
                  {
                     item = quarantine[i];
                  }
                  catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
                  {
                     // The collection loads a page rather than the whole table, and
                     // indexing past it is how that page ends.
                     break;
                  }

                  try
                  {
                     messages.Add(new HeldMessage
                     {
                        Id = (int)item.ID,
                        CreatedTime = (string)item.CreatedTime ?? "",
                        Sender = (string)item.Sender ?? "",
                        Recipients = (string)item.Recipients ?? "",
                        Score = (int)item.Score,
                        Subject = (string)item.Subject ?? "",
                        Reason = (string)item.Reason ?? "",
                        Helo = Optional_(() => (string)item.HeloName),
                        SenderDomain = Optional_(() => (string)item.SenderDomain),
                        HeloDomain = Optional_(() => (string)item.HeloDomain)
                     });
                  }
                  finally
                  {
                     ServerSession.Release(item);
                  }
               }

               // A held time's age is counted only where the server's clock is this
               // computer's: CreatedTime is the server's local time.
               review_.AgesFrom = ServerSession.IsLocalSession ? DateTime.Now : null;
               if (heldColumn_ != null)
                  heldColumn_.Header = review_.HeldCaption;
               review_.Load(messages);
               Rebind_();

               ReadFailure.Clear(status_);

               // What an empty list means depends on the switch: a message is held
               // here only while quarantining is on, and an empty list on a server
               // whose quarantine is on is not a server whose quarantine is off.
               // The page said "switched off" of both, and never read the switch.
               bool quarantining = iniStore_.ReadBool("QuarantineEnabled", false);
               ShowSweepState_(RetentionDays_());
               empty_.Action = quarantining ? null : OpenAntiSpamSettings_();
               StatusText.Show(empty_, null, total, null, quarantining
                  ? L("Nothing is held right now. A message over the delete threshold after its body has arrived appears here; its sender is told it was accepted.")
                  : L("Nothing is held. Quarantining is switched off: until it is turned on under Anti-spam settings, spam over the delete threshold is refused during the SMTP conversation rather than stored."));
               emptyText_ = empty_.Text;
               emptyAction_ = empty_.Action;
               total_ = total;
               ShowSearchState_();

               if (total == 0)
                  status_.Visibility = Visibility.Collapsed;
               else if (messages.Count < total)
                  Status_(StatusLevel.Information, F("{0} held, showing the {1} most recent. The rest are still there - work through these, or let the retention sweep age them out.", total, messages.Count));
               else
                  Status_(StatusLevel.Information, Plural(total, "{0} message held.", "{0} messages held."));
            }
            finally
            {
               ServerSession.Release(quarantine);
            }
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            // Neither the rows of the last read nor "Nothing is held." is a fact
            // this page has now - on the one page whose list is the only copy of
            // what it shows: both go, and the notice says what happened, with Try
            // again (Services/ReadFailure).
            review_.Load(new List<HeldMessage>());
            selectAll_.IsChecked = false;
            ReadFailure.Show(status_, empty_, list_, L("Could not read the quarantine: "), ex, Reload);
         }
      }

      /// <summary>The empty state's action while quarantining is off: the page that turns it on.</summary>
      private static Wpf.Ui.Controls.Button OpenAntiSpamSettings_()
      {
         var button = new Wpf.Ui.Controls.Button { Content = L("_Open Anti-spam settings") };
         AutomationProperties.SetAutomationId(button, "quarantine-open-antispam");
         button.Click += (_, _) => (Application.Current?.MainWindow as MainWindow)?.NavigateTo("antispam");
         return button;
      }

      /// <summary>The rows again, after a regroup, a sort, a fold or a reload - with the sort arrow on its column.</summary>
      private void Rebind_()
      {
         whyCard_.Visibility = Visibility.Collapsed;
         SizeFixedColumns_();
         MeasureTextColumns_();
         list_.ItemsSource = review_.Rows;
         selectAll_.IsChecked = review_.AllChecked;

         foreach (DataGridColumn column in list_.Columns)
         {
            column.SortDirection = column.SortMemberPath is string path && review_.SortColumn is QuarantineColumn sorted && path == sorted.ToString()
               ? (review_.SortDescending ? System.ComponentModel.ListSortDirection.Descending : System.ComponentModel.ListSortDirection.Ascending)
               : null;
         }
      }

      // ---------------------------------------------------------------- the grid

      /// <summary>
      /// An icon button whose click the grid's own handler reads by its Tag, its
      /// glyph in the colour of a resource key. A key and not a brush: a value a
      /// template factory is given is frozen when the template is sealed, and the
      /// shared ThemeTokens brushes this took once then kept the theme they were
      /// frozen in - after a theme change the row and group glyphs stayed the old
      /// theme's green and red, 2.5:1 on the new card, and every field's danger
      /// ring with them. Not a tab stop: the mouse's, while the keyboard has the
      /// row's keys (WireRowKeys_).
      /// </summary>
      private static FrameworkElementFactory IconButton_(string action, SymbolRegular symbol, string colourKey, bool filled, string tipPath, string keyTipPath = null, string key = null)
      {
         var button = new FrameworkElementFactory(typeof(Wpf.Ui.Controls.Button));
         button.SetValue(FrameworkElement.TagProperty, action);
         button.SetValue(Control.IsTabStopProperty, false);
         button.SetValue(Wpf.Ui.Controls.Button.AppearanceProperty, ControlAppearance.Transparent);
         button.SetValue(Control.PaddingProperty, new Thickness(4));
         button.SetValue(FrameworkElement.MarginProperty, new Thickness(1, 0, 1, 0));
         button.SetValue(Control.BorderThicknessProperty, new Thickness(0));
         button.SetValue(FrameworkElement.VerticalAlignmentProperty, VerticalAlignment.Center);
         button.SetBinding(FrameworkElement.ToolTipProperty, new Binding(keyTipPath ?? tipPath));
         button.SetBinding(AutomationProperties.NameProperty, new Binding(tipPath));
         if (key != null)
            button.SetValue(AutomationProperties.AcceleratorKeyProperty, key);

         var icon = new FrameworkElementFactory(typeof(SymbolIcon));
         icon.SetValue(SymbolIcon.SymbolProperty, symbol);
         icon.SetValue(SymbolIcon.FilledProperty, filled);
         icon.SetValue(SymbolIcon.FontSizeProperty, 16.0);

         if (colourKey != null)
            icon.SetResourceReference(IconElement.ForegroundProperty, colourKey);

         button.AppendChild(icon);
         return button;
      }

      /// <summary>The four actions, as a row and a group carry them.</summary>
      private static FrameworkElementFactory FourActions_()
      {
         var panel = new FrameworkElementFactory(typeof(StackPanel));
         panel.SetValue(StackPanel.OrientationProperty, Orientation.Horizontal);
         panel.SetValue(FrameworkElement.WidthProperty, ActionsWidth);
         panel.SetValue(FrameworkElement.VerticalAlignmentProperty, VerticalAlignment.Center);

         panel.AppendChild(IconButton_("release", SymbolRegular.CheckmarkCircle24, KeepColourKey, false, nameof(QuarantineRow.ReleaseTip), nameof(QuarantineRow.ReleaseKeyTip), QuarantineRow.ReleaseKey));
         panel.AppendChild(IconButton_("delete", SymbolRegular.DismissCircle24, RemoveColourKey, false, nameof(QuarantineRow.DeleteTip), nameof(QuarantineRow.DeleteKeyTip), L(QuarantineRow.DeleteKey)));
         panel.AppendChild(IconButton_("allow", SymbolRegular.TextBulletListSquare24, KeepColourKey, false, nameof(QuarantineRow.AllowTip)));
         panel.AppendChild(IconButton_("block", SymbolRegular.TextBulletListSquare24, RemoveColourKey, true, nameof(QuarantineRow.BlockTip)));
         return panel;
      }

      /// <summary>A row's box: not a tab stop, Space checks the selected row.</summary>
      private static FrameworkElementFactory CheckBox_(double width, Thickness margin)
      {
         var box = new FrameworkElementFactory(typeof(CheckBox));
         box.SetValue(FrameworkElement.TagProperty, "check");
         box.SetValue(Control.IsTabStopProperty, false);
         box.SetValue(FrameworkElement.WidthProperty, width);
         box.SetValue(FrameworkElement.MinWidthProperty, 0.0);
         box.SetValue(Control.PaddingProperty, new Thickness(0));
         box.SetValue(FrameworkElement.HorizontalAlignmentProperty, HorizontalAlignment.Left);
         box.SetValue(FrameworkElement.MarginProperty, margin);
         box.SetValue(FrameworkElement.VerticalAlignmentProperty, VerticalAlignment.Center);
         box.SetBinding(ToggleButton.IsCheckedProperty, new Binding(nameof(QuarantineRow.IsChecked)) { Mode = BindingMode.OneWay });
         box.SetBinding(FrameworkElement.ToolTipProperty, new Binding(nameof(QuarantineRow.CheckKeyTip)));
         box.SetBinding(AutomationProperties.NameProperty, new Binding(nameof(QuarantineRow.CheckTip)));
         box.SetValue(AutomationProperties.AcceleratorKeyProperty, L(QuarantineRow.SpaceKey));
         return box;
      }

      /// <summary>A trimmed line of text whose tooltip is the whole value.</summary>
      private static FrameworkElementFactory Trimmed_(string textPath, string tipPath)
      {
         var text = new FrameworkElementFactory(typeof(TextBlock));
         text.SetBinding(TextBlock.TextProperty, new Binding(textPath));
         text.SetBinding(FrameworkElement.ToolTipProperty, new Binding(tipPath));
         text.SetValue(TextBlock.TextTrimmingProperty, TextTrimming.CharacterEllipsis);
         text.SetValue(FrameworkElement.VerticalAlignmentProperty, VerticalAlignment.Center);
         return text;
      }

      /// <summary>
      /// The headers and the preview before the subject: the two looks at the message
      /// itself. The headers' icon was the HELO column's, which the grid leaves out
      /// where it is narrow (LayOutTextColumns_), so it would have gone with it.
      /// </summary>
      private static FrameworkElementFactory SubjectCell_()
      {
         var dock = new FrameworkElementFactory(typeof(DockPanel));
         dock.SetValue(DockPanel.LastChildFillProperty, true);

         foreach ((string action, SymbolRegular symbol, string tipPath, string keyTipPath, string key) in new[]
         {
            ("headers", SymbolRegular.DocumentHeader24, nameof(QuarantineRow.HeadersTip), nameof(QuarantineRow.HeadersKeyTip), QuarantineRow.HeadersKey),
            ("preview", SymbolRegular.Glance24, nameof(QuarantineRow.PreviewTip), nameof(QuarantineRow.PreviewKeyTip), QuarantineRow.PreviewKey)
         })
         {
            FrameworkElementFactory button = IconButton_(action, symbol, null, false, tipPath, keyTipPath, key);
            button.SetValue(DockPanel.DockProperty, Dock.Left);
            dock.AppendChild(button);
         }

         dock.AppendChild(Trimmed_(nameof(QuarantineRow.Subject), nameof(QuarantineRow.SubjectTip)));
         return dock;
      }

      private static DataGridTemplateColumn TemplateColumn_(object header, FrameworkElementFactory cell, DataGridLength width, string sortPath)
      {
         return new DataGridTemplateColumn
         {
            Header = header,
            CellTemplate = new DataTemplate { VisualTree = cell },
            Width = width,
            CanUserSort = sortPath != null,
            SortMemberPath = sortPath
         };
      }

      /// <summary>
      /// A group's header row: its box, its four icons, the fold chevron, the domain
      /// and how many it holds, across the whole width. The box and the icons line
      /// up with the message rows' by the two fixed widths.
      ///
      /// Selected, it shows the brand pill, the selection fill and the High
      /// Contrast outline the grid's other rows show (App.xaml's DataGridRow), and
      /// a ring in the focus colour while the keyboard is on it, as a list's row
      /// has: the keyboard reaches the row itself now, and a template of its own
      /// showed neither.
      /// </summary>
      private static ControlTemplate GroupRowTemplate_()
      {
         var root = new FrameworkElementFactory(typeof(Grid));

         var border = new FrameworkElementFactory(typeof(Border)) { Name = "Bd" }; // no-loc: a template part's name
         border.SetResourceReference(Border.BackgroundProperty, "ControlFillColorSecondaryBrush");
         border.SetResourceReference(Border.BorderBrushProperty, "ControlStrokeColorDefaultBrush");
         border.SetValue(Border.BorderThicknessProperty, new Thickness(0, 0, 0, 1));
         border.SetValue(Border.PaddingProperty, new Thickness(0, 2, 0, 2));

         var line = new FrameworkElementFactory(typeof(StackPanel));
         line.SetValue(StackPanel.OrientationProperty, Orientation.Horizontal);

         line.AppendChild(CheckBox_(BoxWidth, new Thickness(8, 0, CheckWidth - BoxWidth - 8, 0)));
         line.AppendChild(FourActions_());

         var fold = new FrameworkElementFactory(typeof(Wpf.Ui.Controls.Button));
         fold.SetValue(FrameworkElement.TagProperty, "fold");
         fold.SetValue(Control.IsTabStopProperty, false);
         fold.SetValue(Wpf.Ui.Controls.Button.AppearanceProperty, ControlAppearance.Transparent);
         fold.SetValue(Control.PaddingProperty, new Thickness(4));
         fold.SetValue(Control.BorderThicknessProperty, new Thickness(0));
         fold.SetBinding(FrameworkElement.ToolTipProperty, new Binding(nameof(QuarantineRow.FoldTip)));
         fold.SetBinding(AutomationProperties.NameProperty, new Binding(nameof(QuarantineRow.FoldTip)));

         var chevron = new FrameworkElementFactory(typeof(SymbolIcon));
         chevron.SetBinding(SymbolIcon.SymbolProperty, new Binding(nameof(QuarantineRow.IsExpanded)) { Converter = new ChevronConverter() });
         chevron.SetValue(SymbolIcon.FontSizeProperty, 14.0);
         fold.AppendChild(chevron);
         line.AppendChild(fold);

         var title = new FrameworkElementFactory(typeof(TextBlock));
         title.SetBinding(TextBlock.TextProperty, new Binding(nameof(QuarantineRow.GroupTitle)));
         title.SetValue(TextBlock.FontWeightProperty, FontWeights.SemiBold);
         title.SetValue(FrameworkElement.MarginProperty, new Thickness(6, 0, 0, 0));
         title.SetValue(FrameworkElement.VerticalAlignmentProperty, VerticalAlignment.Center);
         line.AppendChild(title);

         var count = new FrameworkElementFactory(typeof(TextBlock));
         count.SetBinding(TextBlock.TextProperty, new Binding(nameof(QuarantineRow.GroupCount)));
         count.SetResourceReference(TextBlock.ForegroundProperty, "TextFillColorSecondaryBrush");
         count.SetValue(FrameworkElement.MarginProperty, new Thickness(10, 0, 0, 0));
         count.SetValue(FrameworkElement.VerticalAlignmentProperty, VerticalAlignment.Center);
         line.AppendChild(count);

         border.AppendChild(line);
         root.AppendChild(border);

         var pill = new FrameworkElementFactory(typeof(System.Windows.Shapes.Rectangle)) { Name = "Pill" }; // no-loc: a template part's name
         pill.SetValue(FrameworkElement.WidthProperty, 3.0);
         pill.SetValue(FrameworkElement.HeightProperty, 16.0);
         pill.SetValue(System.Windows.Shapes.Rectangle.RadiusXProperty, 1.5);
         pill.SetValue(System.Windows.Shapes.Rectangle.RadiusYProperty, 1.5);
         pill.SetValue(FrameworkElement.HorizontalAlignmentProperty, HorizontalAlignment.Left);
         pill.SetValue(FrameworkElement.VerticalAlignmentProperty, VerticalAlignment.Center);
         pill.SetValue(FrameworkElement.MarginProperty, new Thickness(1, 0, 0, 0));
         pill.SetResourceReference(System.Windows.Shapes.Shape.FillProperty, "AppBrandBrush");
         pill.SetValue(UIElement.OpacityProperty, 0.0);
         pill.SetValue(UIElement.IsHitTestVisibleProperty, false);
         root.AppendChild(pill);

         var outline = new FrameworkElementFactory(typeof(System.Windows.Shapes.Rectangle)) { Name = "Outline" }; // no-loc: a template part's name
         outline.SetResourceReference(System.Windows.Shapes.Shape.StrokeProperty, "AppSelectionOutlineBrush");
         outline.SetValue(System.Windows.Shapes.Shape.StrokeThicknessProperty, 1.0);
         outline.SetValue(UIElement.VisibilityProperty, Visibility.Collapsed);
         outline.SetValue(UIElement.IsHitTestVisibleProperty, false);
         root.AppendChild(outline);

         var ring = new FrameworkElementFactory(typeof(System.Windows.Shapes.Rectangle)) { Name = "Ring" }; // no-loc: a template part's name
         ring.SetResourceReference(System.Windows.Shapes.Shape.StrokeProperty, "AppFocusBrush");
         ring.SetValue(System.Windows.Shapes.Shape.StrokeThicknessProperty, 1.5);
         ring.SetValue(UIElement.VisibilityProperty, Visibility.Collapsed);
         ring.SetValue(UIElement.IsHitTestVisibleProperty, false);
         root.AppendChild(ring);

         var template = new ControlTemplate(typeof(DataGridRow)) { VisualTree = root };

         var selected = new Trigger { Property = DataGridRow.IsSelectedProperty, Value = true };
         selected.Setters.Add(new Setter(UIElement.OpacityProperty, 1.0, "Pill")); // no-loc: a template part's name
         selected.Setters.Add(new Setter(UIElement.VisibilityProperty, Visibility.Visible, "Outline")); // no-loc: a template part's name
         selected.Setters.Add(new Setter(Border.BackgroundProperty, new DynamicResourceExtension("AppSelectionFillBrush"), "Bd")); // no-loc: a template part's name
         template.Triggers.Add(selected);

         var focused = new Trigger { Property = UIElement.IsKeyboardFocusedProperty, Value = true };
         focused.Setters.Add(new Setter(UIElement.VisibilityProperty, Visibility.Visible, "Ring")); // no-loc: a template part's name
         template.Triggers.Add(focused);

         return template;
      }

      /// <summary>A folded group points right, an open one down.</summary>
      private sealed class ChevronConverter : IValueConverter
      {
         public object Convert(object value, Type targetType, object parameter, CultureInfo culture)
         {
            return value is true ? SymbolRegular.ChevronDown24 : SymbolRegular.ChevronRight24;
         }

         public object ConvertBack(object value, Type targetType, object parameter, CultureInfo culture)
         {
            throw new NotSupportedException();
         }
      }

      /// <summary>
      /// The box, the four actions, then Held, HELO, Sender, Recipients, Score and
      /// Subject. Why a message was held is the row's tooltip, because the reasons
      /// are sentences. A long sender or HELO name ends in an ellipsis and its
      /// tooltip is the whole of it; several recipients show the first and how many
      /// more, and the tooltip lists them all.
      ///
      /// Header sort is kept - Score is the field worth sorting by - and a sort
      /// orders the messages inside each group rather than scattering the groups.
      /// </summary>
      private void BuildColumns()
      {
         selectAll_.Click += (_, _) => { review_.ToggleAll(); selectAll_.IsChecked = review_.AllChecked; };
         selectAll_.ToolTip = L("Check every message");
         AutomationProperties.SetName(selectAll_, L("Check every message"));

         list_.Columns.Add(TemplateColumn_(selectAll_, CheckBox_(BoxWidth, new Thickness(0)), new DataGridLength(CheckWidth), null));

         var headerActions = new StackPanel { Orientation = Orientation.Horizontal };
         headerActions.Children.Add(HeaderButton_(SymbolRegular.CheckmarkCircle24, KeepColourKey, false, L("Release the checked messages"), () => Act_(review_.CheckedIds, "release", checkedSet: true)));
         headerActions.Children.Add(HeaderButton_(SymbolRegular.DismissCircle24, RemoveColourKey, false, L("Delete the checked messages"), () => Act_(review_.CheckedIds, "delete")));
         headerActions.Children.Add(HeaderButton_(SymbolRegular.TextBulletListSquare24, KeepColourKey, false, L("Allow the checked messages' senders, then release"), () => Act_(review_.CheckedIds, "allow")));
         headerActions.Children.Add(HeaderButton_(SymbolRegular.TextBulletListSquare24, RemoveColourKey, true, L("Block the checked messages' senders, then delete"), () => Act_(review_.CheckedIds, "block")));

         list_.Columns.Add(TemplateColumn_(headerActions, FourActions_(), new DataGridLength(ActionsWidth), null));

         var held = new DataGridTextColumn
         {
            Header = L("Held"),
            Binding = new Binding(nameof(QuarantineRow.Held)),
            SortMemberPath = nameof(QuarantineColumn.Held),
            // Auto, not SizeToCells. SizeToCells measures the CELLS only, so on an
            // empty grid the column collapses to zero and its header disappears
            // with it - which is how Quarantine shipped a table showing Sender,
            // Recipients and Subject while Held and Score were simply absent until
            // the first row arrived. Auto is max(header, cells), so the column is
            // never narrower than the word naming it.
            Width = DataGridLength.Auto
         };
         // How long ago, or the reader's date and time; the age in words and the
         // whole time, as the server's, in the tool tip.
         var heldText = new Style(typeof(TextBlock));
         heldText.Setters.Add(new Setter(ToolTipProperty, new Binding(nameof(QuarantineRow.HeldTip))));
         held.ElementStyle = heldText;
         heldColumn_ = held;
         list_.Columns.Add(held);

         // The text columns share what the fixed ones leave, Sender twice, Recipients
         // one and a half times and Subject two and a half times the HELO name, each
         // floored as LayOutTextColumns_ works out: at 1280 the four shared alike and
         // the addresses were cut ("sender0@mas...") while Subject had room to spare
         // (review finding F261).
         heloColumn_ = TemplateColumn_(L("HELO"),
            Trimmed_(nameof(QuarantineRow.Helo), nameof(QuarantineRow.HeloTip)),
            new DataGridLength(1, DataGridLengthUnitType.Star), nameof(QuarantineColumn.Helo));
         list_.Columns.Add(heloColumn_);

         senderColumn_ = TemplateColumn_(L("Sender"),
            Trimmed_(nameof(QuarantineRow.Sender), nameof(QuarantineRow.SenderTip)),
            new DataGridLength(2, DataGridLengthUnitType.Star), nameof(QuarantineColumn.Sender));
         list_.Columns.Add(senderColumn_);

         recipientsColumn_ = TemplateColumn_(L("Recipients"),
            Trimmed_(nameof(QuarantineRow.Recipients), nameof(QuarantineRow.RecipientsTip)),
            new DataGridLength(1.5, DataGridLengthUnitType.Star), nameof(QuarantineColumn.Recipients));
         list_.Columns.Add(recipientsColumn_);

         var score = new DataGridTextColumn
         {
            Header = L("Score"),
            Binding = new Binding(nameof(QuarantineRow.Score)),
            SortMemberPath = nameof(QuarantineColumn.Score),
            Width = DataGridLength.Auto
         };
         GridStyles.Number(score);
         scoreColumn_ = score;
         list_.Columns.Add(score);

         subjectColumn_ = TemplateColumn_(L("Subject"), SubjectCell_(),
            new DataGridLength(2.5, DataGridLengthUnitType.Star), nameof(QuarantineColumn.Subject));
         list_.Columns.Add(subjectColumn_);

         // Why this message was held, which is the question the page exists to
         // answer and has no column of its own because the reasons are sentences.
         // BasedOn the application's implicit style, because assigning RowStyle
         // replaces the implicit one rather than adding to it. A group's row is
         // drawn by a template of its own, across the whole width.
         var rowStyle = new Style(typeof(DataGridRow));

         if (Application.Current?.TryFindResource(typeof(DataGridRow)) is Style appRowStyle)
            rowStyle.BasedOn = appRowStyle;

         rowStyle.Setters.Add(new Setter(ToolTipProperty, new Binding(nameof(QuarantineRow.HeldBecause))));
         rowStyle.Setters.Add(new Setter(AutomationProperties.NameProperty, new Binding(nameof(QuarantineRow.AccessibleName))));

         var groupRow = new DataTrigger { Binding = new Binding(nameof(QuarantineRow.IsGroup)), Value = true };
         groupRow.Setters.Add(new Setter(TemplateProperty, GroupRowTemplate_()));
         // A group's row has no cells, so the row itself takes the keyboard -
         // from the arrow keys and the rows' one tab stop, not as a stop of its own.
         groupRow.Setters.Add(new Setter(FocusableProperty, true));
         groupRow.Setters.Add(new Setter(IsTabStopProperty, false));
         // Its template draws the ring (GroupRowTemplate_).
         groupRow.Setters.Add(new Setter(FocusVisualStyleProperty, null));
         rowStyle.Triggers.Add(groupRow);

         list_.RowStyle = rowStyle;

         list_.Sorting += (_, e) =>
         {
            e.Handled = true;

            if (e.Column.SortMemberPath is string path && Enum.TryParse(path, out QuarantineColumn column))
            {
               review_.SortBy(column);
               Rebind_();
            }
         };

         list_.AddHandler(ButtonBase.ClickEvent, new RoutedEventHandler(OnRowClick_));
         WireRowKeys_();
      }

      /// <summary>
      /// The rows are one tab stop, after the header's box and its four actions
      /// on the checked messages, Up and Down move between them, groups
      /// included, and the selected row answers the keyboard: Space checks it, R
      /// releases it, Delete deletes it after asking, P or Enter previews its text
      /// and H shows its headers - Enter folds or unfolds a group, whose R and
      /// Delete act on every message in it. Each row carried
      /// about seven tab stops - its box, four actions and two icons - and no key
      /// did what they do (review finding F211). The row's box and icons are the
      /// mouse's now, and their tool tips name the keys; a row's whitelist and
      /// blacklist are its box and the header's two actions on the checked
      /// messages (GridKeys.RowsAreOneTabStop).
      /// </summary>
      private void WireRowKeys_()
      {
         GridKeys.RowsAreOneTabStop(list_);

         list_.PreviewKeyDown += (_, e) =>
         {
            if (e.Handled || Keyboard.Modifiers != ModifierKeys.None || e.OriginalSource is TextBoxBase)
               return;
            if (list_.SelectedItem is not QuarantineRow row)
               return;

            // The rows' keys, from a row or a cell: Space on the header's box,
            // or on a box or an icon a click left the keyboard on, is that
            // control's own.
            if (GridKeys.RowAt(list_, e.OriginalSource as DependencyObject) == null)
               return;

            string groupTitle = row.IsGroup ? row.GroupTitle : null;
            switch (e.Key)
            {
               // From row to row, a group's included: the grid moves the keyboard
               // only between cells, and a group's row has none, so Down on a
               // group stayed where it was.
               case Key.Up:
               case Key.Down:
                  int next = list_.Items.IndexOf(row) + (e.Key == Key.Down ? 1 : -1);
                  if (next >= 0 && next < list_.Items.Count)
                  {
                     list_.SelectedIndex = next;
                     GridKeys.FocusSelectedRow(list_);
                  }
                  break;

               case Key.Space:
                  review_.Toggle(row);
                  selectAll_.IsChecked = review_.AllChecked;
                  break;

               case Key.R:
                  Act_(QuarantineReview.IdsOf(row), "release", groupTitle);
                  FocusRowAfter_();
                  break;

               case Key.Delete:
                  Act_(QuarantineReview.IdsOf(row), "delete", groupTitle);
                  FocusRowAfter_();
                  break;

               case Key.P when row.IsMessage:
               case Key.Enter when row.IsMessage:
                  ShowPreview_(row, list_.ItemContainerGenerator.ContainerFromItem(row) as FrameworkElement ?? list_);
                  break;

               case Key.H when row.IsMessage:
                  ShowHeaders_(row, list_.ItemContainerGenerator.ContainerFromItem(row) as FrameworkElement ?? list_);
                  break;

               case Key.Enter:
                  review_.ToggleFold(row);
                  Rebind_();
                  // The group stays the selected row, so that the next Enter
                  // undoes this one: the rows rebuilt dropped the selection.
                  list_.SelectedItem = review_.Rows.FirstOrDefault(r => r.IsGroup && r.GroupKey == row.GroupKey);
                  FocusRowAfter_();
                  break;

               default:
                  return;
            }

            e.Handled = true;
         };
      }

      /// <summary>The keyboard back on the selected row once the list has been rebuilt.</summary>
      private void FocusRowAfter_()
         => Dispatcher.BeginInvoke(System.Windows.Threading.DispatcherPriority.Loaded, new Action(() => GridKeys.FocusSelectedRow(list_)));

      private static Button HeaderButton_(SymbolRegular symbol, string colourKey, bool filled, string tip, Action onClick)
      {
         // The glyph's colour by its key, as the rows' (IconButton_): a held brush
         // keeps the theme it was taken in.
         var icon = new SymbolIcon { Symbol = symbol, Filled = filled, FontSize = 16 };
         icon.SetResourceReference(IconElement.ForegroundProperty, colourKey);

         var button = new Wpf.Ui.Controls.Button
         {
            Appearance = ControlAppearance.Transparent,
            Padding = new Thickness(4),
            Margin = new Thickness(1, 0, 1, 0),
            BorderThickness = new Thickness(0),
            ToolTip = tip,
            Content = icon
         };

         AutomationProperties.SetName(button, tip);
         button.Click += (_, e) => { e.Handled = true; onClick(); };
         return button;
      }

      /// <summary>Every icon and box on a row, and a group's fold, arrive here by their Tag.</summary>
      private void OnRowClick_(object sender, RoutedEventArgs e)
      {
         if (e.OriginalSource is not FrameworkElement element || element.DataContext is not QuarantineRow row || element.Tag is not string action)
            return;

         e.Handled = true;

         switch (action)
         {
            case "check":
               review_.Toggle(row);
               selectAll_.IsChecked = review_.AllChecked;
               break;

            case "fold":
               review_.ToggleFold(row);
               Rebind_();
               break;

            case "headers":
               ShowHeaders_(row, element);
               break;

            case "preview":
               ShowPreview_(row, element);
               break;

            default:
               Act_(QuarantineReview.IdsOf(row), action, row.IsGroup ? row.GroupTitle : null);
               break;
         }
      }

      // ------------------------------------------------------------- the actions

      /// <summary>
      /// Quarantine.Bulk, or - on a server from before it, which knew only one
      /// message at a time - null, and the caller falls back to the old calls for
      /// the two actions that had them.
      /// </summary>
      private BulkAnswer Bulk_(string request)
      {
         dynamic quarantine = OpenQuarantine();
         try
         {
            string answer = (string)quarantine.Bulk(request);
            BulkAnswer read = QuarantineReview.ReadBulkAnswer(answer);

            if (read == null)
               throw new InvalidOperationException(L("The server's answer could not be read."));

            return read;
         }
         catch (COMException ex) when (ex.HResult == UnknownName)
         {
            return null;
         }
         finally
         {
            ServerSession.Release(quarantine);
         }
      }

      /// <summary>The server before Quarantine.Bulk: one message at a time, as the page did then.</summary>
      private BulkAnswer OneAtATime_(IReadOnlyList<int> ids, bool release)
      {
         var results = new List<BulkResult>();

         dynamic quarantine = OpenQuarantine();
         try
         {
            foreach (int id in ids)
            {
               try
               {
                  if (release)
                     quarantine.ReleaseByDBID(id);
                  else
                     quarantine.DeleteByDBID(id);

                  results.Add(new BulkResult { Id = id, Ok = true });
               }
               catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
               {
                  results.Add(new BulkResult { Id = id, Ok = false, Error = ServerSession.DescribeComError(ex) });
               }
            }
         }
         finally
         {
            ServerSession.Release(quarantine);
         }

         var answer = new BulkAnswer { Succeeded = results.Count(r => r.Ok), Failed = results.Count(r => !r.Ok) };
         answer.Results.AddRange(results);
         return answer;
      }

      /// <summary>
      /// One of the four actions over a set of messages: one row's, one group's, or
      /// every checked one. Delete asks once, and No is the default - the sender was
      /// told 250, so nothing will retry and there is no other copy. Whitelist and
      /// blacklist ask which list, and show what will be written before anything is.
      /// </summary>
      private void Act_(IReadOnlyList<int> ids, string action, string groupTitle = null, bool checkedSet = false)
      {
         if (ids == null || ids.Count == 0)
         {
            Status_(StatusLevel.Information, L("Check the messages first: the header's icons act on every checked message."));
            return;
         }

         try
         {
            BulkAnswer answer;

            switch (action)
            {
               case "delete":
                  if (!ConfirmDelete_(ids, groupTitle))
                     return;

                  answer = Bulk_(QuarantineReview.BulkRequest(ids, "delete")) ?? OneAtATime_(ids, false);
                  break;

               case "release":
                  // A group's or the checked messages' release asks first, however
                  // many it holds: each is delivered at once, and a release cannot
                  // be taken back. One message's own icon releases that message.
                  if ((checkedSet || groupTitle != null || ids.Count > 1) && !ConfirmRelease_(ids.Count, groupTitle))
                     return;

                  answer = Bulk_(QuarantineReview.BulkRequest(ids, "release")) ?? OneAtATime_(ids, true);
                  break;

               case "allow":
               case "block":
                  {
                     bool heloGrouping = review_.Grouping == QuarantineGrouping.HeloDomain;
                     var dialog = new QuarantineListDialog(Window.GetWindow(this), action == "block", ids, heloGrouping, Bulk_);

                     if (dialog.ShowDialog() != true)
                        return;

                     answer = Bulk_(QuarantineReview.BulkRequest(ids, action, dialog.ListWord,
                        dialog.ListWord == "domain" ? (heloGrouping ? "helo" : "sender") : null, false, dialog.ConfirmPublicSuffix));

                     if (answer == null)
                     {
                        Status_(StatusLevel.Critical, L("This server is older than the Control Panel and cannot allow or block a sender from the quarantine. Upgrade the server."));
                        return;
                     }

                     break;
                  }

               default:
                  return;
            }

            Reload();
            Report_(action, answer);
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            Reload();
            Status_(StatusLevel.Critical, ServerSession.DescribeComError(ex));
         }
      }

      private bool ConfirmDelete_(IReadOnlyList<int> ids, string groupTitle)
      {
         // Named Delete and drawn as the destructive act it is, like every other
         // destructive confirmation in the application (revoke key, delete folder,
         // empty mailbox), and Cancel takes Enter: for an action with no undo and
         // no other copy, the keyboard default declines. One message - a row's, or
         // a group's or the checked set's that holds one - is named by its subject
         // and its sender, as the queue's question names its message, where it was
         // "this message"; "this message" is left for one the list no longer holds.
         int count = ids.Count;
         HeldMessage one = count == 1 ? review_.MessagesWith(ids).FirstOrDefault() : null;
         string question = count == 1
            ? one != null
               ? Confirmations.DeleteQuarantined(one.Subject, one.Sender)
               : L("Delete this message permanently?\n\nThe sender was told it was accepted, so nothing will retry and there is no other copy.")
            : groupTitle != null
               ? PluralF(count,
                  "Delete the {0} message from {1} permanently?\n\nThe sender was told it was accepted, so nothing will retry and there is no other copy.",
                  "Delete the {0} messages from {1} permanently?\n\nThe senders were told they were accepted, so nothing will retry and there is no other copy.",
                  count, groupTitle)
               : Plural(count,
                  "Delete the {0} checked message permanently?\n\nThe sender was told it was accepted, so nothing will retry and there is no other copy.",
                  "Delete the {0} checked messages permanently?\n\nThe senders were told they were accepted, so nothing will retry and there is no other copy.");

         return MessageBox.Confirm(question, L("Delete quarantined message"), L("_Delete"), destructive: true);
      }

      private static bool ConfirmRelease_(int count, string groupTitle)
         => MessageBox.Confirm(Confirmations.ReleaseQuarantined(count, groupTitle), L("Control Panel"), L("_Release"), cancelByDefault: true);

      /// <summary>What happened, in one line, with the first reason a message was left where it was.</summary>
      private void Report_(string action, BulkAnswer answer)
      {
         int done = answer.Succeeded;
         int failed = answer.Failed;
         int entries = answer.Entries.Count(e => e.Error.Length == 0);

         string said = action switch
         {
            "release" => Plural(done, "Released {0} message to its recipients.", "Released {0} messages to their recipients."),
            "delete" => Plural(done, "Deleted {0} message.", "Deleted {0} messages."),
            "allow" => F("Allowed. List entries: {0}. Messages released: {1}.", entries, done),
            _ => F("Blocked. List entries: {0}. Messages deleted: {1}.", entries, done)
         };

         if (failed == 0)
         {
            Status_(StatusLevel.Good, said);
            return;
         }

         string why = answer.Results.FirstOrDefault(r => !r.Ok)?.Error ?? "";
         Status_(done == 0 ? StatusLevel.Critical : StatusLevel.Warning,
            Sentences(said, F("Left where they were: {0}. The first reason: {1}", failed, why)));
      }

      // ------------------------------------------------ the headers and the preview

      private void ShowHeaders_(QuarantineRow row, FrameworkElement anchor)
      {
         Read_(row, anchor, true);
      }

      private void ShowPreview_(QuarantineRow row, FrameworkElement anchor)
      {
         Read_(row, anchor, false);
      }

      /// <summary>
      /// Reads one held message's headers or its text, straight from the held file,
      /// and shows it in the flyout beside the icon. Plain text either way: the
      /// preview is the text part, or the HTML part with the markup taken out on the
      /// server, and nothing here renders HTML.
      /// </summary>
      private void Read_(QuarantineRow row, FrameworkElement anchor, bool headers)
      {
         if (row?.Message == null)
            return;

         try
         {
            string text;
            dynamic quarantine = OpenQuarantine();
            try
            {
               dynamic item = quarantine.ItemByDBID[row.Id];
               try
               {
                  text = headers ? (string)item.ReadHeaders() : (string)item.ReadPreview();
               }
               finally
               {
                  ServerSession.Release(item);
               }
            }
            finally
            {
               ServerSession.Release(quarantine);
            }

            text ??= "";

            int limit = headers ? 65536 : 16384;
            flyoutTitle_.Text = headers ? F("Headers of the message from {0}", row.Sender) : F("Preview of \"{0}\"", row.Subject);

            string note = text.Length >= limit
               ? F("Only the first {0} characters are shown.", limit.ToString("N0", CultureInfo.CurrentCulture))
               : !headers && text.Length == 0 ? L("The message has no text to show.") : null;

            flyoutNote_.Text = note ?? "";
            flyoutNote_.Visibility = note == null ? Visibility.Collapsed : Visibility.Visible;

            flyoutText_.Text = text;
            flyoutText_.FontFamily = headers ? new FontFamily(Typography.MonoFontFamily) : new FontFamily(Typography.UiFontFamily);
            flyoutText_.TextWrapping = headers ? TextWrapping.NoWrap : TextWrapping.Wrap;
            flyoutText_.ScrollToHome();
            AutomationProperties.SetName(flyoutText_, flyoutTitle_.Text);

            flyout_.PlacementTarget = anchor;
            flyout_.IsOpen = true;
            flyoutText_.Focus();
         }
         catch (COMException ex) when (ex.HResult == UnknownName)
         {
            Status_(StatusLevel.Critical, L("This server is older than the Control Panel and cannot show a held message's headers or text. Upgrade the server."));
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            Status_(StatusLevel.Critical, ServerSession.DescribeComError(ex));
         }
      }

      private void BuildFlyout()
      {
         var close = new Wpf.Ui.Controls.Button
         {
            Appearance = ControlAppearance.Transparent,
            Padding = new Thickness(4),
            BorderThickness = new Thickness(0),
            ToolTip = L("Close"),
            Content = new SymbolIcon { Symbol = SymbolRegular.Dismiss24, FontSize = 14 }
         };
         AutomationProperties.SetName(close, L("Close"));
         close.Click += (_, _) => flyout_.IsOpen = false;

         var heading = new DockPanel();
         DockPanel.SetDock(close, Dock.Right);
         heading.Children.Add(close);
         heading.Children.Add(flyoutTitle_);

         var body = new StackPanel();
         body.Children.Add(heading);
         body.Children.Add(flyoutNote_);
         body.Children.Add(flyoutText_);

         var frame = new Border
         {
            Padding = new Thickness(12),
            BorderThickness = new Thickness(1),
            CornerRadius = new CornerRadius(8),
            Child = body
         };
         frame.SetResourceReference(Border.BackgroundProperty, "ApplicationBackgroundBrush");
         frame.SetResourceReference(Border.BorderBrushProperty, "ControlStrokeColorDefaultBrush");
         flyoutTitle_.SetResourceReference(TextBlock.ForegroundProperty, "TextFillColorPrimaryBrush");
         flyoutNote_.SetResourceReference(TextBlock.ForegroundProperty, "TextFillColorSecondaryBrush");

         flyout_.Child = frame;
         flyout_.PreviewKeyDown += (_, e) =>
         {
            if (e.Key != Key.Escape)
               return;

            e.Handled = true;
            flyout_.IsOpen = false;
         };
         flyout_.Closed += (_, _) => (flyout_.PlacementTarget as UIElement)?.Focus();
      }

      // ------------------------------------------------------------ the toolbar

      /// <summary>
      /// QuarantineRetentionDays, read as the Anti-spam settings page reads it; 30, the
      /// server's default, when it cannot be read or parsed.
      /// </summary>
      private int RetentionDays_()
         => int.TryParse(iniStore_.Read("QuarantineRetentionDays", "30").Trim(), out int days) ? days : 30;

      /// <summary>Remove expired can be pressed while there is a retention window, and says why not otherwise.</summary>
      private void ShowSweepState_(int days)
      {
         if (removeExpired_ == null)
            return;

         removeExpired_.IsEnabled = days > 0;
         removeExpired_.ToolTip = days > 0 ? null : Confirmations.NothingExpires;
         ToolTipService.SetShowOnDisabled(removeExpired_, true);
      }

      /// <summary>
      /// Deletes every message held longer than the retention window, after asking,
      /// with the window named and Cancel the answer Enter gives, where it ran at the
      /// press (review finding F184); with a window of 0, which keeps everything, it
      /// asks nothing and says so.
      /// </summary>
      private void SweepExpired()
      {
         int days = RetentionDays_();
         ShowSweepState_(days);
         if (days <= 0)
         {
            Status_(StatusLevel.Information, Confirmations.NothingExpires);
            return;
         }

         if (!MessageBox.Confirm(Confirmations.DeleteExpiredQuarantine(days), L("Delete quarantined message"), L("_Delete"), destructive: true))
            return;

         try
         {
            dynamic quarantine = OpenQuarantine();
            int removed;
            try
            {
               removed = (int)quarantine.DeleteExpired();
            }
            finally
            {
               ServerSession.Release(quarantine);
            }

            Reload();

            if (removed == 0)
               Status_(StatusLevel.Information, L("Nothing was old enough to remove. The window is set on the Anti-spam settings page (Delete quarantined messages after), and 0 means never."));
            else
               Status_(StatusLevel.Good, Plural(removed, "{0} message past the retention window was removed.", "{0} messages past the retention window were removed."));
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            Status_(StatusLevel.Critical, ServerSession.DescribeComError(ex));
         }
      }

      /// <summary>
      /// The two server-wide settings the quarantine digest needs, which live in
      /// hm_settings and are reached through the COM Settings object: the address
      /// the release links point at, and how long a link stays usable. Whether a
      /// domain sends a digest, and at what hour, is set on the domain.
      ///
      /// The generated field dialog rather than a page of its own: two values do
      /// not earn a navigation entry, and this is the page an administrator is on
      /// when the question "why did nobody get a digest" comes up.
      /// </summary>
      private void EditDigestSettings()
      {
         dynamic settings = null;

         try
         {
            settings = ServerSession.Current.Application.Settings;

            var current = new CollectionEditorView.Row();
            current.Values["QuarantineDigestBaseUrl"] = (string)settings.QuarantineDigestBaseUrl ?? "";
            current.Values["QuarantineDigestLinkHours"] = (int)settings.QuarantineDigestLinkHours;

            var spec = new CollectionEditorView.CollectionSpec
            {
               ItemNoun = L("digest settings"),
               EditTitle = N("Edit the digest settings"),
               Fields =
               {
                  new CollectionEditorView.FieldSpec { Prop = "QuarantineDigestBaseUrl", Label = L("Address the release links point at, such as https://mail.example.com (empty sends no digest)") },
                  new CollectionEditorView.FieldSpec { Prop = "QuarantineDigestLinkHours", Label = L("Hours a release link stays usable (1 to 720)"), Kind = CollectionEditorView.FieldKind.Number }
               }
            };

            var dialog = new FieldDialog(spec, current, Window.GetWindow(this));
            if (dialog.ShowDialog() != true)
               return;

            // Written in this order so that a refused address - the server insists
            // on an absolute http or https URL - stops before the hours are
            // touched, and the message says which of the two was wrong.
            settings.QuarantineDigestBaseUrl = ((string)dialog.Result["QuarantineDigestBaseUrl"] ?? "").Trim();
            settings.QuarantineDigestLinkHours = (int)dialog.Result["QuarantineDigestLinkHours"];

            Status_(StatusLevel.Good, L("Digest settings saved. Which domains send a digest, and at what hour, is set on each domain's Quarantine digest tab."));
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            Status_(StatusLevel.Critical, ServerSession.DescribeComError(ex));
         }
         finally
         {
            ServerSession.Release((object)settings);
         }
      }

      private void Build()
      {
         var root = new Grid();
         root.SetResourceReference(MarginProperty, "AppPagePadding");
         root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
         root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });

         grouping_.Items.Add(L("Sender domain"));
         grouping_.Items.Add(L("HELO domain"));
         grouping_.Items.Add(L("None"));
         grouping_.SelectedIndex = 0;
         grouping_.SelectionChanged += (_, _) =>
         {
            review_.SetGrouping(grouping_.SelectedIndex switch
            {
               1 => QuarantineGrouping.HeloDomain,
               2 => QuarantineGrouping.None,
               _ => QuarantineGrouping.SenderDomain
            });
            Rebind_();
         };

         var groupLabel = new Label { Content = L("Gro_up by"), Target = grouping_, VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(0, 0, 6, 0) };
         AutomationProperties.SetName(grouping_, L("Group by"));

         var actions = new StackPanel { Orientation = Orientation.Horizontal };
         actions.Children.Add(groupLabel);
         actions.Children.Add(grouping_);
         removeExpired_ = MakeButton(L("Remove _expired"), ControlAppearance.Secondary, (_, _) => SweepExpired());
         ButtonStyles.DangerText(removeExpired_);
         actions.Children.Add(removeExpired_);
         actions.Children.Add(MakeButton(L("_Refresh"), ControlAppearance.Secondary, (_, _) => Reload()));
         actions.Children.Add(MakeButton(L("Di_gest settings"), ControlAppearance.Secondary, (_, _) => EditDigestSettings()));

         root.Children.Add(new PageHeader
         {
            Title = L("Quarantine"),
            Subtitle = L("Messages the server would otherwise have refused, held so somebody can look. The sender was told each of these was accepted, so nothing will arrive again on its own: releasing one delivers it, and deleting one is final. The icons act on one message, on a group, or - in the header - on every checked message."),
            Actions = actions
         });

         // The IP ranges shape: the list takes the height the notice above it
         // leaves in the window, down to a floor of three rows, and below that
         // the page scrolls. The list was a grid's star row under rows sized to
         // their content, which in the narrowest window (760 by 520) left it one
         // row.
         ScrollViewer scroll = FillPanel.ScrollingPage(out FillPanel parts);
         Grid.SetRow(scroll, 1);
         root.Children.Add(scroll);

         parts.Children.Add(status_);

         // The search, over what the page holds.
         search_.SearchPlaceholder = L("Search sender, recipient, subject or reason");
         search_.SearchTextChanged += (_, _) =>
         {
            review_.SetFilter(search_.SearchText);
            Rebind_();
            ShowSearchState_();
         };
         search_.SetResourceReference(MarginProperty, "AppCardGap");
         parts.Children.Add(search_);

         BuildColumns();
         BuildFlyout();
         AutomationProperties.SetName(list_, L("Quarantine"));

         // The grid's three rows on a FillPanel of its own and the card floored
         // at what it holds, so that the empty state laid over the grid - with
         // quarantining off, three lines and the way to Anti-spam settings -
         // raises the card's floor: at the list card's fixed 157 pixels a 760 by
         // 520 window cut it after two lines and hid the button (review finding
         // F207).
         var rows = new FillPanel();
         rows.Children.Add(FillPanel.Filling(list_, FillPanel.ListRowsFloor));
         var host = new Grid();
         host.Children.Add(rows);
         host.Children.Add(empty_);

         // Under the column headers, not over them: the header row, a check
         // box and four icon buttons, is taller than the empty state's padding,
         // and in a 760 by 520 window the glyph was drawn across its line.
         EmptyStateUnderHeaders.Keep(empty_, list_);

         var card = new Card { Padding = new Thickness(8), Content = host };
         parts.Children.Add(FillPanel.Filling(card, 0));

         // Why the selected message was held, which was the row's tool tip alone
         // (Views/RowDetailStrip).
         parts.Children.Add(RowDetailStrip.For(list_, _ => L("Why it was held"), row => row is QuarantineRow { IsMessage: true } held ? held.Message.Reason : null));

         // Why the selected message was held, under the list.
         why_.SetResourceReference(StyleProperty, "TextBody");
         AutomationProperties.SetAutomationId(whyCard_, "quarantine-why");
         whyCard_.Content = why_;
         whyCard_.SetResourceReference(MarginProperty, "AppCardGap");
         parts.Children.Add(whyCard_);
         list_.SelectionChanged += (_, _) => ShowWhy_();

         // The text columns as wide as the grid itself is - not the window, which at
         // 1280 pixels with the sidebar open gives the grid some 920 - and again as
         // its scroll bar comes and goes.
         list_.SizeChanged += (_, _) => LayOutTextColumns_();
         list_.AddHandler(ScrollViewer.ScrollChangedEvent, new ScrollChangedEventHandler((_, e) =>
         {
            if (e.ViewportWidthChange != 0)
               LayOutTextColumns_();
         }));

         Content = root;
      }

      /// <summary>
      /// Held and Score as wide as their headers - with room for the sort arrow on the
      /// column sorted by - and their widest words, so that the text columns share what
      /// is left of the width.
      /// </summary>
      private void SizeFixedColumns_()
      {
         if (heldColumn_ != null)
         {
            double held = GridStyles.HeaderWidth(review_.HeldCaption) + SortArrow_(QuarantineColumn.Held);
            foreach (QuarantineRow row in review_.Rows)
            {
               if (row.IsMessage)
                  held = Math.Max(held, GridStyles.CellWidth(row.Held));
            }
            heldColumn_.Width = new DataGridLength(Math.Ceiling(held));
         }

         if (scoreColumn_ != null)
            scoreColumn_.Width = new DataGridLength(Math.Ceiling(Math.Max(GridStyles.HeaderWidth(L("Score")) + SortArrow_(QuarantineColumn.Score), GridStyles.CellWidth("1000")))); // no-loc: the widest score
      }

      /// <summary>The room a header keeps for the sort arrow: on the column sorted by, which alone shows one.</summary>
      private double SortArrow_(QuarantineColumn column) => review_.SortColumn == column ? 16 : 0;

      /// <summary>What the widest sender, recipients and subject of the rows bound need, the subject's two icons with it.</summary>
      private void MeasureTextColumns_()
      {
         senderNeed_ = recipientsNeed_ = subjectNeed_ = 0;
         foreach (QuarantineRow row in review_.Rows)
         {
            if (!row.IsMessage)
               continue;

            senderNeed_ = Math.Max(senderNeed_, GridStyles.CellWidth(row.Sender));
            recipientsNeed_ = Math.Max(recipientsNeed_, GridStyles.CellWidth(row.Recipients));
            subjectNeed_ = Math.Max(subjectNeed_, GridStyles.CellWidth(row.Subject, 2 * CellIconWidth));
         }

         LayOutTextColumns_();
      }

      /// <summary>
      /// The text columns for the grid's width (QuarantineReview.Columns): the HELO name
      /// only where the grid is 1000 pixels wide or more - its own width, where the
      /// window's was asked, which at 1280 with the sidebar open kept the HELO column
      /// beside cut addresses - the recipients where the floors fit, and the sender and
      /// the subject floored at what they need, so that both are whole wherever there is
      /// room for both. The room is the grid's viewport, its vertical scroll bar left
      /// out, less the fixed columns and two pixels: where the floors fill the room, as
      /// they do where the sender and the subject share it, layout rounding at 125, 150
      /// or 175 per cent could make the columns a fraction of a pixel wider than the
      /// grid, and a grid whose scroll bars depend on each other never settles
      /// (ControlPanelDesign.md, the rules page's lists).
      /// </summary>
      private void LayOutTextColumns_()
      {
         if (heloColumn_ == null || list_.ActualWidth <= 0)
            return;

         ScrollViewer scroller = GridScroller_(list_);
         double viewport = scroller != null && scroller.ViewportWidth > 0 ? scroller.ViewportWidth : list_.ActualWidth;
         double room = viewport - CheckWidth - ActionsWidth - heldColumn_.Width.Value - scoreColumn_.Width.Value - RoundingSlack;

         QuarantineColumnLayout layout = QuarantineReview.Columns(list_.ActualWidth, room, senderNeed_, recipientsNeed_, subjectNeed_);
         heloColumn_.Visibility = layout.ShowsHelo ? Visibility.Visible : Visibility.Collapsed;
         recipientsColumn_.Visibility = layout.ShowsRecipients ? Visibility.Visible : Visibility.Collapsed;
         senderColumn_.MinWidth = Math.Max(1, layout.SenderMin);
         subjectColumn_.MinWidth = Math.Max(1, layout.SubjectMin);
         if (layout.ShowsRecipients)
            recipientsColumn_.MinWidth = Math.Max(1, layout.RecipientsMin);
      }

      private static ScrollViewer GridScroller_(DependencyObject node)
      {
         for (int i = 0; i < VisualTreeHelper.GetChildrenCount(node); i++)
         {
            DependencyObject child = VisualTreeHelper.GetChild(node, i);
            if (child is ScrollViewer scroller)
               return scroller;
            ScrollViewer inner = GridScroller_(child);
            if (inner != null)
               return inner;
         }

         return null;
      }

      /// <summary>Why the selected message was held, or nothing for a group or no selection.</summary>
      private void ShowWhy_()
      {
         if (list_.SelectedItem is QuarantineRow { IsMessage: true } row && row.Message.Reason.Length > 0)
         {
            why_.Text = row.HeldBecause;
            whyCard_.Visibility = Visibility.Visible;
         }
         else
         {
            whyCard_.Visibility = Visibility.Collapsed;
         }
      }

      /// <summary>
      /// The empty state for the search: a search that matches nothing says so - and,
      /// where the server listed only the most recent, that it looked at those - and
      /// a search cleared gives back what the page says with nothing held.
      /// </summary>
      private void ShowSearchState_()
      {
         // A read that failed has neither: its notice says what happened, and the
         // empty state stays hidden (Services/ReadFailure).
         if (ReadFailure.IsShown(status_))
            return;

         if (string.IsNullOrWhiteSpace(review_.Filter) || review_.Messages.Count == 0)
         {
            empty_.Action = emptyAction_;
            StatusText.Show(empty_, null, total_, null, emptyText_);
            return;
         }

         empty_.Action = null;
         if (review_.Shown.Count > 0)
         {
            empty_.Visibility = Visibility.Collapsed;
            return;
         }

         empty_.Text = review_.Messages.Count < total_
            ? F("None of the {0} most recent held messages has \"{1}\" in its sender, recipients, subject or reason. The server lists no more than those.", review_.Messages.Count, review_.Filter)
            : F("No held message has \"{0}\" in its sender, recipients, subject or reason.", review_.Filter);
         empty_.Visibility = Visibility.Visible;
      }

      private static Button MakeButton(string text, ControlAppearance appearance, RoutedEventHandler onClick)
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
