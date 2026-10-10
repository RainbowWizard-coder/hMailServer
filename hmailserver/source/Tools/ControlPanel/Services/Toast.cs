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

      // The presenter itself, for the failure that offers Try again: a snackbar of
      // its own, since the service's takes a string and nothing else.
      private static SnackbarPresenter presenter_;

      /// <summary>Wires the singleton to the main window's snackbar presenter.</summary>
      public static void Init(SnackbarPresenter presenter)
      {
         if (presenter == null)
            return;
         Service.SetSnackbarPresenter(presenter);
         presenter_ = presenter;
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
      /// A failure that doing it again may mend - a page that could not be entered -
      /// with Try again under the sentence, which runs <paramref name="tryAgain"/> and
      /// closes the toast (review finding F008). The toast offered only its close
      /// button, so the reader had to find the page in the sidebar again or press
      /// Reload on a page that was not on screen. Try again carries no Alt key, as
      /// the read failures' Try again does not (Services/ReadFailure): the toast is
      /// over whatever page is open, whose keys it cannot know.
      /// </summary>
      public static void Failure(string message, string title, Action tryAgain)
      {
         if (tryAgain == null)
         {
            Failure(message, title);
            return;
         }

         if (!ready_ || presenter_ == null)
            return;
         try
         {
            var icon = new SymbolIcon { Symbol = SymbolRegular.ErrorCircle24, FontSize = 20 };
            icon.SetResourceReference(IconElement.ForegroundProperty, "AppDangerFillTextBrush");

            var words = new System.Windows.Controls.TextBlock { Text = message, TextWrapping = System.Windows.TextWrapping.Wrap };
            var again = new Button
            {
               Content = L("Try again"),
               Appearance = ControlAppearance.Secondary,
               Margin = new System.Windows.Thickness(0, DesignTokens.Space.Sm, 0, 0),
               HorizontalAlignment = System.Windows.HorizontalAlignment.Left
            };
            System.Windows.Automation.AutomationProperties.SetAutomationId(again, "toast-try-again");
            var content = new System.Windows.Controls.StackPanel();
            content.Children.Add(words);
            content.Children.Add(again);

            var toast = new Snackbar(presenter_)
            {
               Title = title,
               Content = content,
               Appearance = ControlAppearance.Secondary,
               Icon = icon,
               Timeout = TimeSpan.FromSeconds(FailureWithActionSeconds)
            };
            again.Click += (_, _) =>
            {
               // The presenter's: the snackbar's own Hide is protected.
               if (ReferenceEquals(presenter_.Content, toast))
                  _ = presenter_.HideCurrent();
               tryAgain();
            };
            toast.Show(true);
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            // Never let a UI notice crash the failure it reports.
         }
      }

      /// <summary>
      /// How long a failure with Try again stays: longer than one without, since it
      /// asks for a choice as well as a reading.
      /// </summary>
      private const int FailureWithActionSeconds = 12;

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
