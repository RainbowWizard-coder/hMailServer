// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Threading;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// A named mutex the Control Panel holds for as long as it runs, by which setup
   /// tells that one is open and asks for it to be closed before an installation
   /// (hMailServerInnoExtension.iss, InitializeSetup).
   ///
   /// Setup looked for the window's title, "hMailServer Control Panel", and nothing
   /// else. Signed in, the title names the server it manages first, "mail.example.com
   /// - hMailServer Control Panel" (review finding F033), and in Japanese, Swedish,
   /// Turkish and Chinese the product's name in it is translated, so the title alone
   /// missed a Control Panel that was open. Setup still looks for the title as well,
   /// which finds a Control Panel from before this mark.
   ///
   /// In the session's namespace, as the title was looked for on the session's own
   /// desktop: a Control Panel another user has open in another session was not
   /// found before and is not now.
   /// </summary>
   public static class ControlPanelRunningMark
   {
      /// <summary>The mutex's name; setup's CheckForMutexes names the same.</summary>
      public const string MutexName = "hMailServerControlPanel"; // no-loc: a mutex's name, setup's too

      // Held until the process ends, when Windows closes the handle; never released
      // by hand, so that no part of the program can let setup think it closed.
      private static Mutex held_;

      /// <summary>
      /// Holds the mark; called once, at start-up. Another Control Panel open at the
      /// same time holds the same mutex as well, which is what a named mutex opened
      /// twice is. Best effort: a mark that could not be made costs setup's warning
      /// and nothing else - the title is still looked for.
      /// </summary>
      public static void Hold()
      {
         if (held_ != null)
            return;

         try
         {
            held_ = new Mutex(false, MutexName);
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            // Deliberately ignored: setup finds the Control Panel by its title alone.
         }
      }
   }
}
