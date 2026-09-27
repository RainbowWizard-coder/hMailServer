// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using Wpf.Ui;
using Wpf.Ui.Controls;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// Thin wrapper over the WPF-UI snackbar so any view can show a transient
   /// "saved" confirmation without holding a reference to the main window.
   /// </summary>
   public static class Toast
   {
      private static readonly SnackbarService Service = new();
      private static bool ready_;

      /// <summary>Wires the singleton to the main window's snackbar presenter.</summary>
      public static void Init(SnackbarPresenter presenter)
      {
         if (presenter == null)
            return;
         Service.SetSnackbarPresenter(presenter);
         ready_ = true;
      }

      public static void Success(string message, string title = null)
         => Show(title ?? L("Saved"), message, SymbolRegular.CheckmarkCircle24, "AppSuccessFillTextBrush", 3);

      public static void Info(string message, string title = "")
         => Show(title, message, SymbolRegular.Info24, "AppInfoFillTextBrush", 3);

      /// <summary>
      /// Something failed - a page that could not load, a save that did not
      /// complete. It was shown through Info, so a failure arrived looking like
      /// a piece of news. It stays up long enough to read: a failure says what to
      /// do next, and three seconds is not enough for a sentence like that.
      /// </summary>
      public static void Failure(string message, string title = "")
         => Show(title, message, SymbolRegular.ErrorCircle24, "AppDangerFillTextBrush", 8);

      /// <summary>
      /// Every toast is on the theme's neutral surface, with the severity in its
      /// title and in a glyph drawn in the level's colour. Wpf.Ui's coloured
      /// snackbars put white text on #03A9F4 (Info, 2.6:1), #4CAF50 (Success,
      /// 2.8:1) and #F44336 (Danger, 3.7:1), in both themes, and the message
      /// line under the title lighter still - measured at 1.9:1 on a captured
      /// "Server unavailable" toast. The neutral surface carries the theme's
      /// own text colours, 14:1 or better.
      /// </summary>
      private static void Show(string title, string message, SymbolRegular glyph, string glyphBrushKey, int seconds)
      {
         if (!ready_)
            return;
         try
         {
            var icon = new SymbolIcon { Symbol = glyph, FontSize = 20 };
            icon.SetResourceReference(IconElement.ForegroundProperty, glyphBrushKey);
            Service.Show(title, message, ControlAppearance.Secondary, icon, TimeSpan.FromSeconds(seconds));
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            // Never let a UI confirmation crash a successful save.
         }
      }
   }
}
