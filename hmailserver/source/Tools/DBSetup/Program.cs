// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

using System;
using System.Collections.Generic;
using System.Linq;
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

      // Windows' answer when the elevation prompt is cancelled.
      private const int ErrorCancelled = 1223;

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

         // A new installation the installer did not finish: the wizard finishes it
         // once the database is set up (ucPerformTask, NewInstallation), which takes
         // an elevated administrator, so it is started again with Windows' prompt -
         // from the Start menu's shortcut it is not elevated.
         if (PendingNewInstallation.Read() != null && !NewInstallation.IsElevated())
            return RunElevated();

         hMailServer.Application application = new hMailServer.Application();
         if (!Authenticator.AuthenticateUser(application))
            return ExitAuthenticationCancelled;

         Globals.SetApp(application);

         formMain main = new formMain();
         Application.Run(main);

         // Closing the wizard before it finished leaves the database unconfigured.
         return main.SetupCompleted ? ExitSuccess : ExitSetupIncomplete;
      }

      // This program again, elevated, with the arguments it was given, and its exit
      // code; the sentence and ExitSetupIncomplete when the prompt was cancelled.
      private static int RunElevated()
      {
         var arguments = new System.Text.StringBuilder();
         foreach (string argument in Environment.GetCommandLineArgs().Skip(1))
         {
            if (arguments.Length > 0)
               arguments.Append(' ');
            arguments.Append('"').Append(argument.Replace("\"", "\\\"")).Append('"');
         }

         try
         {
            var start = new System.Diagnostics.ProcessStartInfo
            {
               FileName = Environment.ProcessPath,
               Arguments = arguments.ToString(),
               UseShellExecute = true,
               Verb = "runas"
            };
            using (var process = System.Diagnostics.Process.Start(start))
            {
               if (process == null)
                  return ExitSetupIncomplete;
               process.WaitForExit();
               return process.ExitCode;
            }
         }
         catch (System.ComponentModel.Win32Exception cancelled) when (cancelled.NativeErrorCode == ErrorCancelled)
         {
            MessageBox.Show(NewInstallation.NotElevated, "hMailServer Database Setup", MessageBoxButtons.OK, MessageBoxIcon.Information);
            return ExitSetupIncomplete;
         }
         catch (Exception other) when (!ExceptionPolicy.IsFatal(other))
         {
            MessageBox.Show(NewInstallation.NotElevated + "\r\n\r\n" + other.Message, "hMailServer Database Setup", MessageBoxButtons.OK, MessageBoxIcon.Information);
            return ExitSetupIncomplete;
         }
      }
   }
}