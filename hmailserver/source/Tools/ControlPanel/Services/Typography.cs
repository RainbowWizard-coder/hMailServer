// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// The Control Panel type ramp - re-based 21 August 2026 onto the Windows 11
   /// (Fluent) ramp: Caption 12, Body 14, Body Strong 14 SemiBold, Subtitle 20,
   /// Title 28. The previous values were a hand-rolled 12 / 12.5 / 13 / 13.5 /
   /// 14 / 15 / 18 scale: five of seven constants were off the Windows ramp and
   /// four used half-pixel sizes, so even the sites that dutifully used the
   /// tokens rendered a non-native, sub-pixel scale. Because these are consts,
   /// re-basing them moves every consuming site at once.
   ///
   /// Page-level roles (page title/subtitle, KPI values, status labels) live as
   /// named styles in App.xaml and were re-based in the same change.
   ///
   /// Each size is the ramp's rung at the Windows text size in force
   /// (TextScaleFactor, put in force by TextScale at start-up): the rungs were
   /// constants, so raising Text size to 150 per cent changed none of the text a
   /// page sets in code (review finding F317). At 100 per cent they are the
   /// numbers below.
   /// </summary>
   public static class Typography
   {
      /// <summary>Small caption / hint text (e.g. card blurbs, status lines). Fluent Caption.</summary>
      public static double Caption => TextScaleFactor.Scaled(12);

      /// <summary>Secondary text and field labels. Same size as Caption on the
      /// Fluent ramp - the old 12.5 sat between the two rungs and landed on
      /// neither.</summary>
      public static double Label => TextScaleFactor.Scaled(12);

      /// <summary>Default body text and input-control text. Fluent Body.</summary>
      public static double Body => TextScaleFactor.Scaled(14);

      /// <summary>Checkboxes and slightly emphasised body text. Fluent Body -
      /// emphasis on the ramp is carried by weight (SemiBold), not by a
      /// half-pixel size bump.</summary>
      public static double Control => TextScaleFactor.Scaled(14);

      /// <summary>List/item titles. Fluent Body; pair with SemiBold.</summary>
      public static double ItemTitle => TextScaleFactor.Scaled(14);

      /// <summary>Card / section headings inside a page. Fluent Body Strong is
      /// this size with SemiBold, which is how every consumer already uses it.</summary>
      public static double SectionHeading => TextScaleFactor.Scaled(14);

      /// <summary>A help document's level-two heading, between Body and Subtitle (its
      /// level-one heading is the Subtitle, DialogTitle): at 150 per cent it was 16 over
      /// a body of 21.</summary>
      public static double DocumentHeading => TextScaleFactor.Scaled(16);

      /// <summary>Modal dialog header. Fluent Subtitle.</summary>
      public static double DialogTitle => TextScaleFactor.Scaled(20);

      /// <summary>Page titles, and the one-time code a person types from a phone. Fluent Title.</summary>
      public static double Title => TextScaleFactor.Scaled(28);

      /// <summary>The UI face. Segoe UI Variable is the Windows 11 face and the
      /// single cheapest "looks native" property in the application; classic
      /// Segoe UI is the Windows 10 fallback. Nothing set any FontFamily at all
      /// before this existed, which left classic Segoe UI everywhere.</summary>
      public const string UiFontFamily = "Segoe UI Variable Text, Segoe UI"; // no-loc

      /// <summary>The one monospace stack. There were four divergent ones
      /// (Consolas alone, two orderings of Cascadia/Consolas, and one with a
      /// css-ish "monospace" entry WPF ignores).</summary>
      public const string MonoFontFamily = "Cascadia Mono, Consolas"; // no-loc

      /// <summary>The one size of text in <see cref="MonoFontFamily"/>: a cipher list,
      /// an LDAP filter, a log line, a script, a token, a path. It was 12 for the live
      /// log, the Label size for the diagnostics and 14 for the event script (review
      /// finding F304); 13 sets the monospaced face to the UI face's 14 by its x-height.
      /// AppFontSizeMono in Tokens.xaml says the same, and both follow the Windows text
      /// size as the rest of the ramp does (review finding F317).</summary>
      public static double Mono => TextScaleFactor.Scaled(13);
   }
}
