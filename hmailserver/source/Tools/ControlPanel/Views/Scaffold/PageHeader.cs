// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Windows;
using System.Windows.Automation;
using System.Windows.Automation.Peers;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Views.Scaffold
{
   /// <summary>
   /// The top of every page: the title, an optional subtitle under it, an
   /// optional status pill beside the title, and a slot on the right for the
   /// page's actions (refresh, add, save). It is the page's level-1 heading -
   /// screen readers navigate by headings, and a page without one has no
   /// landmark to jump to - so the heading level, the name and the help text
   /// are on the component itself, where its automation peer reports them.
   ///
   /// <code>
   /// &lt;scaffold:PageHeader Title="{loc:L 'Dashboard'}" Subtitle="{loc:L '...'}"&gt;
   ///    &lt;scaffold:PageHeader.Actions&gt;&lt;ui:Button .../&gt;&lt;/scaffold:PageHeader.Actions&gt;
   /// &lt;/scaffold:PageHeader&gt;
   /// </code>
   /// </summary>
   public class PageHeader : ScaffoldControl
   {
      static PageHeader()
      {
         DefaultStyleKeyProperty.OverrideMetadata(typeof(PageHeader), new FrameworkPropertyMetadata(typeof(PageHeader)));
      }

      public static readonly DependencyProperty TitleProperty = DependencyProperty.Register(
         nameof(Title), typeof(string), typeof(PageHeader), new PropertyMetadata(null, OnTitleChanged, NullWhenEmpty));

      public static readonly DependencyProperty SubtitleProperty = DependencyProperty.Register(
         nameof(Subtitle), typeof(string), typeof(PageHeader), new PropertyMetadata(null, OnSubtitleChanged, NullWhenEmpty));

      public static readonly DependencyProperty ActionsProperty = DependencyProperty.Register(
         nameof(Actions), typeof(object), typeof(PageHeader), new PropertyMetadata(null));

      public static readonly DependencyProperty StatusProperty = DependencyProperty.Register(
         nameof(Status), typeof(object), typeof(PageHeader), new PropertyMetadata(null));

      public static readonly DependencyProperty TourIdProperty = DependencyProperty.Register(
         nameof(TourId), typeof(string), typeof(PageHeader), new PropertyMetadata(null, OnTourIdChanged, NullWhenEmpty));

      /// <summary>
      /// Raised by the help button. It bubbles to the shell, which is the only
      /// thing that knows how to run a tour; a scaffold component that reached
      /// into the main window for it would be the design system depending on
      /// the application instead of the other way round.
      /// </summary>
      public static readonly RoutedEvent ShowTourEvent = EventManager.RegisterRoutedEvent(
         "ShowTour", RoutingStrategy.Bubble, typeof(RoutedEventHandler), typeof(PageHeader));

      public PageHeader()
      {
         AutomationProperties.SetHeadingLevel(this, AutomationHeadingLevel.Level1);
      }

      /// <summary>The page title - the same words as its navigation entry.</summary>
      public string Title
      {
         get => (string)GetValue(TitleProperty);
         set => SetValue(TitleProperty, value);
      }

      /// <summary>One line under the title saying what the page shows or when it was last refreshed. Null hides the line.</summary>
      public string Subtitle
      {
         get => (string)GetValue(SubtitleProperty);
         set => SetValue(SubtitleProperty, value);
      }

      /// <summary>The page's actions, right-aligned - usually a horizontal StackPanel of buttons.</summary>
      public object Actions
      {
         get => GetValue(ActionsProperty);
         set => SetValue(ActionsProperty, value);
      }

      /// <summary>An optional status beside the title - a <see cref="StatusPill"/>, as a rule.</summary>
      public object Status
      {
         get => GetValue(StatusProperty);
         set => SetValue(StatusProperty, value);
      }

      /// <summary>
      /// The id of the tour this page's help button starts, or null for no
      /// button. The shell sets it as it opens a page, from the tour catalogue,
      /// so a page gains a help button by being on a tour and not by being
      /// edited.
      /// </summary>
      public string TourId
      {
         get => (string)GetValue(TourIdProperty);
         set => SetValue(TourIdProperty, value);
      }

      /// <summary>Raised when the reader presses the help button.</summary>
      public event RoutedEventHandler ShowTour
      {
         add => AddHandler(ShowTourEvent, value);
         remove => RemoveHandler(ShowTourEvent, value);
      }

      protected override AutomationControlType ControlType => AutomationControlType.Text;

      public override void OnApplyTemplate()
      {
         base.OnApplyTemplate();

         var help = Part<Wpf.Ui.Controls.Button>("PART_Help");
         if (help == null)
            return;

         // The words, not just a glyph: a question mark alone reaches a screen
         // reader as nothing, and a page with two icon buttons in its header
         // has to be able to say which is which.
         string caption = L("Show me around this page");
         help.ToolTip = caption;
         AutomationProperties.SetName(help, caption);
         AutomationProperties.SetAutomationId(help, "page-help");
         help.Click += (s, e) => RaiseEvent(new RoutedEventArgs(ShowTourEvent, this));
         ApplyTourVisibility_();
      }

      /// <summary>
      /// The actions sit beside the title while that leaves the title and subtitle
      /// room to be read, and under the subtitle, from the left, when it does not.
      /// Beside it at any width, a page with a wide set of actions - the
      /// Quarantine page's grouping list and three buttons - squeezed its title to
      /// "Quaran..." and its subtitle to a column of three words a line in a
      /// 1000-pixel window, and pushed the list it describes below the fold.
      /// </summary>
      protected override Size MeasureOverride(Size constraint)
      {
         if (Part<FrameworkElement>("PART_ActionBar") is { } bar && !double.IsInfinity(constraint.Width))
         {
            bar.Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));
            double actions = bar.DesiredSize.Width;
            bool under = actions > 0 && constraint.Width - actions < System.Math.Max(MinimumTitleWidth, constraint.Width * 0.45);

            int row = under ? 1 : 0;
            if (System.Windows.Controls.Grid.GetRow(bar) != row)
            {
               System.Windows.Controls.Grid.SetRow(bar, row);
               System.Windows.Controls.Grid.SetColumn(bar, under ? 0 : 1);
               System.Windows.Controls.Grid.SetColumnSpan(bar, under ? 2 : 1);
               bar.HorizontalAlignment = under ? HorizontalAlignment.Left : HorizontalAlignment.Right;
               bar.Margin = under ? new Thickness(0, 12, 0, 0) : new Thickness(16, 6, 0, 0);
            }
         }

         return base.MeasureOverride(constraint);
      }

      /// <summary>The narrowest the title and subtitle are left beside the actions.</summary>
      private const double MinimumTitleWidth = 360;

      private static void OnTourIdChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
         => ((PageHeader)d).ApplyTourVisibility_();

      private void ApplyTourVisibility_()
      {
         var help = Part<Wpf.Ui.Controls.Button>("PART_Help");
         if (help != null)
            help.Visibility = string.IsNullOrEmpty(TourId) ? Visibility.Collapsed : Visibility.Visible;
      }

      private static void OnTitleChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
         => AutomationProperties.SetName(d, (string)e.NewValue ?? "");

      private static void OnSubtitleChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
         => AutomationProperties.SetHelpText(d, (string)e.NewValue ?? "");
   }
}
