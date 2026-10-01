// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Data;
using System.Windows.Input;
using System.Windows.Media;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// The words WPF puts on the screen by itself, in the Control Panel's language: the
   /// names of the text commands on every text box's right-click menu (the theme builds
   /// that menu - Copy, Cut and Paste, and Paste alone on a password box - from the
   /// commands' own names), and a scroll bar's right-click menu (Scroll Here, Top,
   /// Bottom, Page Up and the rest).
   ///
   /// WPF has those words in thirteen languages of its own. Six of the Control Panel's
   /// seventeen - Danish, Finnish, Norwegian, Dutch, Swedish and Ukrainian - are not
   /// among them, so there both menus were English in a Control Panel that is otherwise
   /// in that language (measured in Swedish: "Copy, Cut, Paste" on a text box, "Scroll
   /// Here, Top, Bottom, ..." on a scroll bar); and WPF's German scroll bar menu calls
   /// both Top and Page Up "Nach oben". So wherever the Control Panel shows one of its
   /// own languages, these words come from its catalogue. In English, and in a language
   /// it has no catalogue for (a Korean Windows, say, where its own words are English),
   /// they stay WPF's.
   ///
   /// The text commands' names are set once, at start-up, before any menu has read
   /// them. A scroll bar's menu is WPF's own until it is first opened: WPF builds it as
   /// it opens, from those English words; the first right-click on a scroll bar now
   /// gives that scroll bar the same menu in the Control Panel's words - the same items
   /// in the same order, the same commands sent to the same scroll bar, the same
   /// automation ids - and every right-click after it opens that one. A scroll bar
   /// given a menu of its own (none is, today) keeps it.
   ///
   /// One pair of words is the theme's, not WPF's, and English in every language: the
   /// screen-reader names of the two buttons that page a date picker's calendar,
   /// "Previous" and "Next" in each of the calendar's views (measured in English, Swedish,
   /// German, Japanese and Korean). Those are named from the catalogue in every language,
   /// English too, by what they page as the calendar shows months, years or decades -
   /// "Previous month", "Next decade" - since one word for both buttons of all three
   /// views is one no translation can choose well.
   ///
   /// And the theme gives the window's own buttons no name at all: Minimise, Maximise
   /// (Restore down while the window is maximised) and Close in the title bar were each
   /// just "button" to a screen reader, in every language (measured through UI
   /// Automation's client). They are named from the catalogue in every language too,
   /// by the button's type, which the title bar switches between Maximise and Restore.
   /// </summary>
   public static class FrameworkWords
   {
      private static bool installed_;

      private static readonly CalendarButtonName PreviousName = new(next: false);
      private static readonly CalendarButtonName NextName = new(next: true);
      private static readonly WindowButtonName WindowButtonNames = new();

      /// <summary>
      /// Puts the catalogue's words in place of WPF's and the theme's. Called once, from
      /// App.OnStartup, after the language is chosen and before the first window; safe to
      /// call again.
      /// </summary>
      public static void Install()
      {
         if (installed_)
            return;
         installed_ = true;

         // The calendar's two buttons and the title bar's, in every language: the theme's
         // names are English whatever the language, or none, so there are no words of
         // WPF's here to keep. Each is named when it is first laid out (SizeChanged,
         // which every new element raises and class handlers hear); a class handler of
         // Loaded is not called unless something in the tree has one of its own, and on
         // the calendar nothing has (measured), and a date picker's CalendarOpened is not
         // a routed event.
         EventManager.RegisterClassHandler(typeof(ButtonBase), FrameworkElement.SizeChangedEvent,
            new SizeChangedEventHandler(OnButtonSized));

         if (!Translated)
            return;

         ApplicationCommands.Copy.Text = L("Copy");
         ApplicationCommands.Cut.Text = L("Cut");
         ApplicationCommands.Paste.Text = L("Paste");

         EventManager.RegisterClassHandler(typeof(ScrollBar), FrameworkElement.ContextMenuOpeningEvent,
            new ContextMenuEventHandler(OnScrollBarMenuOpening));
      }

      /// <summary>
      /// Runs before WPF's own handler on the scroll bar (a class handler of the scroll
      /// bar runs before the one FrameworkElement calls its OnContextMenuOpening from),
      /// which is where WPF makes its menu - only while the menu is not set otherwise.
      /// </summary>
      private static void OnScrollBarMenuOpening(object sender, ContextMenuEventArgs e)
      {
         if (e.Handled || sender is not ScrollBar bar)
            return;
         if (DependencyPropertyHelper.GetValueSource(bar, FrameworkElement.ContextMenuProperty).BaseValueSource != BaseValueSource.Default)
            return;   // a menu of its own, or ours from an earlier right-click

         // WPF swaps a horizontal bar's ends when it runs right to left; none of the
         // catalogue's languages does, so such a bar keeps WPF's menu.
         if (bar.Orientation == Orientation.Horizontal && bar.FlowDirection == FlowDirection.RightToLeft)
            return;

         bar.ContextMenu = ScrollBarMenu(bar.Orientation);
      }

      /// <summary>
      /// WPF's scroll bar menu, item for item, in the catalogue's words: Scroll Here,
      /// the two ends, a page each way and a line each way, with separators between.
      /// </summary>
      public static ContextMenu ScrollBarMenu(Orientation orientation)
      {
         var menu = new ContextMenu
         {
            FontFamily = new FontFamily(Typography.UiFontFamily),
            FontSize = Typography.Control
         };

         menu.Items.Add(Item_(L("Scroll Here"), "ScrollHere", ScrollBar.ScrollHereCommand));
         menu.Items.Add(new Separator());
         if (orientation == Orientation.Vertical)
         {
            menu.Items.Add(Item_(L("Top"), "Top", ScrollBar.ScrollToTopCommand));   // no-loc: the second is WPF's automation id
            menu.Items.Add(Item_(L("Bottom"), "Bottom", ScrollBar.ScrollToBottomCommand));   // no-loc: the second is WPF's automation id
            menu.Items.Add(new Separator());
            menu.Items.Add(Item_(L("Page Up"), "PageUp", ScrollBar.PageUpCommand));
            menu.Items.Add(Item_(L("Page Down"), "PageDown", ScrollBar.PageDownCommand));
            menu.Items.Add(new Separator());
            menu.Items.Add(Item_(L("Scroll Up"), "ScrollUp", ScrollBar.LineUpCommand));
            menu.Items.Add(Item_(L("Scroll Down"), "ScrollDown", ScrollBar.LineDownCommand));
         }
         else
         {
            menu.Items.Add(Item_(L("Left Edge"), "LeftEdge", ScrollBar.ScrollToLeftEndCommand));
            menu.Items.Add(Item_(L("Right Edge"), "RightEdge", ScrollBar.ScrollToRightEndCommand));
            menu.Items.Add(new Separator());
            menu.Items.Add(Item_(L("Page Left"), "PageLeft", ScrollBar.PageLeftCommand));
            menu.Items.Add(Item_(L("Page Right"), "PageRight", ScrollBar.PageRightCommand));
            menu.Items.Add(new Separator());
            menu.Items.Add(Item_(L("Scroll Left"), "ScrollLeft", ScrollBar.LineLeftCommand));
            menu.Items.Add(Item_(L("Scroll Right"), "ScrollRight", ScrollBar.LineRightCommand));
         }

         return menu;
      }

      /// <summary>
      /// A title bar button, or a calendar's previous or next button, as it is first laid
      /// out (a template applied again makes new ones, named in turn; a theme change keeps
      /// them): it is named by a binding to the button's type, or to the calendar's view,
      /// so a maximised window's Restore down, or a switch to years or decades, renames it
      /// at once. Any other button is left at once. A name the Control Panel set otherwise
      /// on such a button (none is, today) stays.
      /// </summary>
      private static void OnButtonSized(object sender, SizeChangedEventArgs e)
      {
         if (sender is Wpf.Ui.Controls.TitleBarButton windowButton)
         {
            if (Unnamed_(windowButton))
            {
               windowButton.SetBinding(AutomationProperties.NameProperty, new Binding(nameof(Wpf.Ui.Controls.TitleBarButton.ButtonType))
               {
                  Source = windowButton,
                  Mode = BindingMode.OneWay,
                  Converter = WindowButtonNames
               });
            }
            return;
         }

         if (sender is not ButtonBase button || button.TemplatedParent is not CalendarItem item || item.TemplatedParent is not Calendar calendar)
            return;

         if (button.Name == "PART_PreviousButton")
            NameCalendarButton_(button, calendar, PreviousName);
         else if (button.Name == "PART_NextButton")
            NameCalendarButton_(button, calendar, NextName);
      }

      /// <summary>
      /// True when the button has no name of its own yet: one set on it already - this
      /// class's binding, at a button laid out again, or a name the Control Panel gave
      /// it - stays.
      /// </summary>
      private static bool Unnamed_(DependencyObject button)
         => button.ReadLocalValue(AutomationProperties.NameProperty) == DependencyProperty.UnsetValue;

      private static void NameCalendarButton_(ButtonBase button, Calendar calendar, CalendarButtonName name)
      {
         if (button == null || !Unnamed_(button))
            return;

         button.SetBinding(AutomationProperties.NameProperty, new Binding(nameof(Calendar.DisplayMode))
         {
            Source = calendar,
            Mode = BindingMode.OneWay,
            Converter = name
         });
      }

      /// <summary>
      /// The name of a calendar's previous or next button in the view it shows, in the
      /// Control Panel's language.
      /// </summary>
      public static string CalendarButton(CalendarMode view, bool next)
      {
         switch (view)
         {
            case CalendarMode.Year:
               return next ? L("Next year") : L("Previous year");
            case CalendarMode.Decade:
               return next ? L("Next decade") : L("Previous decade");
            default:
               return next ? L("Next month") : L("Previous month");
         }
      }

      /// <summary>
      /// The name of one of the window's own buttons in the title bar, by its type, in the
      /// Control Panel's language; empty for a type the title bar does not show.
      /// </summary>
      public static string WindowButton(Wpf.Ui.Controls.TitleBarButtonType type)
      {
         switch (type)
         {
            case Wpf.Ui.Controls.TitleBarButtonType.Minimize:
               return L("Minimise");
            case Wpf.Ui.Controls.TitleBarButtonType.Maximize:
               return L("Maximise");
            case Wpf.Ui.Controls.TitleBarButtonType.Restore:
               return L("Restore down");
            case Wpf.Ui.Controls.TitleBarButtonType.Close:
               return L("Close");
            case Wpf.Ui.Controls.TitleBarButtonType.Help:
               return L("Help");
            default:
               return "";
         }
      }

      /// <summary>A title bar button's type to its name.</summary>
      private sealed class WindowButtonName : IValueConverter
      {
         public object Convert(object value, System.Type targetType, object parameter, System.Globalization.CultureInfo culture)
            => value is Wpf.Ui.Controls.TitleBarButtonType type ? WindowButton(type) : "";

         public object ConvertBack(object value, System.Type targetType, object parameter, System.Globalization.CultureInfo culture)
            => throw new System.NotSupportedException();
      }

      /// <summary>A calendar's view to the name of one of its two paging buttons.</summary>
      private sealed class CalendarButtonName : IValueConverter
      {
         private readonly bool next_;

         public CalendarButtonName(bool next) => next_ = next;

         public object Convert(object value, System.Type targetType, object parameter, System.Globalization.CultureInfo culture)
            => CalendarButton(value is CalendarMode view ? view : CalendarMode.Month, next_);

         public object ConvertBack(object value, System.Type targetType, object parameter, System.Globalization.CultureInfo culture)
            => throw new System.NotSupportedException();
      }

      /// <summary>
      /// One item, made as WPF makes its own: the command, sent to the scroll bar the
      /// menu was opened on (its placement target), and the automation id.
      /// </summary>
      private static MenuItem Item_(string header, string automationId, RoutedCommand command)
      {
         var item = new MenuItem { Header = header, Command = command };
         AutomationProperties.SetAutomationId(item, automationId);
         item.SetBinding(MenuItem.CommandTargetProperty, new Binding
         {
            Path = new PropertyPath(ContextMenu.PlacementTargetProperty),
            Mode = BindingMode.OneWay,
            RelativeSource = new RelativeSource(RelativeSourceMode.FindAncestor, typeof(ContextMenu), 1)
         });
         return item;
      }
   }
}
