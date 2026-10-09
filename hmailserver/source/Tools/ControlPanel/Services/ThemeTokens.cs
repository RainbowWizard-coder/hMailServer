// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Windows;
using System.Windows.Media;
using Microsoft.Win32;          // SystemEvents, for the High Contrast switch
using Wpf.Ui.Appearance;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// Central semantic colour tokens for the Control Panel.
   ///
   /// Each token is a single shared, mutable <see cref="SolidColorBrush"/> that is
   /// registered in the application resources and also exposed as a static field.
   /// When the theme changes we recompute the brush <em>colours in place</em>, so
   /// every consumer updates live — whether it referenced the brush through a
   /// <c>DynamicResource</c> in XAML or holds a direct reference (e.g. the log
   /// viewer assigns these instances to each line). This keeps all state colours
   /// (success / warning / danger / info, plus the log-severity palette) legible
   /// and on-brand across light, dark and high-contrast themes — instead of the
   /// previous hardcoded values that were tuned for dark and failed on light.
   /// </summary>
   public static class ThemeTokens
   {
      // Status / semantic accents.
      public static readonly SolidColorBrush Brand = new();
      public static readonly SolidColorBrush Success = new();
      public static readonly SolidColorBrush Warning = new();
      public static readonly SolidColorBrush Danger = new();
      public static readonly SolidColorBrush Info = new();
      public static readonly SolidColorBrush Neutral = new();

      // Log-severity palette.
      public static readonly SolidColorBrush LogDefault = new();
      public static readonly SolidColorBrush LogSmtp = new();
      public static readonly SolidColorBrush LogImap = new();
      public static readonly SolidColorBrush LogPop3 = new();
      public static readonly SolidColorBrush LogApp = new();
      public static readonly SolidColorBrush LogError = new();

      /// <summary>
      /// Raised after every recomputation, so consumers that cannot express
      /// themselves as a brush reference can rebuild.
      ///
      /// The charts are the reason this exists. A LiveCharts series holds Skia
      /// paints, not WPF brushes, so it can neither take a DynamicResource nor
      /// benefit from the in-place brush mutation the rest of the app relies on:
      /// the only way a chart follows a theme change is to be told about it and
      /// rebuild its paints. Subscribers should hook this on Loaded and unhook on
      /// Unloaded rather than in a constructor - the pages are cached and swapped
      /// in and out of the content host, and a static event is exactly the kind of
      /// root that keeps a dead page alive forever.
      /// </summary>
      public static event EventHandler Changed;

      /// <summary>
      /// Which palette the charts must use, recomputed with the tokens.
      ///
      /// High Contrast wins over the light/dark preference: the user asked the
      /// operating system for it, and the Control Panel's own theme toggle is not
      /// a licence to override that.
      /// </summary>
      public static ChartTheme CurrentChartTheme { get; private set; } = ChartTheme.Dark;

      /// <summary>
      /// The system colours the High Contrast palette is built from, snapshotted
      /// on each refresh so the WPF-free palette code never has to touch
      /// SystemColors itself.
      /// </summary>
      public static ChartSystemColors CurrentSystemColors { get; private set; } = ChartSystemColors.Fallback;

      private static bool initialised_;

      /// <summary>
      /// Computes the initial colours and subscribes to theme changes.
      /// Safe to call more than once.
      /// </summary>
      public static void Initialize()
      {
         if (initialised_)
            return;
         initialised_ = true;

         // A focus ring that is visible on every theme is part of the same job as
         // the colour tokens, and it is registered here rather than in App.xaml so
         // that the whole of it lives in one file.
         AccessibleFocus.Register();

         // And the ring and glow round a text field that has the keyboard, in
         // the focus colour published below.
         FocusGlow.Register();

         Refresh();

         try
         {
            ApplicationThemeManager.Changed += (_, __) => Refresh();
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            // If the theme manager doesn't raise changes we still refresh
            // explicitly from the theme toggle (see MainWindow).
         }

         // Windows' own accessibility switches, which the WPF-UI theme manager knows
         // nothing about. Without this the High Contrast palette was only ever picked
         // up at startup or when the user pressed the light/dark toggle - and pressing
         // that toggle calls SystemThemeWatcher.UnWatch, so for anybody who had ever
         // used it, turning High Contrast on in Windows did nothing at all until the
         // application was restarted. That is the headline requirement of the
         // accessibility work, and it was the one thing not wired up.
         //
         // UserPreferenceChanged rather than SystemParameters.StaticPropertyChanged:
         // it is raised for the Accessibility, Color, VisualStyle and General
         // categories, which between them cover turning High Contrast on and switching
         // between High Contrast themes. It arrives on a background thread, so the
         // refresh is marshalled to the dispatcher - the tokens are read by the UI and
         // Changed subscribers touch WPF objects.
         try
         {
            SystemEvents.UserPreferenceChanged += OnUserPreferenceChanged_;
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            // A missing SystemEvents pump is not a reason to fail startup; the theme
            // toggle still refreshes explicitly.
         }
      }

      private static void OnUserPreferenceChanged_(object sender, UserPreferenceChangedEventArgs e)
      {
         if (e.Category != UserPreferenceCategory.Accessibility &&
             e.Category != UserPreferenceCategory.Color &&
             e.Category != UserPreferenceCategory.VisualStyle &&
             e.Category != UserPreferenceCategory.General)
            return;

         try
         {
            System.Windows.Threading.Dispatcher dispatcher = Application.Current?.Dispatcher;

            if (dispatcher != null && !dispatcher.CheckAccess())
               dispatcher.BeginInvoke(new Action(Refresh));
            else
               Refresh();
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            // Never let a system notification take the application down.
         }
      }

      /// <summary>
      /// The three status colours for the light or the dark theme, from
      /// <see cref="StatusPalette"/> - the one place they live, where a test holds
      /// every pair apart for colour-blind eyes.
      /// </summary>
      public static (Color success, Color warning, Color danger) StatusColours(bool light)
      {
         (uint success, uint warning, uint danger) = StatusPalette.Argb(light);
         return (FromArgb(success), FromArgb(warning), FromArgb(danger));
      }

      private static Color FromArgb(uint argb)
      {
         return Color.FromArgb((byte)(argb >> 24), (byte)(argb >> 16), (byte)(argb >> 8), (byte)argb);
      }

      /// <summary>
      /// Whether the application is in High Contrast: Windows says so, or
      /// HMAILCP_HIGH_CONTRAST=1 in the environment says to behave as if it did.
      /// The variable exists for one reason - the High Contrast theme changes
      /// every control in the application, and a change like that has to be looked
      /// at, so build/capture-cp.ps1 -HighContrast launches the Control Panel with
      /// it set and captures the result on an ordinary desktop. It is read once.
      /// </summary>
      public static bool IsHighContrast => SystemParameters.HighContrast || ForcedHighContrast;

      private static readonly bool ForcedHighContrast =
         string.Equals(Environment.GetEnvironmentVariable("HMAILCP_HIGH_CONTRAST"), "1", StringComparison.Ordinal);

      /// <summary>Recomputes every token colour for the current theme.</summary>
      public static void Refresh()
      {
         // Snapshot what the charts need before we start branching, so there is
         // exactly one decision about which theme is in force.
         CurrentSystemColors = ReadSystemColors();
         CurrentChartTheme = IsHighContrast
            ? ChartTheme.HighContrast
            : IsLight() ? ChartTheme.Light : ChartTheme.Dark;

         // The status palette, the surfaces and the live log's palette come from
         // DesignTokens - the one WPF-free table, held by a test - for the theme
         // decided above. The log's palette is the log viewer's own, not a design
         // token, but it is measured there with the rest: its dark values were
         // once chosen against #1B1B1B and failed 4.5:1 on the card the log sits
         // on. Under High Contrast every one is a colour of the system palette
         // that ReadSystemColors just snapshotted.
         DesignTokens.StatusArgb status = DesignTokens.Status(CurrentChartTheme, CurrentSystemColors);
         DesignTokens.SurfaceArgb surfaces = DesignTokens.Surfaces(CurrentChartTheme, CurrentSystemColors);
         DesignTokens.LogArgb log = DesignTokens.Log(CurrentChartTheme, CurrentSystemColors);

         Color brand = FromArgb(status.Brand);
         Color success = FromArgb(status.Success);
         Color warning = FromArgb(status.Warning);
         Color danger = FromArgb(status.Danger);
         Color info = FromArgb(status.Info);
         Color neutral = FromArgb(status.Neutral);

         Color logDefault = FromArgb(log.Other);
         Color logSmtp = FromArgb(log.Smtp);
         Color logImap = FromArgb(log.Imap);
         Color logPop3 = FromArgb(log.Pop3);
         Color logApp = FromArgb(log.Application);
         Color logError = FromArgb(log.Error);

         // Channel 1: mutate the shared, never-frozen instances in place.
         Set(Brand, brand);
         Set(Success, success);
         Set(Warning, warning);
         Set(Danger, danger);
         Set(Info, info);
         Set(Neutral, neutral);
         Set(LogDefault, logDefault);
         Set(LogSmtp, logSmtp);
         Set(LogImap, logImap);
         Set(LogPop3, logPop3);
         Set(LogApp, logApp);
         Set(LogError, logError);

         // App.xaml declares "BrandBrush" as a fixed dark-theme blue (#2F81F7), and
         // every StaticResource reference to it - the sidebar pill, the tab
         // underline, the per-page icons - kept that blue on a light surface, where
         // it reads washed-out. The declared instance is mutated IN PLACE rather
         // than replaced, and the difference is load order: a StaticResource
         // resolves to an instance once, when its view loads, so replacing the
         // dictionary entry only reaches views loaded afterwards - and the App
         // constructor (where Initialize runs) executes before App.xaml's
         // resources even exist. Mutating the declared brush updates every
         // consumer that ever resolved it, whenever the theme changes. XAML
         // resource brushes are not frozen unless marked so; the guard keeps a
         // future Freeze from becoming a crash here.
         if (Application.Current?.Resources["BrandBrush"] is SolidColorBrush declaredBrand && !declaredBrand.IsFrozen)
            declaredBrand.Color = brand;

         // Channel 2: publish fresh brushes for XAML DynamicResource consumers.
         Publish("AppBrandBrush", brand);
         Publish("AppSuccessBrush", success);
         Publish("AppWarningBrush", warning);
         Publish("AppDangerBrush", danger);
         Publish("AppInfoBrush", info);
         Publish("AppNeutralBrush", neutral);

         // The surfaces and the notice tint, declared with their dark values in
         // Views/Scaffold/Tokens.xaml so that they exist from the first parse,
         // and republished here for the theme in force. Setting the key on the
         // application's own dictionary shadows the merged declaration.
         Publish("AppCardBackgroundBrush", FromArgb(surfaces.CardBackground));
         Publish("AppCardBorderBrush", FromArgb(surfaces.CardBorder));
         Publish("AppCardHoverBrush", FromArgb(surfaces.CardHover));
         Publish("AppDividerBrush", FromArgb(surfaces.Divider));
         PublishValue("AppNoticeTintOpacity", DesignTokens.NoticeTintOpacityFor(CurrentChartTheme));

         // The status pill's box: a fill and the colour of the word on it per
         // level, and its outline, which only High Contrast draws.
         foreach (StatusLevel level in StatusSemantics.AllLevels)
         {
            StatusPresentation presentation = StatusSemantics.For(level);
            DesignTokens.StatusFillArgb fill = DesignTokens.StatusFill(level, CurrentChartTheme, CurrentSystemColors);
            Publish(presentation.FillBrushKey, FromArgb(fill.Fill));
            Publish(presentation.FillTextBrushKey, FromArgb(fill.Text));
         }
         PublishValue("AppStatusFillBorderThickness", new Thickness(DesignTokens.StatusFillBorderFor(CurrentChartTheme)));

         // The light theme's tertiary text, darkened to clear 4.5:1 (the reason
         // is at DesignTokens.LightTertiaryText). Set on the application's own
         // dictionary it shadows the theme's for every DynamicResource user -
         // the TextCaptionTertiary hints, the breadcrumb, the shell - and taken
         // away again it hands the dark and High Contrast themes back theirs.
         if (DesignTokens.TertiaryTextFor(CurrentChartTheme) is uint tertiary)
            Publish("TextFillColorTertiaryBrush", FromArgb(tertiary));
         else
            Application.Current?.Resources.Remove("TextFillColorTertiaryBrush");

         // The focus colour (DesignTokens.Focus): the ring FocusGlow draws round
         // a text field that has the keyboard, the glow outside it, and the
         // border the navigation draws round the row the keyboard is on. The
         // theme's own line under a focused text box, password box, number box
         // and editable combo box takes it too - it is what a field shows where
         // FocusGlow cannot draw (a window without an adorner layer, a field
         // drawn as an underline). Under High Contrast those two keys are taken
         // away again, handing the theme its own, and FocusGlow draws nothing.
         DesignTokens.FocusArgb focus = DesignTokens.Focus(CurrentChartTheme, CurrentSystemColors);
         Publish("AppFocusBrush", FromArgb(focus.Ring));
         Publish("AppFocusGlowBrush", FromArgb(focus.Glow));
         foreach (string themeKey in new[] { "TextControlFocusedBorderBrush", "ComboBoxBorderBrushFocused" })
         {
            if (DesignTokens.FocusGlowFor(CurrentChartTheme))
               Publish(themeKey, FromArgb(focus.Ring));
            else
               Application.Current?.Resources.Remove(themeKey);
         }

         // A selected row's fill, and the outline only High Contrast draws
         // round it (DesignTokens.Selection): the lists' selection language,
         // which App.xaml's row styles give every grid, list box and list view
         // beside the sidebar's pill.
         DesignTokens.SelectionArgb selection = DesignTokens.Selection(CurrentChartTheme, CurrentSystemColors);
         Publish("AppSelectionFillBrush", FromArgb(selection.Fill));
         Publish("AppSelectionOutlineBrush", FromArgb(selection.Outline));

         // The filled destructive button (DesignTokens.DangerSolid): the fill at
         // rest, under the pointer and pressed, and its words, which App.xaml's
         // button style gives every Appearance="Danger" in place of WPF-UI's own
         // #F44336, whose white words in the dark theme measured 3.7:1.
         DesignTokens.DangerSolidArgb dangerSolid = DesignTokens.DangerSolid(CurrentChartTheme, CurrentSystemColors);
         Publish("AppDangerSolidBrush", FromArgb(dangerSolid.Fill));
         Publish("AppDangerSolidHoverBrush", FromArgb(dangerSolid.Hover));
         Publish("AppDangerSolidPressedBrush", FromArgb(dangerSolid.Pressed));
         Publish("AppOnDangerSolidBrush", FromArgb(dangerSolid.Text));

         // A text field's border at rest (DesignTokens.TextFieldStroke): the
         // theme's own key, which the text box, password box and number box draw
         // their border in (and the date picker, through App.xaml's style), set
         // on the application's own dictionary so that it shadows the theme's.
         // Fluent's drew the outline at 1.1:1 and the bottom edge at 1.5 to 1.7:1
         // on the card; the bottom edge is the theme's strong stroke now, 3:1 or
         // more. Under High Contrast the key is taken away again, handing the
         // theme its own.
         if (DesignTokens.TextFieldStroke(CurrentChartTheme) is DesignTokens.TextFieldStrokeArgb fieldStroke)
            PublishValue("TextControlElevationBorderBrush", TextFieldBorder_(fieldStroke));
         else
            Application.Current?.Resources.Remove("TextControlElevationBorderBrush");

         Publish("LogDefaultBrush", logDefault);
         Publish("LogSmtpBrush", logSmtp);
         Publish("LogImapBrush", logImap);
         Publish("LogPop3Brush", logPop3);
         Publish("LogAppBrush", logApp);
         Publish("LogErrorBrush", logError);

         // Channel 3: tell the consumers that cannot be expressed as a brush.
         //
         // Invoked one handler at a time, deliberately. A single try/catch around
         // Changed?.Invoke looks equivalent and is not: an exception from the first
         // subscriber abandons the rest of the invocation list, so with two chart cards
         // subscribed the second silently keeps the previous theme's paints - and in
         // High Contrast that means a card still painted in colours the user has just
         // told Windows they cannot see. Wrapping each handler means one broken
         // consumer cannot cost the others their restyle.
         //
         // Still swallowed rather than propagated, for the original reason: a chart
         // that failed to restyle is not worth taking the window down for. But it is
         // reported now instead of vanishing.
         EventHandler handlers = Changed;

         if (handlers == null)
            return;

         foreach (Delegate handler in handlers.GetInvocationList())
         {
            try
            {
               ((EventHandler)handler)(null, EventArgs.Empty);
            }
            catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
            {
               App.LogException(ex);
            }
         }
      }

      /// <summary>
      /// Snapshots the system colours the High Contrast chart palette is built
      /// from. SystemColors is safe to read at any time, but it is only meaningful
      /// once a WPF application exists, so fall back to the classic High Contrast
      /// White values rather than to zeroes (which would be transparent black -
      /// invisible on every surface).
      /// </summary>
      private static ChartSystemColors ReadSystemColors()
      {
         try
         {
            return new ChartSystemColors(
               ToArgb(SystemColors.WindowTextColor),
               ToArgb(SystemColors.WindowColor),
               ToArgb(SystemColors.HighlightColor),
               ToArgb(SystemColors.HotTrackColor),
               ToArgb(SystemColors.GrayTextColor));
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            return ChartSystemColors.Fallback;
         }
      }

      private static uint ToArgb(Color color)
      {
         return ((uint)color.A << 24) | ((uint)color.R << 16) | ((uint)color.G << 8) | color.B;
      }

      private static bool IsLight()
      {
         try
         {
            return ApplicationThemeManager.GetAppTheme() == ApplicationTheme.Light;
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            return false;
         }
      }

      private static void Set(SolidColorBrush brush, Color color)
      {
         if (!brush.IsFrozen)
            brush.Color = color;
      }

      private static void Publish(string key, Color color)
      {
         if (Application.Current == null)
            return;
         Application.Current.Resources[key] = new SolidColorBrush(color);
      }

      /// <summary>
      /// A text field's border: the shape of Fluent's own elevation border - a
      /// gradient three pixels tall, in absolute units, flipped to run from the
      /// bottom edge up - with the bottom pixel row in the edge colour and
      /// everything above it in the outline colour, the two meeting at a hard
      /// stop, so that the sides keep the subtle outline and the bottom edge is
      /// the line a reader finds the field by.
      /// </summary>
      private static Brush TextFieldBorder_(DesignTokens.TextFieldStrokeArgb stroke)
      {
         var brush = new LinearGradientBrush
         {
            MappingMode = BrushMappingMode.Absolute,
            StartPoint = new Point(0, 0),
            EndPoint = new Point(0, 3),
            RelativeTransform = new ScaleTransform(1, -1, 0, 0.5)
         };
         brush.GradientStops.Add(new GradientStop(FromArgb(stroke.Edge), 1.0 / 3));
         brush.GradientStops.Add(new GradientStop(FromArgb(stroke.Outline), 1.0 / 3));
         brush.Freeze();
         return brush;
      }

      private static void PublishValue(string key, object value)
      {
         if (Application.Current == null)
            return;
         Application.Current.Resources[key] = value;
      }
   }
}
