// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Windows.Forms;
using hMailServer.Shared;

namespace DBSetup
{
   static class Program
   {
      // Exit codes consumed by the installer: a cancelled or incomplete database
      // setup must not look like a successful install.
      private const int ExitSuccess = 0;
      private const int ExitSetupIncomplete = 1;
      private const int ExitAuthenticationCancelled = 3;

      /// <summary>
      /// The main entry point for the application.
      /// </summary>
      [STAThread]
      static int Main()
      {
         ToolApplication.Initialize();

         CommandLineParser.Parse();

         // /unattended: the database the HM_DB_ variables name, set up with no window
         // at all, the result in the exit code (Unattended.cs). Decided before the
         // server is contacted, so that settings it refuses change nothing. Without
         // the switch the wizard runs as it always has, whatever the environment
         // holds: a variable left in an administrator's environment for a container
         // does not turn the Start menu's Database Setup into a silent one.
         if (UnattendedSetup.IsRequested())
            return UnattendedSetup.Run(Console.Out, Console.Error);

         hMailServer.Application application = new hMailServer.Application();
         if (!Authenticator.AuthenticateUser(application))
            return ExitAuthenticationCancelled;

         Globals.SetApp(application);

         formMain main = new formMain();
         Application.Run(main);

         // Closing the wizard before it finished leaves the database unconfigured.
         return main.SetupCompleted ? ExitSuccess : ExitSetupIncomplete;
      }
   }
}