// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Windows.Forms;
using System.Diagnostics;
using hMailServer.Shared;

namespace DBSetupQuick
{
   static class Program
   {
      private static hMailServer.Application _application;

      [STAThread]
      static int Main()
      {
         ToolApplication.Initialize();

         CommandLineParser.Parse();

         _application = new hMailServer.Application();

         // Propagate the outcome to the installer: a failed database create/upgrade
         // must fail the install rather than silently produce a broken server.
         return _application.Database.DatabaseExists ? UpgradeDatabase() : CreateDatabase();
      }

      private static int UpgradeDatabase()
      {
         try
         {
            // Database upgrader. Resolve relative to this executable rather than
            // the working directory, which the installer leaves unspecified.
            System.Diagnostics.ProcessStartInfo upgradeProcess = new System.Diagnostics.ProcessStartInfo();
            upgradeProcess.FileName = System.IO.Path.Join(AppContext.BaseDirectory, "DBUpdater.exe");

            // The environment below reaches the child only without the shell.
            upgradeProcess.UseShellExecute = false;

            // Each argument on its own, quoted by .NET by the rules DBUpdater's
            // command line is split by, rather than one string built by hand.
            //
            // Means that it should automatically exit if already up to date. This is always
            // the case when we launch it via 'quick'.
            upgradeProcess.ArgumentList.Add("/SilentIfOk");

            // If the /silent param has been supplied to this process, we should forward it to the updater
            if (CommandLineParser.IsSilent())
               upgradeProcess.ArgumentList.Add("/silent");

            // Forward the administrator password as well. The create path has always
            // done this; the upgrade path did not, and that asymmetry is what made an
            // unattended upgrade hang. DBUpdater authenticates before it can upgrade,
            // had no password to authenticate with, and fell through to a modal dialog
            // nobody was there to answer.
            //
            // In DBUpdater's environment, HM_ADMIN_PASSWORD, and not on its command
            // line: until 26 September 2026 it was appended there unquoted, so a
            // password with a space reached DBUpdater cut at the space, a double quote
            // or backslashes before one changed it, and a silent upgrade failed with
            // exit code 3. Whether this process was given it that way by the
            // installer or as password:VALUE by an administrator.
            string password = Authenticator.GetGivenPassword();
            if (password != null)
               upgradeProcess.Environment[Authenticator.PasswordVariable] = password;

            // Launch upgrader and wait for it to complete.
            Process p = Process.Start(upgradeProcess);
            p.WaitForExit();

            return p.ExitCode;
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            Console.Error.WriteLine("Failed to start DBUpdater.exe: " + ex.Message);

            if (!CommandLineParser.IsSilent())
               MessageBox.Show("Failed to start DBUpdater.exe" + Environment.NewLine + ex.Message, "hMailServer");

            return 1;
         }
      }

      private static int CreateDatabase()
      {
         // In HM_ADMIN_PASSWORD from the installer, or as password:VALUE from an
         // administrator; none is the empty password a new installation may have.
         string adminPassword = Authenticator.GetGivenPassword() ?? string.Empty;

         if (!Authenticator.AuthenticateUser(_application, adminPassword))
            return 3;

         if (_application.Database.DatabaseType == hMailServer.eDBtype.hDBTypeMSSQLCE ||
             _application.Database.DatabaseType == hMailServer.eDBtype.hDBTypeUnknown)
         {
            return InitializeInternalDatabase();
         }

         return 0;
      }

      private static int InitializeInternalDatabase()
      {
         try
         {
            hMailServer.Database database = _application.Database;

            database.CreateInternalDatabase();

            // Database has been upgraded. Reinitialize the connections.
            _application.Reinitialize();

            // Re-initialize to connect to the newly created database.
            _application.Reinitialize();

            return 0;
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            Console.Error.WriteLine("Failed to create the hMailServer database: " + ex.Message);

            if (!CommandLineParser.IsSilent())
               MessageBox.Show(ex.Message, "hMailServer", MessageBoxButtons.OK, MessageBoxIcon.Error);

            return 1;
         }
      }

   }
}