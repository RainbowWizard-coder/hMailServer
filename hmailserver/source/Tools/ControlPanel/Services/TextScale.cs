// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Windows;
using Microsoft.Win32;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// Honours the Windows "Text size" setting (review finding F317): nothing read it, so
   /// raising Text size to 125 or 150 per cent changed nothing and only display scaling
   /// made the Control Panel's text larger.
   ///
   /// At start-up, before the first window (App.OnStartup), <see cref="Install"/> reads
   /// the setting (TextScaleFactor), puts it in force for the type ramp's code-set sizes
   /// (Typography), and sets every font-size resource the application draws text with at
   /// the factor: the ramp's AppFontSize* tokens (Tokens.xaml), which every keyed text
   /// style and the shared templates take by DynamicResource, and WPF-UI's own -
   /// ControlContentThemeFontSize and its neighbours, the size of every box, button,
   /// list and grid - in whichever merged dictionary holds them, scaled from the size it
   /// first held so that a second change does not compound. An icon's size is no text
   /// and keeps its own. Grid columns floor themselves at their words measured in the
   /// scaled font (GridStyles), and dialogs widen with their text (FluentDialogWindow).
   ///
   /// When Windows says a setting changed, the setting is read again; a new text size is
   /// put in force at once for everything drawn from the resources, and a toast says that
   /// pages already open keep the rest of their text at the old size until the Control
   /// Panel is restarted, since a page's code-set sizes are given when it is built and
   /// pages are kept once built.
   /// </summary>
   public static class TextScale
   {
      private const string KeyPath = @"Software\Microsoft\Accessibility"; // no-loc: the Windows registry key
      private const string ValueName = "TextScaleFactor"; // no-loc: the Windows registry value

      // The size each font-size resource held before it was first scaled, by its
      // dictionary and key.
      private static readonly Dictionary<ResourceDictionary, Dictionary<object, double>> bases_ = new();
      private static bool listening_;

      /// <summary>The Windows text size as a factor: 1 where it is not set or cannot be read.</summary>
      public static double ReadWindowsFactor()
      {
         try
         {
            using RegistryKey key = Registry.CurrentUser.OpenSubKey(KeyPath);
            return TextScaleFactor.FromRegistry(key?.GetValue(ValueName));
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            return 1.0;
         }
      }

      /// <summary>Puts the Windows text size in force before the first window, and follows a change of it.</summary>
      public static void Install()
      {
         Apply(ReadWindowsFactor());

         if (listening_)
            return;

         listening_ = true;
         SystemEvents.UserPreferenceChanged += (_, _) => Application.Current?.Dispatcher.BeginInvoke(new Action(FollowWindows_));
      }

      /// <summary>
      /// Puts a text size in force: the type ramp's factor and every font-size resource
      /// of the application at it. Called by Install with the Windows setting, and by the
      /// render harness with a size of its own.
      /// </summary>
      public static void Apply(double factor)
      {
         double f = TextScaleFactor.Use(factor);
         if (Application.Current != null)
            Scale_(Application.Current.Resources, f, new HashSet<ResourceDictionary>());
      }

      /// <summary>
      /// Whether a resource key is one of a text size: the name says FontSize and not an
      /// icon's, or it is the line height of text set in lines (AppLineHeight*), which
      /// rises with its text - at 150 per cent Live logs' 18-pixel lines stood 17 apart.
      /// </summary>
      public static bool IsTextSizeKey(object key)
         => key is string name && ((name.Contains("FontSize", StringComparison.Ordinal) && !name.Contains("Icon", StringComparison.Ordinal))
            || name.StartsWith("AppLineHeight", StringComparison.Ordinal));

      private static void Scale_(ResourceDictionary dictionary, double factor, HashSet<ResourceDictionary> seen)
      {
         if (dictionary == null || !seen.Add(dictionary))
            return;

         var keys = new List<object>();
         foreach (object key in dictionary.Keys)
         {
            if (IsTextSizeKey(key))
               keys.Add(key);
         }

         foreach (object key in keys)
         {
            object value;
            try
            {
               value = dictionary[key];
            }
            catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
            {
               continue;
            }

            if (value is not double size)
               continue;

            if (!bases_.TryGetValue(dictionary, out Dictionary<object, double> bases))
               bases_[dictionary] = bases = new Dictionary<object, double>();
            if (!bases.TryGetValue(key, out double baseSize))
               bases[key] = baseSize = size;

            double scaled = TextScaleFactor.Scaled(baseSize, factor);
            if (scaled != size)
               dictionary[key] = scaled;
         }

         foreach (ResourceDictionary merged in dictionary.MergedDictionaries)
            Scale_(merged, factor, seen);
      }

      private static void FollowWindows_()
      {
         double now = ReadWindowsFactor();
         if (Math.Abs(now - TextScaleFactor.Factor) < 0.001)
            return;

         Apply(now);
         Toast.Info(F("Windows text size is now {0}%. Pages already open keep the rest of their text at the old size until the Control Panel is restarted.",
            (int)Math.Round(now * 100)), L("Text size"));
      }
   }
}
