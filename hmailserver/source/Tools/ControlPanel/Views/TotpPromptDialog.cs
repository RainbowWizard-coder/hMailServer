// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Windows;
using hMailServer.ControlPanel.Services;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Views
{
   /// <summary>
   /// Prompts for a 6-digit two-factor verification code at login. On the
   /// standard frame: one field, OK takes Enter, Cancel takes Escape, and the
   /// keyboard is in the code box when the dialog opens.
   ///
   /// It says whose code it asks for (review finding F241): the server's, which
   /// it enforces on the administrator credential and asks for with the password,
   /// or this computer's, which this Control Panel checks itself after the server
   /// has accepted the password. With both on, the administrator was asked twice by
   /// identical dialogs, both titled "Two-factor authentication", for codes from two
   /// different entries of the authenticator.
   /// </summary>
   public class TotpPromptDialog : FluentDialogWindow
   {
      /// <summary>Whose code the prompt asks for.</summary>
      public enum Factor
      {
         /// <summary>The server's, on the administrator credential: sign-in, the REST session, a password check.</summary>
         Server,

         /// <summary>This Control Panel's own, on this computer, after the password (TotpManager).</summary>
         ThisComputer
      }

      private readonly Wpf.Ui.Controls.TextBox code_ = new()
      {
         // 28 is the ramp's Title rung: six digits read from a phone are typed
         // one at a time and checked by eye, and the setup dialog shows the same.
         FontSize = Typography.Title,
         FontFamily = new System.Windows.Media.FontFamily(Typography.MonoFontFamily),
         MaxLength = 6,
         MinWidth = 200,
         PlaceholderText = "000000",
         HorizontalAlignment = HorizontalAlignment.Left,
         HorizontalContentAlignment = HorizontalAlignment.Center
      };

      public string Code => code_.Text.Trim();

      /// <summary>The server's code, which every caller but the Control Panel's own sign-in check asks for.</summary>
      public TotpPromptDialog(Window owner) : this(owner, Factor.Server)
      {
      }

      public TotpPromptDialog(Window owner, Factor factor)
      {
         Owner = owner;
         string heading = factor == Factor.ThisComputer ? L("This computer's Control Panel code") : L("The server's administrator code");
         string description = factor == Factor.ThisComputer
            ? L("The code this Control Panel asks for on this computer, from its own entry in your authenticator app - not the server's administrator code.")
            : L("The code the server asks for with the administrator password, from the administrator's entry in your authenticator app.");
         Title = heading;

         var ok = new Wpf.Ui.Controls.Button { Content = L("OK"), Appearance = Wpf.Ui.Controls.ControlAppearance.Primary, MinWidth = 88 };
         ok.Click += (s, e) => { DialogResult = true; Close(); };
         var cancel = new Wpf.Ui.Controls.Button { Content = L("Cancel"), MinWidth = 88 };
         cancel.Click += (s, e) => { DialogResult = false; Close(); };

         UseFrame(heading,
            DialogFields.Field(L("6-digit _code from your authenticator app"), code_),
            ok, cancel, description, width: DesignTokens.Dialog.NarrowWidth);
      }
   }
}
