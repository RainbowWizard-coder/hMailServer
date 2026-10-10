// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Shapes;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// Installs the keyboard focus ring, one for every control, that is visible on
   /// every theme: in the light and dark themes a ring in the focus colour
   /// (AppFocusBrush), two pixels wide and two pixels outside the control - the
   /// colour and the width a text field's ring has (FocusGlow) and the navigation's
   /// and the lists' rows draw inside themselves, since a row's neighbours leave no
   /// room outside it - and under High Contrast the two-tone ring described below.
   /// The focus was drawn four ways (review finding F296): the violet ring on the
   /// fields, a violet border on the navigation's rows, this two-tone ring on the
   /// grids and the tabs, and WPF-UI's own one-pixel rectangle in the theme's text
   /// colour on every WPF-UI control - its buttons, check boxes and radio buttons -
   /// which ask for it by its key, DefaultControlFocusVisualStyle. The ring is put
   /// under both keys, so that every control that keeps a focus visual draws this one.
   ///
   /// WPF's stock focus visual is a one-pixel black dotted rectangle. On the dark
   /// theme that is black on near-black and effectively invisible; on a High
   /// Contrast Black theme it disappears completely. Keyboard users are the whole
   /// audience for a focus ring, so an invisible one is a functional bug, not a
   /// cosmetic one.
   ///
   /// The replacement is the standard two-tone ring: a solid dark rectangle with a
   /// dashed light rectangle drawn on top of it. The pair cannot vanish, because
   /// whichever of the two colours matches the background, the other one does not
   /// - on white the black solid line carries it, on black the white dashes do,
   /// and on a mid-tone surface both are visible. That is why it is done with two
   /// fixed colours rather than a theme brush: a ring computed from the current
   /// theme is only as good as our guess about what is behind it, and the chart
   /// cards, the log viewer and the nav sidebar all have different backgrounds.
   ///
   /// Under High Contrast the two-tone ring stands, whose two colours are no guess
   /// about the user's palette.
   ///
   /// Registered under <see cref="SystemParameters.FocusVisualStyleKey"/> in the
   /// application resources, which is the documented hook for replacing the focus
   /// visual for every control that has not opted out of it, and under WPF-UI's own
   /// key; <see cref="Apply"/> chooses between the two rings as the theme changes.
   ///
   /// It is registered from code rather than from App.xaml so that the whole
   /// feature is one file, and because App.xaml is shared with the navigation and
   /// palette work.
   /// </summary>
   public static class AccessibleFocus
   {
      /// <summary>The key WPF-UI's control styles ask for their focus visual by.</summary>
      internal const string WpfUiFocusVisualKey = "DefaultControlFocusVisualStyle";

      /// <summary>
      /// The same ring drawn inside the control's edge, for a control whose own scroller
      /// clips what is drawn outside it: a tab, whose strip scrolls and cut the ring
      /// outside a tab to its sides.
      /// </summary>
      internal const string InsetFocusVisualKey = "AppFocusVisualInset";

      private static bool registered_;
      private static Style ring_;
      private static Style twoTone_;
      private static Style insetRing_;
      private static Style insetTwoTone_;

      /// <summary>Safe to call more than once; does nothing without an Application.</summary>
      public static void Register()
      {
         if (registered_ || Application.Current == null)
            return;

         registered_ = true;
         ring_ = BuildRing_(DesignTokens.FocusRingThickness);
         twoTone_ = BuildTwoTone_(2);
         insetRing_ = BuildRing_(0);
         insetTwoTone_ = BuildTwoTone_(0);
         Apply(ThemeTokens.IsHighContrast);
      }

      /// <summary>
      /// Puts the ring for the theme in force under both keys: the focus colour's in
      /// the light and dark themes, the two-tone ring under High Contrast. The
      /// application's own dictionary shadows WPF-UI's, and a control looks its focus
      /// visual up each time it is shown, so a change of theme reaches the next one.
      /// </summary>
      public static void Apply(bool highContrast)
      {
         if (!registered_ || Application.Current == null)
            return;

         Style style = highContrast ? twoTone_ : ring_;
         Application.Current.Resources[SystemParameters.FocusVisualStyleKey] = style;
         Application.Current.Resources[WpfUiFocusVisualKey] = style;
         Application.Current.Resources[InsetFocusVisualKey] = highContrast ? insetTwoTone_ : insetRing_;
      }

      /// <summary>
      /// The ring: the focus colour by its resource key, so that it follows the theme,
      /// two pixels wide, <paramref name="outside"/> pixels outside the control - two,
      /// or none for the inset ring - its corners the control's radius that far out.
      /// </summary>
      private static Style BuildRing_(double outside)
      {
         var ring = new FrameworkElementFactory(typeof(Rectangle));
         ring.SetResourceReference(Shape.StrokeProperty, "AppFocusBrush");
         ring.SetValue(Shape.StrokeThicknessProperty, DesignTokens.FocusRingThickness);
         ring.SetValue(Rectangle.RadiusXProperty, DesignTokens.Radius.Control + outside);
         ring.SetValue(Rectangle.RadiusYProperty, DesignTokens.Radius.Control + outside);
         ring.SetValue(FrameworkElement.MarginProperty, new Thickness(-outside));
         ring.SetValue(UIElement.SnapsToDevicePixelsProperty, true);

         var template = new ControlTemplate(typeof(Control)) { VisualTree = ring };
         var style = new Style(typeof(Control));
         style.Setters.Add(new Setter(Control.TemplateProperty, template));
         style.Seal();
         return style;
      }

      private static Style BuildTwoTone_(double outside)
      {
         // Frozen: a FrameworkElementFactory hands the same value to every
         // instantiation of the template, and every focused control in the app
         // shares this one template.
         var dashes = new DoubleCollection(new[] { 2.0, 2.0 });
         dashes.Freeze();

         var tree = new FrameworkElementFactory(typeof(Grid));
         tree.AppendChild(Ring_(Brushes.Black, null, outside));
         tree.AppendChild(Ring_(Brushes.White, dashes, outside));

         var template = new ControlTemplate(typeof(Control)) { VisualTree = tree };

         // The focus adorner hosts a Control and applies this style to it, so the
         // template property has to be set as Control.Template.
         var style = new Style(typeof(Control));
         style.Setters.Add(new Setter(Control.TemplateProperty, template));
         style.Seal();
         return style;
      }

      private static FrameworkElementFactory Ring_(Brush stroke, DoubleCollection dashes, double outside)
      {
         var ring = new FrameworkElementFactory(typeof(Rectangle));
         ring.SetValue(Shape.StrokeProperty, stroke);
         ring.SetValue(Shape.StrokeThicknessProperty, 2.0);
         ring.SetValue(Rectangle.RadiusXProperty, 4.0);
         ring.SetValue(Rectangle.RadiusYProperty, 4.0);

         // Sit just outside the focused element. The adorner layer does not clip,
         // and a ring drawn inside the bounds gets lost against the control's own
         // border on the Fluent controls.
         ring.SetValue(FrameworkElement.MarginProperty, new Thickness(-outside));
         ring.SetValue(UIElement.SnapsToDevicePixelsProperty, true);

         if (dashes != null)
            ring.SetValue(Shape.StrokeDashArrayProperty, dashes);

         return ring;
      }
   }
}
