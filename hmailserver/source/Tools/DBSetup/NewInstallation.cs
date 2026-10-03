// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Principal;
using System.ServiceProcess;
using System.Threading.Tasks;
using Microsoft.Win32;
using hMailServer.Shared;

namespace DBSetup
{
   /// <summary>
   /// The mark the installer leaves on a new installation whose database step did not
   /// complete (PendingNewInstallation in hMailServerInnoExtension.iss): in
   /// HKEY_LOCAL_MACHINE\SOFTWARE\hMailServer, the 32-bit view the installer writes,
   /// NewInstallationPending holds the account the service was to run as - virtual or
   /// localsystem - and NewInstallationProfile the profile chosen, if one was. Only an
   /// administrator can write the key. The installer clears it before its own database
   /// step, so the DBSetup it runs never sees it; it is there for DBSetup run afterwards,
   /// by an administrator, which then finishes the installation (NewInstallation).
   /// </summary>
   internal sealed class PendingNewInstallation
   {
      public const string KeyPath = @"SOFTWARE\hMailServer";
      public const string AccountValue = "NewInstallationPending";
      public const string ProfileValue = "NewInstallationProfile";

      private static readonly string[] Profiles = { "office", "gateway", "relay", "sender", "none" };

      /// <summary>virtual, localsystem or keep, as the installer's choice was.</summary>
      public string Account { get; private set; }

      /// <summary>The profile chosen, or null for none chosen.</summary>
      public string Profile { get; private set; }

      public bool MovesToVirtualAccount
      {
         get { return string.Equals(Account, "virtual", StringComparison.Ordinal); }
      }

      /// <summary>
      /// The mark from its two values; null when there is none. A profile that is none
      /// of the installer's words is not one, and is left out.
      /// </summary>
      internal static PendingNewInstallation FromValues(string account, string profile)
      {
         string accountWord = (account ?? string.Empty).Trim().ToLowerInvariant();
         if (accountWord.Length == 0)
            return null;

         string profileWord = (profile ?? string.Empty).Trim().ToLowerInvariant();
         return new PendingNewInstallation
         {
            Account = accountWord,
            Profile = Array.IndexOf(Profiles, profileWord) >= 0 ? profileWord : null
         };
      }

      /// <summary>The mark on this machine, or null.</summary>
      public static PendingNewInstallation Read()
      {
         try
         {
            using (RegistryKey machine = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, RegistryView.Registry32))
            using (RegistryKey key = machine.OpenSubKey(KeyPath))
            {
               if (key == null)
                  return null;
               return FromValues(key.GetValue(AccountValue) as string, key.GetValue(ProfileValue) as string);
            }
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            return null;
         }
      }

