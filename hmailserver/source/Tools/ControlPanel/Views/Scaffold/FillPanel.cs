// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Windows;
using System.Windows.Controls;
using hMailServer.ControlPanel.Services;

namespace hMailServer.ControlPanel.Views.Scaffold
{
   /// <summary>
   /// A page's parts one under another, the ones marked
   /// <c>FillPanel.Fill="True"</c> - the page's lists, or the cards that hold
   /// them - taking the height the others leave in the window, down to their
   /// floors. Below that the panel is taller than the window and the page's
   /// ScrollViewer scrolls it. Every other part is given the height it asks for,
   /// always, so nothing on the page is cut off where scrolling cannot reach it.
   ///
   /// Several filling parts share the room by <c>FillPanel.Weight</c> (1 unless
   /// set), as a grid's star rows share theirs. A filling part's floor is one of
   /// two things.
   /// <list type="bullet">
   /// <item>A part with a MinHeight - a list, or a card around one - is floored
   /// at it, its margins with it, and is measured at the height it is given, so
   /// that a list realises the rows it shows. The MinHeight has to hold all of
   /// the part: three rows, and the empty state that takes their place.</item>
   /// <item>A part without one is floored at the height it asks for when nothing
   /// limits it, and is stretched to its share. That is the shape for a card
   /// that holds a toolbar, a list and a form: the card's content is a FillPanel
   /// of its own whose filling part is the list, so the card asks for its
   /// toolbar, its form and its list's floor, and a form that wraps or an empty
   /// state taller than the rows raises the card's floor with it. The list's
   /// own floor is a MinHeight, on the list or on a FillPanel around it where
   /// an empty state is laid over it.</item>
   /// </list>
   ///
   /// The window's height comes from <see cref="ViewportHeight"/>, bound to the
   /// page ScrollViewer's ViewportHeight: a panel inside a ScrollViewer is measured
   /// with an infinite height and has no other way to learn it. A FillPanel inside
   /// a filling part needs no binding: it is given its height when it is arranged.
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
   /// heights and cuts off whatever passes it - the shape the domains and rules
   /// pages had too. The arithmetic is <see cref="PageFill"/>.
   /// </summary>
   public class FillPanel : Panel
   {
      public static readonly DependencyProperty FillProperty = DependencyProperty.RegisterAttached(
         "Fill", typeof(bool), typeof(FillPanel), // no-loc
         new FrameworkPropertyMetadata(false, FrameworkPropertyMetadataOptions.AffectsParentMeasure));

      public static readonly DependencyProperty WeightProperty = DependencyProperty.RegisterAttached(
         "Weight", typeof(double), typeof(FillPanel), // no-loc
         new FrameworkPropertyMetadata(1.0, FrameworkPropertyMetadataOptions.AffectsParentMeasure),
         value => value is double weight && weight >= 0 && !double.IsInfinity(weight));

      public static readonly DependencyProperty ViewportHeightProperty = DependencyProperty.Register(
         nameof(ViewportHeight), typeof(double), typeof(FillPanel),
         new FrameworkPropertyMetadata(double.NaN, FrameworkPropertyMetadataOptions.AffectsMeasure));

      private double[] floors_ = Array.Empty<double>();
      private double[] measuredAt_ = Array.Empty<double>();
      private bool arranging_;

      public static bool GetFill(UIElement element)
         => element != null && (bool)element.GetValue(FillProperty);

      public static void SetFill(UIElement element, bool value)
         => element?.SetValue(FillProperty, value);

      /// <summary>A filling part's share of the room beside the others', as a star row's.</summary>
      public static double GetWeight(UIElement element)
         => element == null ? 0 : (double)element.GetValue(WeightProperty);

      public static void SetWeight(UIElement element, double value)
         => element?.SetValue(WeightProperty, value);

      /// <summary>The height of the window the page is seen through: the page ScrollViewer's ViewportHeight.</summary>
      public double ViewportHeight
      {
         get => (double)GetValue(ViewportHeightProperty);
         set => SetValue(ViewportHeightProperty, value);
      }

