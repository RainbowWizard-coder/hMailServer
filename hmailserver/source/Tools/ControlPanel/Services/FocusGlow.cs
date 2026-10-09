// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.ComponentModel;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Documents;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Animation;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// The focus look of a text field: while a text box, a password box, a
   /// number box, a combo box or a date picker has the keyboard - clicked into or tabbed into
   /// alike - a ring in the focus colour goes round it and a soft glow of the
   /// same colour outside the ring. The look the Control Deck and the webmail
   /// give a field (--focus, --focus-glow), and the colours are theirs
   /// (<see cref="DesignTokens.Focus"/>, which ThemeTokens publishes as
   /// AppFocusBrush and AppFocusGlowBrush).
   ///
   /// One class handler for the whole application, registered once at start-up,
   /// rather than a template per control: the fields come from Wpf.Ui's
   /// templates, which draw the focus as a two-pixel accent line along the
   /// bottom edge, and re-templating TextBox, Wpf.Ui's TextBox, PasswordBox and
   /// NumberBox and ComboBox to add a ring would be five copies of Wpf.Ui's
   /// templates to keep in step with it. The ring and the glow are an adorner
   /// instead: drawn on the adorner layer over the field, so it keeps the
   /// field's size and corner radius and moves nothing - the ring sits inside
   /// the field's edge, over its own border and the accent line, and the glow
   /// outside it. The layer is the one outside any scroll viewer the field is
   /// in, not the viewer's own, which would cut the glow off at the sides of a
   /// field as wide as the viewer - every dialog's fields; the adorner clips
   /// itself to what of the field is in view instead, letting the glow out
   /// only past an edge that is. A field whose window has no adorner layer
   /// still shows the focus colour: ThemeTokens makes the theme's accent line
   /// the same violet.
   ///
   /// Not drawn: under High Contrast, whose colours are the user's (the theme's
   /// own focus visuals stand, and ThemeTokens hands the accent line back); on
   /// a field that is disabled; on a field drawn as an underline or with no
   /// border at all (<see cref="DesignTokens.DrawsFocusRing"/> - a box whose
   /// bottom edge is the theme's accent line, as Wpf.Ui's date picker is, is a
   /// box). A read-only
   /// field takes the keyboard, so it is ringed. A field whose binding reports a
   /// validation error, or whose FieldRow carries a message
   /// (<see cref="Views.Scaffold.FieldRow.HasErrorProperty"/>), keeps the danger
   /// colour for its ring, with the violet glow outside it - a message set on a
   /// row sets no Validation.HasError, so the ring of a refused field stayed
   /// violet. While the ring is shown the field's own keyboard focus
   /// rectangle is held off, so the two are not drawn over each other. The ring
   /// fades in over 120 ms, unless Windows has animations turned off.
   /// </summary>
   public static class FocusGlow
   {
      private static bool registered_;
      private static Control field_;
      private static GlowAdorner adorner_;
      private static bool heldFocusVisual_;

      private static readonly DependencyPropertyDescriptor HasErrorDescriptor =
         DependencyPropertyDescriptor.FromProperty(Validation.HasErrorProperty, typeof(FrameworkElement));

      private static readonly DependencyPropertyDescriptor RefusedDescriptor =
         DependencyPropertyDescriptor.FromProperty(Views.Scaffold.FieldRow.HasErrorProperty, typeof(FrameworkElement));

      /// <summary>Safe to call more than once.</summary>
      public static void Register()
      {
         if (registered_)
            return;
         registered_ = true;

         // handledEventsToo: a control that marks the focus event handled must
         // not keep the ring from following the keyboard.
         EventManager.RegisterClassHandler(typeof(UIElement), Keyboard.GotKeyboardFocusEvent,
            new KeyboardFocusChangedEventHandler(OnGotKeyboardFocus_), true);
         EventManager.RegisterClassHandler(typeof(UIElement), Keyboard.LostKeyboardFocusEvent,
            new KeyboardFocusChangedEventHandler(OnLostKeyboardFocus_), true);

         // A theme change recolours the ring, or takes it away for High Contrast.
         ThemeTokens.Changed += (_, __) => Update_(Keyboard.FocusedElement);
      }

      // The focus events bubble, so every element on the way up sees them; act
      // once, at the element the focus arrived at or left.
      private static void OnGotKeyboardFocus_(object sender, KeyboardFocusChangedEventArgs e)
      {
         if (ReferenceEquals(sender, e.NewFocus))
            Update_(e.NewFocus);
      }

      private static void OnLostKeyboardFocus_(object sender, KeyboardFocusChangedEventArgs e)
      {
         if (ReferenceEquals(sender, e.OldFocus))
            Update_(e.NewFocus);
      }

      private static void Update_(IInputElement focused)
      {
         try
         {
            Control field = focused is DependencyObject node ? FieldOf_(node) : null;
            if (field != null && !Glows_(field))
               field = null;

            if (ReferenceEquals(field, field_))
            {
               adorner_?.InvalidateVisual();
               return;
            }

            Detach_();
            if (field != null)
               Attach_(field);
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            // A ring that failed to draw is not worth losing the keystroke over.
            App.LogException(ex);
         }
      }

      private static bool IsField_(DependencyObject node)
         => node is TextBoxBase || node is PasswordBox || node is ComboBox || node is DatePicker;

      /// <summary>
      /// The field the keyboard is in: the nearest text box, password box,
      /// combo box or date picker at or above the focused element - through a
      /// popup to the control that owns it, so that a combo box's open list
      /// keeps its box ringed - and then the outermost one whose template it is
      /// part of: the text box inside an editable combo box is the combo box's,
      /// and the one inside a date picker the date picker's.
      /// </summary>
      private static Control FieldOf_(DependencyObject node)
      {
         Control field = null;
         for (int depth = 0; node != null && depth < 128; depth++)
         {
            if (IsField_(node))
            {
               field = (Control)node;
               break;
            }

            if (node is Window)
               return null;

            // A popup's content leads back to the popup through its logical
            // parent, not its visual one (that ends at the popup's own window).
            DependencyObject parent = (node as FrameworkElement)?.Parent as Popup;
            parent ??= node is Visual || node is System.Windows.Media.Media3D.Visual3D ? VisualTreeHelper.GetParent(node) : null;
            node = parent ?? LogicalTreeHelper.GetParent(node);
         }

         while (field?.TemplatedParent is Control owner && IsField_(owner))
            field = owner;

         return field;
      }

      private static bool Glows_(Control field)
      {
         if (!DesignTokens.FocusGlowFor(ThemeTokens.CurrentChartTheme) || !field.IsEnabled || !field.IsVisible)
            return false;

         Thickness border = field.BorderThickness;
         return DesignTokens.DrawsFocusRing(border.Left, border.Top, border.Right, border.Bottom);
      }

      /// <summary>
      /// The adorner layer to draw on: the nearest one above the field that is
      /// not a scroll viewer's. A ScrollViewer's content presenter carries an
      /// adorner layer of its own, clipped to its viewport, and a field as wide
      /// as that viewport - every dialog's, whose body scrolls - lost its glow
      /// at both sides there. On the layer outside it, the adorner clips itself
      /// to what of the field is in view (<see cref="GlowAdorner"/>).
      /// </summary>
      private static AdornerLayer LayerFor_(Visual field)
      {
         AdornerLayer layer = AdornerLayer.GetAdornerLayer(field);
         while (layer != null && VisualTreeHelper.GetParent(layer) is ScrollContentPresenter viewport)
         {
            AdornerLayer outer = AdornerLayer.GetAdornerLayer(viewport);
            if (outer == null)
               break;
            layer = outer;
         }

         return layer;
      }

      private static void Attach_(Control field)
      {
         AdornerLayer layer = LayerFor_(field);
         if (layer == null)
            return;

         // The field's own keyboard focus rectangle would be drawn over the
         // ring; held off while the ring is shown, and only where nothing else
         // has set it.
         if (field.ReadLocalValue(FrameworkElement.FocusVisualStyleProperty) == DependencyProperty.UnsetValue)
         {
            field.FocusVisualStyle = null;
            heldFocusVisual_ = true;
         }

         field_ = field;
         adorner_ = new GlowAdorner(field, layer);
         layer.Add(adorner_);

         field.IsEnabledChanged += OnFieldChanged_;
         field.IsVisibleChanged += OnFieldChanged_;
         field.LayoutUpdated += OnFieldLayout_;
         HasErrorDescriptor.AddValueChanged(field, OnHasErrorChanged_);
         RefusedDescriptor.AddValueChanged(field, OnHasErrorChanged_);

         if (SystemParameters.ClientAreaAnimation)
            adorner_.BeginAnimation(UIElement.OpacityProperty, new DoubleAnimation(0, 1, TimeSpan.FromMilliseconds(120)));
      }

      private static void Detach_()
      {
         Control field = field_;
         GlowAdorner adorner = adorner_;
         field_ = null;
         adorner_ = null;
         if (field == null)
            return;

         field.IsEnabledChanged -= OnFieldChanged_;
         field.IsVisibleChanged -= OnFieldChanged_;
         field.LayoutUpdated -= OnFieldLayout_;
         HasErrorDescriptor.RemoveValueChanged(field, OnHasErrorChanged_);
         RefusedDescriptor.RemoveValueChanged(field, OnHasErrorChanged_);

         if (heldFocusVisual_ && field.ReadLocalValue(FrameworkElement.FocusVisualStyleProperty) == null)
            field.ClearValue(FrameworkElement.FocusVisualStyleProperty);
         heldFocusVisual_ = false;

         adorner?.Layer.Remove(adorner);
      }

      private static void OnFieldChanged_(object sender, DependencyPropertyChangedEventArgs e)
         => Update_(Keyboard.FocusedElement);

      private static void OnHasErrorChanged_(object sender, EventArgs e)
         => adorner_?.InvalidateVisual();

      // Any layout pass may have scrolled the field or resized what holds it;
      // the adorner draws again only when what of the field is in view changed.
      private static void OnFieldLayout_(object sender, EventArgs e)
         => adorner_?.Reclip();

      /// <summary>
      /// The ring inside the field's edge and the glow outside it, on a layer
      /// that may be outside the scroll viewers the field is in, so clipped by
      /// hand rather than by the layer (<see cref="Adorner.IsClipEnabled"/>,
      /// which would clip the glow to every viewport on the way up): the ring to
      /// what of the field is in view, and the glow to that and the glow's width
      /// beyond it on each side whose edge is in view
      /// (<see cref="DesignTokens.FocusGlowBounds"/>). A field scrolled half out
      /// of its page draws neither over the page's header.
      /// </summary>
      private sealed class GlowAdorner : Adorner
      {
         private Rect inView_ = Rect.Empty;

         public GlowAdorner(Control field, AdornerLayer layer) : base(field)
         {
            Layer = layer;
            IsHitTestVisible = false;
         }

         public AdornerLayer Layer { get; }

         public void Reclip()
         {
            if (InView_() != inView_)
               InvalidateVisual();
         }

         /// <summary>
         /// What of the field and its glow is in view, in the field's own
         /// coordinates: the field's box and the glow's width round it, and
         /// every clip between the field and the layer, intersected. Never
         /// larger than what is drawn: a clip far larger than its content made
         /// the composition draw the field's own text and border away.
         /// </summary>
         private Rect InView_()
         {
            var field = AdornedElement;
            double g = DesignTokens.FocusGlowWidth;
            Size size = field.RenderSize;
            Rect inView = new Rect(-g, -g, size.Width + 2 * g, size.Height + 2 * g);
            Visual top = VisualTreeHelper.GetParent(Layer) as Visual;
            for (Visual node = VisualTreeHelper.GetParent(field) as Visual; node != null && !ReferenceEquals(node, top); node = VisualTreeHelper.GetParent(node) as Visual)
            {
               Geometry clip = VisualTreeHelper.GetClip(node);
               if (clip == null)
                  continue;

               GeneralTransform toField = node.TransformToDescendant(field);
               if (toField == null)
                  continue;

               inView.Intersect(toField.TransformBounds(clip.Bounds));
               if (inView.IsEmpty)
                  break;
            }

            return inView;
         }

         protected override void OnRender(DrawingContext drawing)
         {
            var field = (Control)AdornedElement;
            inView_ = InView_();
            if (!field.IsEnabled || !DesignTokens.FocusGlowFor(ThemeTokens.CurrentChartTheme) || inView_.IsEmpty)
               return;

            // The tokens ThemeTokens publishes for the theme in force; the table's
            // own values only if a window cannot reach the application's resources.
            // The danger ring of a field whose value is refused by its key too, not
            // by the shared brush: a template that took that brush once froze it,
            // and a frozen brush keeps the colour of the theme it was frozen in.
            DesignTokens.FocusArgb focus = DesignTokens.Focus(ThemeTokens.CurrentChartTheme, ThemeTokens.CurrentSystemColors);
            Color ring = Refused_(field)
               ? Token_(field, "AppDangerBrush", DesignTokens.Status(ThemeTokens.CurrentChartTheme, ThemeTokens.CurrentSystemColors).Danger)
               : Token_(field, "AppFocusBrush", focus.Ring);
            Color glow = Token_(field, "AppFocusGlowBrush", focus.Glow);

            Size size = field.RenderSize;
            if (size.Width <= 0 || size.Height <= 0)
               return;

            double radius = Math.Max(0, ((CornerRadius)field.GetValue(Border.CornerRadiusProperty)).TopLeft);

            // The glow: one-pixel rings outside the edge, fading outward.
            (double left, double top, double right, double bottom) = DesignTokens.FocusGlowBounds(
               inView_.Left, inView_.Top, inView_.Right, inView_.Bottom, size.Width, size.Height);
            if (right > left && bottom > top)
            {
               drawing.PushClip(Frozen_(new RectangleGeometry(new Rect(new Point(left, top), new Point(right, bottom)))));
               for (int step = 0; step < DesignTokens.FocusGlowWidth; step++)
               {
                  double opacity = DesignTokens.FocusGlowOpacityAt(step);
                  if (opacity <= 0)
                     continue;

                  double at = step + 0.5;
                  Pen pen = Frozen_(new Pen(Frozen_(new SolidColorBrush(Color.FromArgb((byte)Math.Round(glow.A * opacity), glow.R, glow.G, glow.B))), 1));
                  drawing.DrawRoundedRectangle(null, pen, new Rect(-at, -at, size.Width + 2 * at, size.Height + 2 * at), radius + at, radius + at);
               }

               drawing.Pop();
            }

            // The ring: inside the edge, over the field's border and the theme's
            // accent line, with the field's own corners - only where the field is.
            double half = DesignTokens.FocusRingThickness / 2;
            Pen ringPen = Frozen_(new Pen(Frozen_(new SolidColorBrush(ring)), DesignTokens.FocusRingThickness));
            drawing.PushClip(Frozen_(new RectangleGeometry(inView_)));
            drawing.DrawRoundedRectangle(null, ringPen,
               new Rect(half, half, Math.Max(0, size.Width - 2 * half), Math.Max(0, size.Height - 2 * half)),
               Math.Max(0, radius - half), Math.Max(0, radius - half));
            drawing.Pop();
         }

         /// <summary>
         /// Whether the field's value is refused: by its binding's validation, or by
         /// the message its FieldRow carries.
         /// </summary>
         private static bool Refused_(Control field)
            => Validation.GetHasError(field) || Views.Scaffold.FieldRow.GetHasError(field);

         private static Color Token_(FrameworkElement field, string key, uint fallback)
            => field.TryFindResource(key) is SolidColorBrush brush ? brush.Color : FromArgb(fallback);

         private static T Frozen_<T>(T freezable) where T : Freezable
         {
            freezable.Freeze();
            return freezable;
         }

         private static Color FromArgb(uint argb)
            => Color.FromArgb((byte)(argb >> 24), (byte)(argb >> 16), (byte)(argb >> 8), (byte)argb);
      }
   }
}