      /// <summary>The mark removed. Null when it is gone, else why not.</summary>
      public static string Clear()
      {
         try
         {
            using (RegistryKey machine = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, RegistryView.Registry32))
            using (RegistryKey key = machine.OpenSubKey(KeyPath, true))
            {
               if (key != null)
               {
                  key.DeleteValue(AccountValue, false);
                  key.DeleteValue(ProfileValue, false);
               }
            }
            return null;
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            return ex.Message;
         }
      }
   }

   /// <summary>
   /// What finishing a new installation needs of the machine: hMailServer.exe's verbs,
   /// the service stopped and started, its account changed, and the database's version
   /// read through the running service. The real one is <see cref="MachineHost" />; the
   /// tests give one of their own.
   /// </summary>
   internal interface INewInstallationHost
   {
      /// <summary>hMailServer.exe from Bin with these arguments: its exit code and what it wrote.</summary>
      int RunServer(string[] arguments, out string said);

      /// <summary>Stopped, and its process gone. False, with why, when it is not.</summary>
      bool StopService(out string why);

      /// <summary>Started and running. False, with why, when it is not.</summary>
      bool StartService(out string why);

      /// <summary>
      /// The database's version through the running service and the version this build
      /// needs; false, with why, when the service did not answer them.
      /// </summary>
      bool ReadVersions(out int current, out int required, out string why);
   }

   /// <summary>
   /// The steps the installer runs after the database step of a new installation
   /// (RunPostInstallTasks in hMailServerInnoExtension.iss), run by DBSetup when the
   /// installer left the mark of one it did not finish (<see cref="PendingNewInstallation" />):
   /// the secure defaults of a new installation, the profile chosen, the stored-secrets
   /// key, and - for the virtual account - NT SERVICE\hMailServer's login on SQL Server
   /// with Windows authentication, the grant of its folders, its registration and a start
   /// that is held to opening the database, moved back to LocalSystem when it does not.
   /// The installer advises "run DBSetup.exe" when its database step was refused; until
   /// 6.3.6 that set the database up and nothing else, so the server was left on
   /// LocalSystem, with no self-signed certificate - the database still marked
   /// NewInstallDefaultsPending - and no stored-secrets key (the installer smoke VM,
   /// 2 October 2026). Each step is one of hMailServer.exe's verbs, as the installer runs
   /// it, and is said as it goes; the mark is cleared at the end, whatever the steps
   /// answered, since what each left is said with what to do.
   /// </summary>
   internal static class NewInstallation
   {
      /// <summary>
      /// Whether this process may finish one: the verbs write hMailServer.INI, the
      /// service's registration and folder permissions, which takes an elevated
      /// administrator, as the installer is.
      /// </summary>
      public static bool IsElevated()
      {
         try
         {
            using (WindowsIdentity identity = WindowsIdentity.GetCurrent())
               return new WindowsPrincipal(identity).IsInRole(WindowsBuiltInRole.Administrator);
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            return false;
         }
      }

      /// <summary>The sentence for a new installation this process may not finish.</summary>
      public const string NotElevated =
         "This hMailServer installation's setup did not finish: its database step did not complete. DBSetup sets the database up and then finishes the installation " +
         "as setup would have - the secure defaults, the stored-secrets key and the service's account - and for that it runs as an administrator: " +
         "start it from an elevated prompt (or with Run as administrator), or run hMailServer setup again.";

      /// <summary>
      /// The steps, said line by line through say, and what went wrong through warn.
      /// True when every one did what it is for; false when one did not, which the lines
      /// say, with what to do. The database is set up either way: these come after it.
      /// created says that this run created the database, rather than naming one that
      /// existed, as the installer's g_bDatabaseCreatedHere does: see the move. ran is
      /// false only when the service could not be stopped, so that none of them ran:
      /// the mark is then kept, for setup or DBSetup run again.
      /// </summary>
      public static bool Finish(INewInstallationHost host, PendingNewInstallation pending, bool attended, bool created, Action<string> say, Action<string> warn, out bool ran)
      {
         ran = false;
         bool complete = true;
         Action<string> problem = line => { complete = false; warn(line); };

         say("This is a new installation whose setup did not finish. DBSetup finishes it as setup would have" +
             (pending.MovesToVirtualAccount ? ", the service moved to NT SERVICE\\hMailServer:" : ", the service kept on LocalSystem as chosen:"));

         // The verbs below change a stopped server only, and the service's account
         // changes while it is stopped.
         if (!host.StopService(out string why))
         {
            problem("The hMailServer service did not stop (" + why + "), so nothing after the database was done. Stop it, then run hMailServer setup again, which finishes the installation.");
            return false;
         }
         ran = true;

         // The secure defaults: only a database DBSetup created carries the mark they
         // require, so 5 is the answer for one it attached, which keeps what it has.
         int code = host.RunServer(new[] { "--apply-new-install-defaults" }, out string said);
         Relay(say, said);
         if (code == 0)
            say("The secure defaults of a new installation are applied: a self-signed certificate, STARTTLS on ports 25, 587, 110 and 143, and passwords from other computers only over TLS.");
         else if (code == 5)
            say("The secure defaults were not applied: the database was not created by this setup, and keeps what it has.");
         else
            problem("The secure defaults were not applied (hMailServer.exe --apply-new-install-defaults returned " + code + "). The Control Deck's checklist says what is left; or run that command again from an elevated prompt in the Bin folder with the service stopped.");

         if (pending.Profile != null)
         {
            code = host.RunServer(new[] { "--apply-profile", pending.Profile, "--once", "--if-new", "--confirm" }, out said);
            Relay(say, said);
            if (code == 0)
               say("The " + pending.Profile + " profile chosen for this installation is applied.");
            else
               problem("The " + pending.Profile + " profile was not applied (hMailServer.exe --apply-profile returned " + code + "): apply it from the Control Deck's checklist or the Control Panel's Setting profiles page, which show what it changes first.");
         }

         code = host.RunServer(new[] { "--create-stored-secrets-key" }, out said);
         Relay(say, said);
         if (code == 0)
            say("Export the stored-secrets key now and keep the file with the database backups, stored apart from them: hMailServer.exe --export-stored-secrets-key FILE, from an elevated prompt in the Bin folder.");
         else
            problem("The stored-secrets key was not made (hMailServer.exe --create-stored-secrets-key returned " + code + "): run it again from an elevated prompt in the Bin folder to read why.");

         bool moved = false;
         if (pending.MovesToVirtualAccount)
            moved = MoveToVirtualAccount(host, attended, created, say, problem);

         if (!host.StartService(out why))
         {
            problem("The hMailServer service did not start (" + why + "). The hMailServer error log and the Windows Application log say why.");
            if (moved)
               MoveBack(host, "it did not start", say, problem);
            return false;
         }

         if (moved)
         {
            // The first thing to open the database as the virtual account, and a
            // running service has not necessarily opened anything.
            if (!host.ReadVersions(out int current, out int required, out why))
               MoveBack(host, "it started, and did not open its database (" + why + ")", say, problem);
            else if (current != required)
               MoveBack(host, "it opened its database at version " + current + ", where this build needs " + required, say, problem);
            else
               say("The hMailServer service runs as NT SERVICE\\hMailServer, on its database at version " + current + ".");
         }
         else
            say("The hMailServer service is running.");

         return complete;
      }

      // The move to NT SERVICE\hMailServer, as the installer makes it: the login on
      // SQL Server with Windows authentication first (the verb answers 0 with
      // nothing to do otherwise), the folders granted, and the registration only on
      // a clean grant. A login that could not be given on a database this run
      // created means no move, since no login of that account can have reached it
      // yet; on one that existed, a SQL Server administrator may have given it one,
      // so the move is made and held to the service's opening its database (Finish).
      // True when the service was moved.
      private static bool MoveToVirtualAccount(INewInstallationHost host, bool attended, bool created, Action<string> say, Action<string> problem)
      {
         int code = host.RunServer(new[] { "--grant-database-login" }, out string said);
         if (code != 0 && created)
         {
            problem(Trimmed(said));
            problem("The service was not moved to NT SERVICE\\hMailServer, and runs as LocalSystem: that account could not be given what it needs to open the database. " +
                    "Once a SQL Server administrator has run the statements above, move it from the Control Panel (Windows service account).");
            return false;
         }
         Relay(say, said);
         if (code != 0)
            say("The database existed before, so a SQL Server administrator may have given NT SERVICE\\hMailServer its login there already: the move is made, and the service's opening its database decides.");

         string[] grant = attended ? new[] { "--confirm", "/GrantServiceAccess" } : new[] { "/GrantServiceAccess" };
         code = host.RunServer(grant, out said);
         Relay(say, said);
         if (code != 0)
         {
            problem("The service was not moved to NT SERVICE\\hMailServer, and runs as LocalSystem: the grant of its folders did not complete (hMailServer.exe /GrantServiceAccess returned " + code + "). " +
                    "Move it from the Control Panel (Windows service account), which shows what the grant says.");
            return false;
         }

         code = host.RunServer(new[] { "--service-account", "virtual", "/Register" }, out said);
         Relay(say, said);
         if (code != 0)
         {
            problem("The service was not moved to NT SERVICE\\hMailServer, and runs as LocalSystem: hMailServer.exe --service-account virtual /Register returned " + code + ".");
            return false;
         }

         say("The hMailServer service is registered as NT SERVICE\\hMailServer.");
         return true;
      }

      private static void MoveBack(INewInstallationHost host, string what, Action<string> say, Action<string> problem)
      {
         problem("The hMailServer service was moved to NT SERVICE\\hMailServer, and as that account " + what + ". It is moved back to LocalSystem. " +
                 "Where the database is SQL Server with Windows authentication, hMailServer.exe --grant-database-login, run from an elevated prompt in the Bin folder by a SQL Server administrator, " +
                 "gives the account its login there, or says what to run; then move the service from the Control Panel.");

         if (!host.StopService(out string why))
         {
            problem("It did not stop (" + why + "): run hMailServer.exe --service-account localsystem /Register from an elevated prompt in the Bin folder, then start it.");
            return;
         }

         int code = host.RunServer(new[] { "--service-account", "localsystem", "/Register" }, out string said);
         Relay(say, said);
         if (code != 0)
         {
            problem("hMailServer.exe --service-account localsystem /Register returned " + code + ": run it from an elevated prompt in the Bin folder, then start the service.");
            return;
         }

         if (host.StartService(out why))
            say("The hMailServer service runs as LocalSystem.");
         else
            problem("As LocalSystem it did not start either (" + why + "): the hMailServer error log says why.");
      }

      private static void Relay(Action<string> say, string said)
      {
         foreach (string line in Trimmed(said).Split('\n'))
         {
            string text = line.TrimEnd('\r');
            if (text.Length > 0)
               say("   " + text);
         }
      }

      private static string Trimmed(string text)
      {
         return (text ?? string.Empty).Trim();
      }
   }

   /// <summary>
   /// The machine itself: hMailServer.exe beside DBSetup.exe in Bin, the hMailServer
   /// service through the service controller, its version through COM.
   /// </summary>
   internal sealed class MachineHost : INewInstallationHost
   {
      private const string ServiceName = "hMailServer";
      private static readonly TimeSpan StopBudget = TimeSpan.FromSeconds(120);
      private static readonly TimeSpan StartBudget = TimeSpan.FromSeconds(120);

      public int RunServer(string[] arguments, out string said)
      {
         said = string.Empty;
         try
         {
            var start = new ProcessStartInfo
            {
               FileName = Path.Join(AppContext.BaseDirectory, "hMailServer.exe"),
               UseShellExecute = false,
               CreateNoWindow = true,
               RedirectStandardOutput = true,
               RedirectStandardError = true
            };
            foreach (string argument in arguments)
               start.ArgumentList.Add(argument);

            using (Process process = Process.Start(start))
            {
               if (process == null)
               {
                  said = "hMailServer.exe did not start.";
                  return -1;
               }

               Task<string> output = process.StandardOutput.ReadToEndAsync();
               Task<string> error = process.StandardError.ReadToEndAsync();
               process.WaitForExit();
               said = output.Result + error.Result;
               return process.ExitCode;
            }
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            said = "hMailServer.exe could not be run: " + ex.Message;
            return -1;
         }
      }

      public bool StopService(out string why)
      {
         why = null;
         try
         {
            using (var service = new ServiceController(ServiceName))
            {
               if (service.Status == ServiceControllerStatus.Stopped)
                  return true;

               // The process is found before the stop is asked for, as the
               // installer finds it: the service controller stops naming it once the
               // service has stopped, and the process lives about a second longer.
               Process process = ProcessOf(service);
               if (service.Status != ServiceControllerStatus.StopPending)
                  service.Stop();
               service.WaitForStatus(ServiceControllerStatus.Stopped, StopBudget);

               if (process != null)
               {
                  using (process)
                  {
                     if (!process.WaitForExit(30000))
                     {
                        why = "the service stopped, and its process did not exit within 30 seconds";
                        return false;
                     }
                  }
               }
               else
                  System.Threading.Thread.Sleep(3000);
               return true;
            }
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            why = ex.Message;
            return false;
         }
      }

      public bool StartService(out string why)
      {
         why = null;
         try
         {
            using (var service = new ServiceController(ServiceName))
            {
               if (service.Status != ServiceControllerStatus.Running)
               {
                  if (service.Status != ServiceControllerStatus.StartPending)
                     service.Start();
                  service.WaitForStatus(ServiceControllerStatus.Running, StartBudget);
               }
               return true;
            }
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            why = ex.Message;
            return false;
         }
      }

      public bool ReadVersions(out int current, out int required, out string why)
      {
         current = 0;
         required = 0;
         why = null;
         hMailServer.Application application = null;
         try
         {
            application = new hMailServer.Application();
            hMailServer.Database database = application.Database;
            current = database.CurrentVersion;
            required = database.RequiredVersion;
            Marshal.ReleaseComObject(database);
            return true;
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            why = ex.Message.Trim();
            return false;
         }
         finally
         {
            if (application != null)
               Marshal.ReleaseComObject(application);
         }
      }

      // The service's process, from QueryServiceStatusEx; null when it cannot be
      // found or opened.
      private static Process ProcessOf(ServiceController service)
      {
         try
         {
            var status = new ServiceStatusProcess();
            if (!QueryServiceStatusEx(service.ServiceHandle, 0, ref status, Marshal.SizeOf<ServiceStatusProcess>(), out int _) || status.ProcessId == 0)
               return null;
            return Process.GetProcessById((int)status.ProcessId);
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            return null;
         }
      }

      [StructLayout(LayoutKind.Sequential)]
      private struct ServiceStatusProcess
      {
         public uint ServiceType;
         public uint CurrentState;
         public uint ControlsAccepted;
         public uint Win32ExitCode;
         public uint ServiceSpecificExitCode;
         public uint CheckPoint;
         public uint WaitHint;
         public uint ProcessId;
         public uint ServiceFlags;
      }

      [DllImport("advapi32.dll", SetLastError = true)]
      [return: MarshalAs(UnmanagedType.Bool)]
      private static extern bool QueryServiceStatusEx(SafeHandle service, int infoLevel, ref ServiceStatusProcess buffer, int bufferSize, out int bytesNeeded);
   }
}
