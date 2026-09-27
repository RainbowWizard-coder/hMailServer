// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Windows;
using System.Windows.Automation;
using System.Windows.Automation.Peers;
using System.Windows.Controls;
using System.Windows.Shapes;
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

      public override void OnApplyTemplate()
      {
         base.OnApplyTemplate();
         Apply_();
      }

      private static void OnPresentationChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
         => ((StatusPill)d).Apply_();

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
