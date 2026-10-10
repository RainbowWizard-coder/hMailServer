// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Globalization;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Automation.Peers;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Shapes;
using System.Windows.Threading;
using hMailServer.ControlPanel.Services;

namespace hMailServer.ControlPanel.Views.Scaffold
{
   /// <summary>
   /// A status as a filled, rounded box: the shape and the colour of its level
   /// (<see cref="StatusSemantics"/>) and a short word - "Connected", "Backlog",
   /// "Expires in 12 days". The shell's server status is one; a page header's
   /// status is one; a grid cell that says the state of a row is one. Its
   /// accessible name leads with the severity word, so "Warning: Backlog" is
   /// heard where "Backlog" is seen beside a triangle.
   ///
   /// A pill that says a list's count - a list page's, beside its title - is a
   /// polite live region (<c>AutomationProperties.LiveSetting="Polite"</c>) and
   /// announces itself through <see cref="Announce"/> as its text or level
   /// changes and as it comes into view, as a notice does (InlineNotice): "Shown:
   /// 2 of 40" as a search is typed in, "101 to 200 of 1284" after a page step
   /// (WCAG 2.2 success criterion 4.1.3, Status Messages, whose own example is a
   /// search's result count). The counts were Information notices, which said
   /// themselves, until the list page pattern moved them here (review finding
   /// F288). A pill that is no live region - the setting's default, Off - asks for
   /// nothing, so the shell's pill and a grid's cells are as they were.
   ///
   /// A pill its row squeezes ends its word in an ellipsis and says the whole
   /// word in a tool tip, unless the page gave it a tool tip of its own. The
   /// accessible name always held the whole word; a reader who sees the pill
   /// had "On, but cann…" and nothing to point at (review finding F265).
   ///
   /// <code>
   /// &lt;scaffold:StatusPill Level="Good" Text="{loc:L 'Connected'}" /&gt;
   /// </code>
   /// </summary>
   public class StatusPill : ScaffoldControl
   {
      static StatusPill()
      {
         DefaultStyleKeyProperty.OverrideMetadata(typeof(StatusPill), new FrameworkPropertyMetadata(typeof(StatusPill)));
      }

      public StatusPill()
      {
         // A live region's pill filled in while it was collapsed is announced as
         // it is shown; one that changes while it is on screen, as it changes.
         IsVisibleChanged += (s, e) =>
         {
            if ((bool)e.NewValue && IsLive_())
               Announce.Request(this);
         };
      }

      public static readonly DependencyProperty LevelProperty = DependencyProperty.Register(
         nameof(Level), typeof(StatusLevel), typeof(StatusPill), new PropertyMetadata(StatusLevel.Normal, OnPresentationChanged));

      public static readonly DependencyProperty TextProperty = DependencyProperty.Register(
         nameof(Text), typeof(string), typeof(StatusPill), new PropertyMetadata(null, OnPresentationChanged, NullWhenEmpty));

      /// <summary>The level, which decides the colour and the shape.</summary>
      public StatusLevel Level
      {
         get => (StatusLevel)GetValue(LevelProperty);
         set => SetValue(LevelProperty, value);
      }

      /// <summary>The word or two on the pill.</summary>
      public string Text
      {
         get => (string)GetValue(TextProperty);
         set => SetValue(TextProperty, value);
      }

      protected override AutomationControlType ControlType => AutomationControlType.Text;

      // The name this component last gave itself. A page that sets a fuller
      // sentence of its own - the shell's "Connected to host as user" - keeps
      // it: the component only replaces a name it wrote.
      private string ownName_;

      // The template's word, watched for being cut (TipWhenTrimmed_).
      private TextBlock label_;

      // The tool tip this component gave itself, the whole word of a cut pill. A
      // tool tip the page set is not this one, and is left alone.
      private object ownTip_;

      public override void OnApplyTemplate()
      {
         if (label_ != null)
            label_.SizeChanged -= Label_SizeChanged;

         base.OnApplyTemplate();

         label_ = Part<TextBlock>("PART_Text");
         if (label_ != null)
            label_.SizeChanged += Label_SizeChanged;

         Apply_();
      }

      private void Label_SizeChanged(object sender, SizeChangedEventArgs e) => TipWhenTrimmed_();

