// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.ServiceProcess;
using System.Threading.Tasks;
using System.Windows.Input;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>What a restart of the service came to.</summary>
   public enum ServiceRestartOutcome
   {
      /// <summary>The service stopped, started and the session is attached to it again.</summary>
      Restarted,

      /// <summary>The service could not be stopped and started; the reason says why.</summary>
      NotRestarted,

      /// <summary>The service came back, and the session could not attach to it; the reason says why.</summary>
      NotReattached
   }

   /// <summary>
   /// Restarting the hMailServer service on this machine and attaching the session to
   /// it again: the flow the feature settings pages had to themselves after a save,
   /// shared with the window's pending bar, whose settings waiting for a restart had
   /// nothing to press (review finding F010), and with the directory synchronisation
   /// page's schedule.
   /// </summary>
   public static class ServiceRestart
   {
      /// <summary>
      /// Whether the service this Control Panel is signed in to is the one on this
      /// machine - the only one a restart here can reach. A Control Panel signed in to
      /// another server offers no restart, rather than restarting the wrong service.
      /// </summary>
      public static bool Available => ServerSession.IsLocalSession;

      /// <summary>
      /// Stops and starts the service on a worker thread, then attaches the session to
      /// it again on the caller's, with the reason for whichever step failed.
      /// </summary>
      public static async Task<(ServiceRestartOutcome Outcome, string Reason)> RunAsync()
      {
         string error = await Task.Run(TryRestart);
         if (error != null)
            return (ServiceRestartOutcome.NotRestarted, error);

         string lost = Reattach();
         return lost == null ? (ServiceRestartOutcome.Restarted, null) : (ServiceRestartOutcome.NotReattached, lost);
      }

      /// <summary>
      /// Stops and starts the service, returning null on success or a message
      /// describing the failure. Stopping a service needs administrator rights
      /// and the Control Panel runs asInvoker, so a non-elevated session hands
      /// the work to net.exe behind a single UAC prompt instead of failing
      /// with an opaque "Cannot open hMailServer service" error.
      /// </summary>
      public static string TryRestart()
      {
         try
         {
            bool elevated;
            using (var identity = System.Security.Principal.WindowsIdentity.GetCurrent())
               elevated = new System.Security.Principal.WindowsPrincipal(identity)
                  .IsInRole(System.Security.Principal.WindowsBuiltInRole.Administrator);

            if (elevated)
            {
               using var controller = new ServiceController("hMailServer");
               if (controller.Status != ServiceControllerStatus.Stopped)
               {
                  if (controller.Status == ServiceControllerStatus.Running ||
                      controller.Status == ServiceControllerStatus.Paused)
                     controller.Stop();
                  controller.WaitForStatus(ServiceControllerStatus.Stopped, TimeSpan.FromSeconds(60));
               }
               controller.Start();
               controller.WaitForStatus(ServiceControllerStatus.Running, TimeSpan.FromSeconds(60));
               return null;
            }

            // "net stop" fails harmlessly when the service is already stopped,
            // so chain with "&" rather than "&&".
            //
            // cmd.exe and net.exe by their full System32 paths, and System32 as
            // the working directory: ShellExecuteEx looks for a bare name in the
            // current directory first, and that is whatever folder a file dialog
            // last left it in, so a cmd.exe waiting in Downloads would have run
            // elevated behind the prompt the administrator expects. /d skips any
            // Command Processor AutoRun; with /s, cmd takes off only the outer
            // pair of quotes.
            string system = Environment.SystemDirectory;
            string net = System.IO.Path.Combine(system, "net.exe"); // no-loc
            var startInfo = new System.Diagnostics.ProcessStartInfo
            {
               FileName = System.IO.Path.Combine(system, "cmd.exe"), // no-loc
               Arguments = "/d /s /c \"\"" + net + "\" stop hMailServer & \"" + net + "\" start hMailServer\"", // no-loc
               WorkingDirectory = system,
               UseShellExecute = true,
               Verb = "runas",
               WindowStyle = System.Diagnostics.ProcessWindowStyle.Hidden
            };

            using (var process = System.Diagnostics.Process.Start(startInfo))
               process?.WaitForExit();

            using (var controller = new ServiceController("hMailServer"))
               controller.WaitForStatus(ServiceControllerStatus.Running, TimeSpan.FromSeconds(60));
            return null;
         }
         catch (System.ComponentModel.Win32Exception ex) when (ex.NativeErrorCode == 1223)
         {
            return L("the elevation prompt was cancelled.");
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            return ex.GetBaseException().Message;
         }
      }

      /// <summary>
      /// The COM server lives inside the service process, so restarting it
      /// invalidates every interface pointer the Control Panel holds. Rebuild
      /// the session straight away rather than letting the next page fail with
      /// "The RPC server is unavailable". The service registers its COM class
      /// factory a moment before it can serve calls, hence the retries. Null when
      /// the session is attached again, or there is none; otherwise why not, and
      /// the session is marked broken so that it keeps trying as it is used.
      /// </summary>
      public static string Reattach()
      {
         ServerSession session = ServerSession.Current;
         if (session == null)
            return null;

         Mouse.OverrideCursor = Cursors.Wait;
         try
         {
            if (session.Reconnect(20, TimeSpan.FromMilliseconds(500), out string error))
               return null;

            session.Invalidate();
            return error ?? "";
         }
         finally
         {
            Mouse.OverrideCursor = null;
         }
      }
   }
}
