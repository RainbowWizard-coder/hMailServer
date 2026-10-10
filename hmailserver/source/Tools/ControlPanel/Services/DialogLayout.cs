// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// The arithmetic behind a tabbed dialog on the standard frame. The frame
   /// sizes a dialog to its content and caps it at a fraction of the work area;
   /// a tab control inside it is sized to its tallest tab so the window does not
   /// change height with every tab, and that height has a floor (a dialog whose
   /// tabs are all short still needs room to be a dialog) and a ceiling (the
   /// window's cap less what the frame draws around the tabs, past which the
   /// tallest tab scrolls inside its own viewer). Plain numbers, so a test can
   /// hold the three cases.
   /// </summary>
   public static class DialogLayout
   {
      /// <summary>
      /// What the frame draws around a tab control: the title bar, the frame's
      /// inset above and below, the heading and its gap, the footer and its gap.
      /// Generous rather than exact, because the ceiling only has to keep the
      /// footer on screen.
      /// </summary>
      public const double FrameChrome = 200;

      /// <summary>The least a tab control is given, strip included.</summary>
      public const double MinTabControlHeight = 240;

      /// <summary>The window height the frame allows on a work area of this height (<see cref="DesignTokens.Dialog.MaxHeightFraction"/>).</summary>
      public static double MaxWindowHeight(double workAreaHeight)
         => Math.Max(MinTabControlHeight, workAreaHeight * DesignTokens.Dialog.MaxHeightFraction);

      /// <summary>The height a tab control takes: its tallest tab, no less than the floor, no more than the window allows.</summary>
      public static double TabControlHeight(double tallestTab, double maxWindowHeight)
      {
         double ceiling = Math.Max(MinTabControlHeight, maxWindowHeight - FrameChrome);
         return Math.Min(Math.Max(tallestTab, MinTabControlHeight), ceiling);
      }

      /// <summary>
      /// The most a tab control of many tabs is given, strip included, whatever
      /// the work area allows: the domain and account dialogs (review finding
      /// F194), whose tabs range from three fields to a form of twenty and to
      /// whole embedded lists. Given the ceiling - nine tenths of the work area
      /// less the frame - they opened 1,180 pixels tall on a large screen,
      /// General's three fields above 780 pixels of nothing and Save at the foot
      /// of the screen. A tab taller than this scrolls inside it, as the tallest
      /// tab always did past the ceiling.
      /// </summary>
      public const double ManyTabsCap = 640;

      /// <summary>
      /// The least a tab that holds an embedded list is given, strip included:
      /// the list's toolbar, a hint and a list of several rows, so that the list
      /// tabs of a dialog whose form tabs are all short are not three rows high.
      /// </summary>
      public const double ListTabFloor = 360;

      /// <summary>
      /// The height a tab control of many tabs takes: its tallest tab, no less than
      /// <paramref name="floor"/> or the frame's own floor, no more than
      /// <paramref name="cap"/> or what the window allows.
      /// </summary>
      public static double TabControlHeight(double tallestTab, double maxWindowHeight, double floor, double cap)
      {
         double ceiling = Math.Min(Math.Max(MinTabControlHeight, cap), Math.Max(MinTabControlHeight, maxWindowHeight - FrameChrome));
         return Math.Min(Math.Max(tallestTab, Math.Max(floor, MinTabControlHeight)), ceiling);
      }

      /// <summary>
      /// What a strip's headers may grow by once laid out: the selected header is
      /// drawn SemiBold, a few pixels wider than the Regular it was measured in,
      /// and any header can be the selected one.
      /// </summary>
      public const double StripSlack = 16;

      /// <summary>
      /// The width a tabbed dialog asks for so that its strip shows every header
      /// whole: the dialog's own width where the headers fit in it, or the strip's
      /// width and the frame's inset on both sides, no more than
      /// <see cref="DesignTokens.Dialog.MaxWidth"/> - past which the strip scrolls
      /// (review finding F193). The route and IP range dialogs' five tabs fit the
      /// 560 pixels they named of their own in English and not in German, where
      /// "Authentifizierung" and "Schutz" were cut behind the chevron; they fit the
      /// token's 640 they start from now (F295) in English, German and Japanese.
      /// </summary>
      public static double WidthForStrip(double headersWidth, double preferredWidth)
      {
         double needed = headersWidth + StripSlack + 2 * DesignTokens.Space.Xl;
         return Math.Min(DesignTokens.Dialog.MaxWidth, Math.Max(preferredWidth, Math.Ceiling(needed)));
      }
   }
}
