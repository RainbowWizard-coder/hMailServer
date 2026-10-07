// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Windows;
using System.Windows.Controls;
using hMailServer.ControlPanel.Services;

namespace hMailServer.ControlPanel.Views.Scaffold
{
   /// <summary>
   /// A page's parts one under another, one of them - the one marked
   /// <c>FillPanel.Fill="True"</c>, usually the card with the page's list - taking
   /// the height the others leave in the window, down to its own MinHeight. Below
   /// that the panel is taller than the window and the page's ScrollViewer scrolls
   /// it. Every other part is given the height it asks for, always, so nothing on
   /// the page is cut off where scrolling cannot reach it.
   ///
   /// The window's height comes from <see cref="ViewportHeight"/>, bound to the
   /// page ScrollViewer's ViewportHeight: a panel inside a ScrollViewer is measured
   /// with an infinite height and has no other way to learn it. Outside a
   /// ScrollViewer the height the panel is given is the window's.
   /// <code>
   /// &lt;ScrollViewer x:Name="PageScroll" VerticalScrollBarVisibility="Auto" HorizontalScrollBarVisibility="Disabled"&gt;
   ///    &lt;scaffold:FillPanel ViewportHeight="{Binding ViewportHeight, ElementName=PageScroll}"&gt;
   ///       &lt;scaffold:Toolbar ... /&gt;
   ///       &lt;scaffold:Card scaffold:FillPanel.Fill="True" MinHeight="200"&gt; ...the list... &lt;/scaffold:Card&gt;
   ///       &lt;scaffold:Card Title="..."&gt; ...a form... &lt;/scaffold:Card&gt;
   ///    &lt;/scaffold:FillPanel&gt;
   /// &lt;/ScrollViewer&gt;
   /// </code>
   /// It replaces the two shapes that lost the IP ranges page its list and then
   /// its add form (work item 79): the list in a Grid's star row between rows
   /// sized to their content, which a short window gives nothing, and a Grid held
   /// to the viewport over a fixed MinHeight, which knows nothing of the parts'
   /// heights and cuts off whatever passes it. The arithmetic is
   /// <see cref="PageFill"/>.
   /// </summary>
   public class FillPanel : Panel
   {
      public static readonly DependencyProperty FillProperty = DependencyProperty.RegisterAttached(
         "Fill", typeof(bool), typeof(FillPanel), // no-loc
         new FrameworkPropertyMetadata(false, FrameworkPropertyMetadataOptions.AffectsParentMeasure));

      public static readonly DependencyProperty ViewportHeightProperty = DependencyProperty.Register(
         nameof(ViewportHeight), typeof(double), typeof(FillPanel),
         new FrameworkPropertyMetadata(double.NaN, FrameworkPropertyMetadataOptions.AffectsMeasure));

      private double fillHeight_;

      public static bool GetFill(UIElement element)
         => element != null && (bool)element.GetValue(FillProperty);

      public static void SetFill(UIElement element, bool value)
         => element?.SetValue(FillProperty, value);

      /// <summary>The height of the window the page is seen through: the page ScrollViewer's ViewportHeight.</summary>
      public double ViewportHeight
      {
         get => (double)GetValue(ViewportHeightProperty);
         set => SetValue(ViewportHeightProperty, value);
      }

      protected override Size MeasureOverride(Size availableSize)
      {
         double others = 0;
         double widest = 0;
         UIElement fill = FillChild();

         foreach (UIElement child in InternalChildren)
         {
            if (child == null || child == fill)
               continue;

            child.Measure(new Size(availableSize.Width, double.PositiveInfinity));
            others += child.DesiredSize.Height;
            widest = Math.Max(widest, child.DesiredSize.Width);
         }

         fillHeight_ = 0;
         if (fill == null)
            return new Size(widest, others);

         // The page is the viewport, exactly, when everything fits, so that the
         // scroll viewer shows no bar; otherwise the parts and the floor.
         double viewport = double.IsInfinity(availableSize.Height) ? ViewportHeight : availableSize.Height;
         bool shown = fill.Visibility != Visibility.Collapsed;
         double height = shown ? PageFill.PageHeight(viewport, others, Floor(fill)) : others;
         fillHeight_ = shown ? PageFill.FillHeight(viewport, others, Floor(fill)) : 0;
         fill.Measure(new Size(availableSize.Width, fillHeight_));
         if (fill.DesiredSize.Height > fillHeight_)
         {
            fillHeight_ = fill.DesiredSize.Height;
            height = others + fillHeight_;
         }

         widest = Math.Max(widest, fill.DesiredSize.Width);
         return new Size(widest, height);
      }

      protected override Size ArrangeOverride(Size finalSize)
      {
         UIElement fill = FillChild();
         double others = 0;
         foreach (UIElement child in InternalChildren)
         {
            if (child != null && child != fill)
               others += child.DesiredSize.Height;
         }

         // The filling part takes what the panel was given beyond the others,
         // which is the viewport, or more where a parent stretched the panel.
         double fillHeight = fill == null || fill.Visibility == Visibility.Collapsed
            ? 0
            : Math.Max(fillHeight_, finalSize.Height - others);

         double y = 0;
         foreach (UIElement child in InternalChildren)
         {
            if (child == null)
               continue;

            double height = child == fill ? fillHeight : child.DesiredSize.Height;
            child.Arrange(new Rect(0, y, finalSize.Width, height));
            y += height;
         }

         return finalSize;
      }

      private UIElement FillChild()
      {
         foreach (UIElement child in InternalChildren)
         {
            if (GetFill(child))
               return child;
         }

         return null;
      }

      // The floor is the part's own MinHeight, and its margins with it, since
      // the height the panel hands out is the part's slot, margins included.
      private static double Floor(UIElement element)
         => element is FrameworkElement fe ? fe.MinHeight + fe.Margin.Top + fe.Margin.Bottom : 0;
   }
}
