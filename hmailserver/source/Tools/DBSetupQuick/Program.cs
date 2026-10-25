// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Windows.Forms;
using System.Diagnostics;
using hMailServer.Shared;

namespace DBSetupQuick
{
   static class Program
   {
      /// <summary>
      /// The exit code for a database step that could not run at all: the hMailServer
      /// service could not be reached through COM, or could not say whether a database
      /// is configured, so this program asked nothing of it that changes anything and
      /// started no DBUpdater - neither changed the database. The installer reads it so
      /// (RunPostInstallTasks in hMailServerInnoExtension.iss): with [Database]
      /// UpgradeOnStart off, the service's own start ran no schema step either, and the
      /// database is as the earlier version left it. The other codes are DBUpdater's on
      /// the upgrade path (0 done, 1 the upgrade failed, 2 its configuration, 3 no
      /// sign-in) and this program's own 1 and 3 on the create path. Until 6.3.6 an
      /// unreachable service was an unhandled COMException: the process died with
      /// 0xE0434352 (-532462766), and what had happened was written nowhere.
      /// </summary>
      internal const int ExitServiceNotReached = 4;

      /// <summary>
      /// The argument the installer names a file in, report:PATH, into which this
      /// program writes what it says about a failure as well as on standard error: it is
      /// a Windows program the installer starts without a console, whose output nothing
      /// would read otherwise, and the installer copies the file into its own log. On the
      /// command line and never in the environment, which an elevated program inherits
      /// from the user who started it.
      /// </summary>
      internal const string ReportArgument = "report";

      private static hMailServer.Application _application;

      [STAThread]
      static int Main()
      {
         ToolApplication.Initialize();

         CommandLineParser.Parse();

         string report = CommandLineParser.ContainsArgument(ReportArgument) ? CommandLineParser.GetArgument(ReportArgument) : null;

         bool databaseExists;
         string why;
         if (!Reach(() => new hMailServer.Application(), application => application.Database.DatabaseExists, out _application, out databaseExists, out why))
         {
            Say(report, why);

            if (!CommandLineParser.IsSilent())
               MessageBox.Show(why, "hMailServer", MessageBoxButtons.OK, MessageBoxIcon.Error);

            return ExitServiceNotReached;
         }

         // Propagate the outcome to the installer: a failed database create/upgrade
         // must fail the install rather than silently produce a broken server.
         return databaseExists ? UpgradeDatabase(report) : CreateDatabase(report);
      }

      /// <summary>
      /// The hMailServer service, reached through COM, and whether a database is
      /// configured - the two things asked before anything that changes one. False, with
      /// why, when either throws: the service is not registered, cannot be started, does
      /// not answer, or refuses. Nothing has been asked of it then that changes anything.
      /// </summary>
      internal static bool Reach<T>(Func<T> create, Func<T, bool> databaseExists, out T application, out bool exists, out string why)
         where T : class
      {
         application = null;
         exists = false;
         why = null;

         T reached = null;
         try
         {
            reached = create();
            exists = databaseExists(reached);
            application = reached;
            return true;
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            why = (reached == null
                     ? "DBSetupQuick.exe could not reach the hMailServer service through COM: "
                     : "The hMailServer service did not say whether a database is configured: ") +
                  Describe(ex) + ". Nothing was asked of the service that changes anything, and DBUpdater.exe was not started: " +
                  "neither changed the database. Check that the hMailServer service starts (the Windows Application log and " +
                  "the hMailServer error log say why it does not), then run DBSetupQuick.exe from the hMailServer Bin folder again.";
            return false;
         }
      }

      /// <summary>An exception as a clause: its message, and a COM error's HRESULT.</summary>
      internal static string Describe(Exception ex)
      {
         string message = (ex.Message ?? string.Empty).Replace("\r", " ").Replace("\n", " ").Trim();
         while (message.EndsWith("."))
            message = message.Substring(0, message.Length - 1);
         if (ex is COMException || ex is UnauthorizedAccessException)
            return string.Format("{0} (HRESULT 0x{1:X8})", message, ex.HResult);
         return string.Format("{0}: {1}", ex.GetType().Name, message);
      }

      /// <summary>
      /// What this program says about a failure: on standard error, and appended to the
      /// installer's report file when it named one (ReportArgument). A file that cannot
      /// be written loses the line and nothing else.
      /// </summary>
      internal static void Say(string report, string text)
      {
         Console.Error.WriteLine(text);

         if (string.IsNullOrEmpty(report))
            return;

         try
         {
            File.AppendAllText(report, text + Environment.NewLine);
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            Console.Error.WriteLine("The report could not be written to " + report + ": " + ex.Message);
         }
      }

      private static int UpgradeDatabase(string report)
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

            if (p.ExitCode != 0)
               Say(report, "DBUpdater.exe exited with " + p.ExitCode + " (1 the upgrade failed, 2 its configuration, 3 no sign-in); its own message says why.");

            return p.ExitCode;
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            Say(report, "Failed to start DBUpdater.exe: " + ex.Message);

            if (!CommandLineParser.IsSilent())
               MessageBox.Show("Failed to start DBUpdater.exe" + Environment.NewLine + ex.Message, "hMailServer");

            return 1;
         }
      }

      private static int CreateDatabase(string report)
      {
         // In HM_ADMIN_PASSWORD from the installer, or as password:VALUE from an
         // administrator; none is the empty password a new installation may have.
         string adminPassword = Authenticator.GetGivenPassword() ?? string.Empty;

         try
         {
            if (!Authenticator.AuthenticateUser(_application, adminPassword))
            {
               Say(report, "The hMailServer service did not accept the administrator password, so no database was created.");
               return 3;
            }

            if (_application.Database.DatabaseType == hMailServer.eDBtype.hDBTypeMSSQLCE ||
                _application.Database.DatabaseType == hMailServer.eDBtype.hDBTypeUnknown)
            {
               return InitializeInternalDatabase(report);
            }
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            // The sign-in or the question of which database: nothing created yet.
            Say(report, "Failed to create the hMailServer database: the hMailServer service did not answer (" + Describe(ex) + ").");

            if (!CommandLineParser.IsSilent())
               MessageBox.Show(ex.Message, "hMailServer", MessageBoxButtons.OK, MessageBoxIcon.Error);

            return 1;
         }

         return 0;
      }

      private static int InitializeInternalDatabase(string report)
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
            Say(report, "Failed to create the hMailServer database: " + ex.Message);

            if (!CommandLineParser.IsSilent())
               MessageBox.Show(ex.Message, "hMailServer", MessageBoxButtons.OK, MessageBoxIcon.Error);

            return 1;
         }
      }

   }
}