      private static void OnPresentationChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
      {
         var pill = (StatusPill)d;
         pill.Apply_();
         if (pill.IsLive_())
            Announce.Request(pill);

         // A new word in a box held to the same width changes no size: whether it
         // is cut is asked once it is laid out.
         if (e.Property == TextProperty && pill.label_ != null)
            pill.Dispatcher.BeginInvoke(DispatcherPriority.Loaded, new Action(pill.TipWhenTrimmed_));
      }

      /// <summary>
      /// Gives the pill its whole word as a tool tip while the word is cut - its
      /// width in the word's own font wider than the room the row left it - and
      /// takes that tool tip away once the word fits again.
      /// </summary>
      private void TipWhenTrimmed_()
      {
         TextBlock label = label_;
         string text = Text;
         bool trimmed = label != null && !string.IsNullOrEmpty(text) && label.ActualWidth > 0 &&
            NaturalWidth_(label, text) > label.ActualWidth + 1;

         if (trimmed)
         {
            if (ToolTip == null || ReferenceEquals(ToolTip, ownTip_))
            {
               ownTip_ = text;
               ToolTip = text;
            }
         }
         else if (ownTip_ != null)
         {
            if (ReferenceEquals(ToolTip, ownTip_))
               ClearValue(ToolTipProperty);
            ownTip_ = null;
         }
      }

      /// <summary>The width <paramref name="text"/> takes in <paramref name="label"/>'s font, as the window draws it.</summary>
      private static double NaturalWidth_(TextBlock label, string text)
      {
         var words = new FormattedText(text, CultureInfo.CurrentUICulture, label.FlowDirection,
            new Typeface(label.FontFamily, label.FontStyle, label.FontWeight, label.FontStretch), label.FontSize, Brushes.Black, null,
            TextOptions.GetTextFormattingMode(label), VisualTreeHelper.GetDpi(label).PixelsPerDip);
         return words.WidthIncludingTrailingWhitespace;
      }

      // Whether the pill has something to say as a live region. Asked before a
      // request is queued, so that the pills that are none - a grid's, many at
      // once as its rows come into view - queue nothing.
      private bool IsLive_()
         => !string.IsNullOrEmpty(Text) && AutomationProperties.GetLiveSetting(this) != AutomationLiveSetting.Off;

      /// <summary>
      /// Paints the level into the template's parts: the level's fill behind,
      /// and the word and the mark in the shade of the level's colour chosen
      /// for that fill (<see cref="DesignTokens.StatusFill"/>). It used to be a
      /// capsule on the theme's subtle grey with a one-pixel outline in the
      /// status colour, which read as a stray line drawn round the word, and
      /// whose light-theme green measured 4.2:1 against that grey. The outline
      /// brush is the text colour, drawn only where the thickness token is
      /// non-zero - under High Contrast, whose fill is the window colour.
      /// Brushes go by resource key, never by instance, so a theme change
      /// reaches the pill (see InlineNotice).
      /// </summary>
      private void Apply_()
      {
         StatusPresentation status = StatusSemantics.For(Level);
         string text = Text ?? "";

         // Normal is the level that says nothing, and it takes the neutral
         // grey box (StatusSemantics gives Normal the neutral fill keys).
         string fillKey = status.FillBrushKey;
         string textKey = status.FillTextBrushKey;

         string current = AutomationProperties.GetName(this);
         if (string.IsNullOrEmpty(current) || string.Equals(current, ownName_, System.StringComparison.Ordinal))
         {
            ownName_ = Level == StatusLevel.Normal ? text : status.SeverityWord + ": " + text;
            AutomationProperties.SetName(this, ownName_);
         }

         if (Part<Path>("PART_Mark") is { } mark)
            ShapeMarkVisuals.ApplyMark(mark, status.Shape, textKey);

         if (Part<TextBlock>("PART_Text") is { } label)
            label.SetResourceReference(TextBlock.ForegroundProperty, textKey);

         if (Part<Border>("PART_Frame") is { } frame)
         {
            frame.SetResourceReference(Border.BackgroundProperty, fillKey);
            frame.SetResourceReference(Border.BorderBrushProperty, textKey);
         }
      }
   }
}
