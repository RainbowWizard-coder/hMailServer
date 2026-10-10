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
   /// &lt;scaffold:PageHeader PageKey="dashboard" Subtitle="{loc:L '...'}"&gt;
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
         nameof(Subtitle), typeof(string), typeof(PageHeader), new PropertyMetadata(null, OnSubtitleChanged, SubtitleOrDescription_));

      public static readonly DependencyProperty ActionsProperty = DependencyProperty.Register(
         nameof(Actions), typeof(object), typeof(PageHeader), new PropertyMetadata(null));

      public static readonly DependencyProperty StatusProperty = DependencyProperty.Register(
         nameof(Status), typeof(object), typeof(PageHeader), new PropertyMetadata(null));

      public static readonly DependencyProperty TourIdProperty = DependencyProperty.Register(
         nameof(TourId), typeof(string), typeof(PageHeader), new PropertyMetadata(null, OnTourIdChanged, NullWhenEmpty));

      public static readonly DependencyProperty PageKeyProperty = DependencyProperty.Register(
         nameof(PageKey), typeof(string), typeof(PageHeader), new PropertyMetadata(null, OnPageKeyChanged, NullWhenEmpty));

      public static readonly DependencyProperty SubtitleInToolTipProperty = DependencyProperty.Register(
         nameof(SubtitleInToolTip), typeof(bool), typeof(PageHeader), new PropertyMetadata(false));

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

      /// <summary>
      /// The page title - the same words as its navigation entry, which
      /// <see cref="PageKey"/> sets. A page names its key and not its title.
      /// </summary>
      public string Title
      {
         get => (string)GetValue(TitleProperty);
         set => SetValue(TitleProperty, value);
      }

      /// <summary>
      /// What the page shows, or when it was last refreshed, in a sentence or two on a
      /// line of its own under the title and the actions, up to
      /// <see cref="Services.DesignTokens.SubtitleWidth"/> wide; the rest of what a
      /// page has to say is in its help (review finding F279). Left unset on a page
      /// that names its <see cref="PageKey"/>, it is the page's description in the
      /// navigation map - its tool tip in the sidebar and its line in the palette -
      /// so that a page with nothing more to say than that says it in the same
      /// words. Null hides the line on a header with no key.
      /// </summary>
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

      /// <summary>
      /// The navigation key of the page this header heads ("antispam"). It gives
      /// the header its title: the page's title in the navigation map, in the
      /// reader's language, which is what the sidebar, the breadcrumb, the
      /// palette and the help pane call the page. A page set its own title, and
      /// eight said something else - "Anti-spam" under the breadcrumb's
      /// "Anti-spam settings", "Domains &amp; accounts" under "Domains" (review
      /// findings F215, F025 and F080). PageTitlesTests holds every page to
      /// naming its key here.
      /// </summary>
      public string PageKey
      {
         get => (string)GetValue(PageKeyProperty);
         set => SetValue(PageKeyProperty, value);
      }

      /// <summary>
      /// The subtitle as the title's tool tip rather than a line under it, for a page
      /// that pins its header above content that scrolls, in a window too short for
      /// both (Services/PageHeaderLayout.SubtitleInToolTip; review finding F290). It
      /// stays the header's help text either way, so a screen reader hears it as
      /// before.
      /// </summary>
      public bool SubtitleInToolTip
      {
         get => (bool)GetValue(SubtitleInToolTipProperty);
         set => SetValue(SubtitleInToolTipProperty, value);
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

         // The words, not just a glyph: a glyph alone reaches a screen reader as
         // nothing, and a page with two icon buttons in its header has to be able
         // to say which is which. The glyph is a light bulb and the button says
         // "Show me around" where the header has room (MeasureOverride): it was
         // the question mark the command bar's help button is, 250 pixels away,
         // which opens the help pane and not the tour (review findings F024 and
         // F081; the owner's decision of 9 October 2026). No Alt key, as the tour's
         // card has none: the button is on whichever page a tour visits, and a key
         // it claimed would sooner or later be one that page already owns. The name
         // starts with the words the button shows, in every language, so that saying
         // them reaches it (WCAG 2.5.3, Label in Name; TourGlyphTests holds the
         // catalogues to it).
         string caption = L("Show me around this page");
         help.ToolTip = caption;
         AutomationProperties.SetName(help, caption);
         AutomationProperties.SetAutomationId(help, "page-help");
         help.Click += (s, e) => RaiseEvent(new RoutedEventArgs(ShowTourEvent, this));
         ShowTourWords_(help, true);
         ApplyTourVisibility_();
      }

      /// <summary>The tour button with its words, "Show me around", or as its light bulb alone.</summary>
      private static void ShowTourWords_(Wpf.Ui.Controls.Button help, bool words)
      {
         object content = words ? L("Show me around") : null;
         if (!Equals(help.Content, content))
            help.Content = content;
      }

      // Two buttons off the tree, as the tour's is with its words and without, so
      // that the header can tell how wide each would be without changing the one
      // it shows while it measures.
      private Wpf.Ui.Controls.Button tourWithWords_;
      private Wpf.Ui.Controls.Button tourGlyphOnly_;

      /// <summary>How wide the tour button is with its words, or as its glyph alone, in the font it is drawn in.</summary>
      private double TourButtonWidth_(Wpf.Ui.Controls.Button help, bool words)
      {
         ref Wpf.Ui.Controls.Button probe = ref words ? ref tourWithWords_ : ref tourGlyphOnly_;
         probe ??= new Wpf.Ui.Controls.Button
         {
            Appearance = Wpf.Ui.Controls.ControlAppearance.Transparent,
            Icon = new Wpf.Ui.Controls.SymbolIcon(Wpf.Ui.Controls.SymbolRegular.Lightbulb24),
            Content = words ? L("Show me around") : null
         };
         probe.Padding = help.Padding;
         probe.FontFamily = help.FontFamily;
         probe.FontSize = help.FontSize;
         probe.Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));
         return probe.DesiredSize.Width;
      }

      /// <summary>
      /// The actions sit beside the title while the title fits beside them whole,
      /// and under the subtitle, from the left, when it does not. The subtitle has a
      /// line of its own under both, so only the title is measured against them:
      /// beside the actions at any width, a page with a wide set of them - the
      /// Quarantine page's grouping list and three buttons - squeezed its title to
      /// "Quaran..." in a 1000-pixel window, and while the subtitle shared the
      /// title's column it ran to a column of three words a line and pushed the
      /// list it describes below the fold (review finding F279).
      /// </summary>
      protected override Size MeasureOverride(Size constraint)
      {
         if (Part<FrameworkElement>("PART_ActionBar") is { } bar && !double.IsInfinity(constraint.Width))
         {
            var infinite = new Size(double.PositiveInfinity, double.PositiveInfinity);
            bar.Measure(infinite);
            double actions = BesideWidth_(bar);
            double title = TitleWidth_();

            // The tour's words where the header has room for them, its light bulb
            // alone where only that keeps the actions beside the title (the owner's
            // decision of 9 October 2026), decided on the bar's width beside the
            // title whichever place it has now, so that the decision does not turn
            // with the place.
            if (Part<Wpf.Ui.Controls.Button>("PART_Help") is { Visibility: Visibility.Visible } help)
            {
               // The rest of the bar, and the button's margin, which a DesiredSize counts.
               double others = actions - help.DesiredSize.Width + help.Margin.Left + help.Margin.Right;
               bool words = Services.PageHeaderLayout.TourCaptionShows(constraint.Width,
                  others + TourButtonWidth_(help, true), others + TourButtonWidth_(help, false), title);
               if (words != (help.Content != null))
               {
                  // The bar's width with the button as it now is, from the button
                  // measured off the tree: measured again here, the bar answers with
                  // the width it had until the layout reaches the button's words. The
                  // next pass measures the button as it is, and decides the same,
                  // since nothing it decides on has changed.
                  ShowTourWords_(help, words);
                  actions = others + TourButtonWidth_(help, words);
               }
            }

            bool under = Services.PageHeaderLayout.ActionsGoUnder(constraint.Width, actions, TitleWidth_());

            int row = under ? 2 : 0;
            if (System.Windows.Controls.Grid.GetRow(bar) != row)
            {
               System.Windows.Controls.Grid.SetRow(bar, row);
               System.Windows.Controls.Grid.SetColumn(bar, under ? 0 : 1);
               System.Windows.Controls.Grid.SetColumnSpan(bar, under ? 2 : 1);
               bar.HorizontalAlignment = under ? HorizontalAlignment.Left : HorizontalAlignment.Right;
               bar.Margin = under ? new Thickness(0, 12, 0, 0) : new Thickness(BesideGap, 6, 0, 0);
            }
         }

         return base.MeasureOverride(constraint);
      }

      /// <summary>The gap the actions keep from the title beside it.</summary>
      private const double BesideGap = 16;

      /// <summary>
      /// The actions' width as they would stand beside the title, with the gap they
      /// keep there, wherever they are now - none when there are none. Measured
      /// under the subtitle, without that gap, they fitted beside the title by up
      /// to sixteen pixels they then did not have, and went under again.
      /// </summary>
      private static double BesideWidth_(FrameworkElement bar)
      {
         double bare = bar.DesiredSize.Width - bar.Margin.Left - bar.Margin.Right;
         return bare > 0 ? bare + BesideGap : 0;
      }

      /// <summary>The title's own width, whole, with its gap before the pill.</summary>
      private double TitleWidth_()
      {
         if (Part<FrameworkElement>("PART_Title") is not { } title)
            return 0;
         title.Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));
         return title.DesiredSize.Width;
      }

      private static void OnTourIdChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
         => ((PageHeader)d).ApplyTourVisibility_();

      private void ApplyTourVisibility_()
      {
         var help = Part<Wpf.Ui.Controls.Button>("PART_Help");
         if (help != null)
            help.Visibility = string.IsNullOrEmpty(TourId) ? Visibility.Collapsed : Visibility.Visible;
      }

      private static void OnPageKeyChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
      {
         var header = (PageHeader)d;
         if (e.NewValue is string key)
            header.Title = L(Services.NavigationMap.TitleOf(key));
         header.CoerceValue(SubtitleProperty);
      }

      /// <summary>The subtitle a page gave, or its navigation description where it gave none.</summary>
      private static object SubtitleOrDescription_(DependencyObject d, object value)
      {
         if (NullWhenEmpty(d, value) is string subtitle)
            return subtitle;

         string purpose = Services.NavigationMap.Find(((PageHeader)d).PageKey)?.Purpose;
         return string.IsNullOrEmpty(purpose) ? null : L(purpose);
      }

      private static void OnTitleChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
         => AutomationProperties.SetName(d, (string)e.NewValue ?? "");

      private static void OnSubtitleChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
         => AutomationProperties.SetHelpText(d, (string)e.NewValue ?? "");
   }
}