      protected override Size MeasureOverride(Size availableSize)
      {
         var unlimited = new Size(availableSize.Width, double.PositiveInfinity);
         double others = 0;
         double widest = 0;
         List<UIElement> fills = FillChildren();

         foreach (UIElement child in InternalChildren)
         {
            if (child == null || fills.Contains(child))
               continue;

            child.Measure(unlimited);
            others += child.DesiredSize.Height;
            widest = Math.Max(widest, child.DesiredSize.Width);
         }

         // A part without a MinHeight is measured as tall as it asks to be, and
         // that is its floor; it stays measured so, so that whatever changes
         // inside it - a form that wraps, an empty state that appears - reaches
         // this panel as a change of its size.
         double[] floors = new double[fills.Count];
         double[] weights = new double[fills.Count];
         for (int i = 0; i < fills.Count; i++)
         {
            UIElement fill = fills[i];
            bool shown = fill.Visibility != Visibility.Collapsed;
            weights[i] = shown ? GetWeight(fill) : 0;
            if (shown && HasFloor(fill))
               floors[i] = Floor(fill);
            else
            {
               fill.Measure(unlimited);
               floors[i] = shown ? fill.DesiredSize.Height : 0;
            }
         }

         // The page is the viewport, exactly, when everything fits, so that the
         // scroll viewer shows no bar; otherwise the parts and the floors. A
         // part with a floor of its own is measured at its share, which inside
         // another filling part, where the height is not known until the panel
         // is arranged, is its floor.
         double viewport = double.IsInfinity(availableSize.Height) ? ViewportHeight : availableSize.Height;
         double[] heights = PageFill.Share(viewport, others, floors, weights);
         double[] measuredAt = new double[fills.Count];
         double height = others;
         for (int i = 0; i < fills.Count; i++)
         {
            UIElement fill = fills[i];
            measuredAt[i] = double.PositiveInfinity;
            if (HasFloor(fill) && fill.Visibility != Visibility.Collapsed)
            {
               fill.Measure(new Size(availableSize.Width, heights[i]));
               measuredAt[i] = heights[i];
            }

            height += heights[i];
            widest = Math.Max(widest, fill.DesiredSize.Width);
         }

         floors_ = floors;
         measuredAt_ = measuredAt;
         return new Size(widest, height);
      }

      protected override Size ArrangeOverride(Size finalSize)
      {
         List<UIElement> fills = FillChildren();
         double others = 0;
         foreach (UIElement child in InternalChildren)
         {
            if (child != null && !fills.Contains(child))
               others += child.DesiredSize.Height;
         }

         // The filling parts share what the panel was given beyond the others -
         // the viewport, or more where a parent stretched the panel - each never
         // less than its floor.
         bool measured = floors_.Length == fills.Count;
         double[] floors = new double[fills.Count];
         double[] weights = new double[fills.Count];
         for (int i = 0; i < fills.Count; i++)
         {
            bool shown = fills[i].Visibility != Visibility.Collapsed;
            floors[i] = !shown ? 0 : measured ? floors_[i] : fills[i].DesiredSize.Height;
            weights[i] = shown ? GetWeight(fills[i]) : 0;
         }

         double[] heights = PageFill.Share(finalSize.Height, others, floors, weights);

         arranging_ = true;
         try
         {
            double y = 0;
            foreach (UIElement child in InternalChildren)
            {
               if (child == null)
                  continue;

               int fill = fills.IndexOf(child);
               double height = fill >= 0 ? heights[fill] : child.DesiredSize.Height;

               // A list measured at its floor and given more is measured again at
               // what it is given, so that it realises the rows that fill it.
               if (fill >= 0 && measured && HasFloor(child) && child.Visibility != Visibility.Collapsed && measuredAt_[fill] != height)
               {
                  child.Measure(new Size(finalSize.Width, height));
                  measuredAt_[fill] = height;
               }

               child.Arrange(new Rect(0, y, finalSize.Width, height));
               y += height;
            }
         }
         finally
         {
            arranging_ = false;
         }

         return finalSize;
      }

      /// <summary>
      /// A list measured again while the panel is arranged has a new size that is
      /// the panel's own doing, not a change the panel has to measure itself again
      /// for: answered with a new measure, it would measure the list at its floor
      /// and then again at its share, for ever.
      /// </summary>
      protected override void OnChildDesiredSizeChanged(UIElement child)
      {
         if (!arranging_)
            base.OnChildDesiredSizeChanged(child);
      }

      private List<UIElement> FillChildren()
      {
         var fills = new List<UIElement>();
         foreach (UIElement child in InternalChildren)
         {
            if (GetFill(child))
               fills.Add(child);
         }

         return fills;
      }

      private static bool HasFloor(UIElement element)
         => element is FrameworkElement fe && fe.MinHeight > 0;

      // The floor is the part's own MinHeight, and its margins with it, since
      // the height the panel hands out is the part's slot, margins included.
      private static double Floor(UIElement element)
         => element is FrameworkElement fe ? fe.MinHeight + fe.Margin.Top + fe.Margin.Bottom : 0;
   }
}
