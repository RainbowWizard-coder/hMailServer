// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// The Windows "Text size" setting (Settings > Accessibility > Text size) as the
   /// factor the Control Panel draws its text at, without WPF, so that the type ramp
   /// (Typography) and TextScaleTests can use it (review finding F317). Nothing read
   /// the setting: raising Text size to 125 or 150 per cent changed nothing, and only
   /// display scaling made the Control Panel's text larger.
   ///
   /// Windows keeps the setting as HKCU\Software\Microsoft\Accessibility's
   /// TextScaleFactor, a whole percentage from 100 to 225 (UISettings.TextScaleFactor
   /// is the same number over WinRT); with no value the text is at 100 per cent.
   /// TextScale reads it at start-up and when Windows says a setting changed, and puts
   /// <see cref="Factor"/> in force.
   /// </summary>
   public static class TextScaleFactor
   {
      /// <summary>The smallest and largest factor Windows offers.</summary>
      public const double Smallest = 1.0;
      public const double Largest = 2.25;

      /// <summary>The factor the text is drawn at: 1 until TextScale puts the Windows setting in force.</summary>
      public static double Factor { get; private set; } = 1.0;

      /// <summary>Puts a factor in force, held to what Windows offers; a value that is no number is 1.</summary>
      public static double Use(double factor)
      {
         Factor = Clamp(factor);
         return Factor;
      }

      /// <summary>A factor held to the range Windows offers; anything that is no number is 1.</summary>
      public static double Clamp(double factor)
         => double.IsNaN(factor) || double.IsInfinity(factor) ? 1.0 : Math.Max(Smallest, Math.Min(Largest, factor));

      /// <summary>
      /// The factor of the registry's TextScaleFactor value: a percentage, 100 to 225, as
      /// an integer or its text; 1 for none, for zero and for anything that is not a
      /// whole number.
      /// </summary>
      public static double FromRegistry(object value)
      {
         long percent = value switch
         {
            int i => i,
            long l => l,
            string s when long.TryParse(s.Trim(), System.Globalization.NumberStyles.Integer, System.Globalization.CultureInfo.InvariantCulture, out long parsed) => parsed,
            _ => 0,
         };

         return percent <= 0 ? 1.0 : Clamp(percent / 100.0);
      }

      /// <summary>
      /// A size on the type ramp at the factor in force, to the half pixel: 14 at 150 per
      /// cent is 21, 12 at 125 per cent 15.
      /// </summary>
      public static double Scaled(double size) => Scaled(size, Factor);

      /// <summary>A size at <paramref name="factor"/>, to the half pixel.</summary>
      public static double Scaled(double size, double factor) => Math.Round(size * Clamp(factor) * 2, MidpointRounding.AwayFromZero) / 2;
   }
}
