// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// What the administrator password dialog asks of what was typed before it asks
   /// the server anything.
   ///
   /// The password was one masked box on the Administrative access page, written by
   /// the page's Save together with the mailbox password policy beside it, with no
   /// second box, no current password and nothing said afterwards (review finding
   /// F168). A typo locked the administrator out of this Control Panel, the REST API
   /// and every COM script, and the way back in is an elevated prompt on the server.
   /// The dialog asks for the current password, the new one and the new one again,
   /// and says which box is wrong; this is the part of that which needs no window,
   /// held by AdministratorPasswordChangeTests.
   /// </summary>
   public static class AdministratorPasswordChange
   {
      /// <summary>The dialog's three boxes.</summary>
      public enum Box
      {
         Current,
         New,
         Confirm
      }

      /// <summary>What is wrong, and on which box it is said.</summary>
      public sealed record Problem(Box Box, string Sentence);

      /// <summary>
      /// The first thing wrong with what was typed, in the order the boxes are on
      /// screen, or null when the server can be asked. Passwords are taken as typed:
      /// a space at either end is part of one, as the server takes it.
      /// </summary>
      public static Problem Check(string current, string newPassword, string confirmation)
      {
         if (string.IsNullOrEmpty(current))
            return new Problem(Box.Current, L("Type the administrator password in use now."));

         if (string.IsNullOrEmpty(newPassword))
            return new Problem(Box.New, L("Type the new password."));

         if (!string.Equals(newPassword, confirmation ?? "", StringComparison.Ordinal))
            return new Problem(Box.Confirm, L("The two passwords do not match."));

         return null;
      }

      /// <summary>Said on the current password's box when the server does not take it.</summary>
      public static string WrongCurrent => L("That is not the administrator password in use now.");

      /// <summary>Said once the server has the new password.</summary>
      public static string Changed => L("The administrator password was changed. Use the new one the next time you sign in, in scripts and in REST API clients.");
   }
}
