// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.ServiceProcess;
using System.Threading.Tasks;
using System.Windows.Input;
using Microsoft.Win32.SafeHandles;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// Restarting the hMailServer service on this machine and attaching the session to
   /// it again: the flow the feature settings pages had to themselves after a save,
   /// shared with the window's pending bar, whose settings waiting for a restart had
   /// nothing to press (review finding F010), and with the directory synchronisation
   /// page's schedule. How long a stop and a start are waited for, and what is said
   /// when the service is left stopped, is <see cref="ServiceRestartFlow"/>'s.
   /// </summary>
   public static class ServiceRestart
   {
      private const string ServiceName = "hMailServer"; // no-loc: the service's name

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
         (ServiceRestartOutcome outcome, string reason) = await Task.Run(TryRestart);
         if (outcome != ServiceRestartOutcome.Restarted)
            return (outcome, reason);

         string lost = Reattach();
         return lost == null ? (ServiceRestartOutcome.Restarted, null) : (ServiceRestartOutcome.NotReattached, lost);
      }

      /// <summary>
      /// Stops and starts the service, waited for as the service asks
      /// (<see cref="ServiceRestartFlow"/>). Stopping a service needs administrator
      /// rights and the Control Panel runs asInvoker, so a non-elevated session hands
      /// the work to net.exe behind a single UAC prompt instead of failing with an
      /// opaque "Cannot open hMailServer service" error, and watches the service's
      /// state from here.
      /// </summary>
      public static (ServiceRestartOutcome Outcome, string Reason) TryRestart()
      {
         try
         {
            bool elevated;
            using (var identity = System.Security.Principal.WindowsIdentity.GetCurrent())
               elevated = new System.Security.Principal.WindowsPrincipal(identity)
                  .IsInRole(System.Security.Principal.WindowsBuiltInRole.Administrator);

            var service = new WindowsService();
            var clock = new SystemWaitClock();
            return elevated
               ? ServiceRestartFlow.Run(service, clock)
               : ServiceRestartFlow.RunThroughHelper(service, clock, RunNetElevated_);
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            return (ServiceRestartOutcome.NotRestarted, ex.GetBaseException().Message);
         }
      }

      /// <summary>
      /// net.exe stop and start, or start alone, behind one UAC prompt; false when the
      /// prompt was cancelled. "net stop" fails harmlessly when the service is already
      /// stopped, so the two are chained with "&amp;" rather than "&amp;&amp;".
      ///
      /// cmd.exe and net.exe by their full System32 paths, and System32 as the working
      /// directory: ShellExecuteEx looks for a bare name in the current directory first,
      /// and that is whatever folder a file dialog last left it in, so a cmd.exe waiting
      /// in Downloads would have run elevated behind the prompt the administrator
      /// expects. /d skips any Command Processor AutoRun; with /s, cmd takes off only
      /// the outer pair of quotes.
      /// </summary>
      private static bool RunNetElevated_(bool startOnly)
      {
         string system = Environment.SystemDirectory;
         string net = System.IO.Path.Combine(system, "net.exe"); // no-loc
         string command = startOnly
            ? "\"" + net + "\" start " + ServiceName // no-loc
            : "\"" + net + "\" stop " + ServiceName + " & \"" + net + "\" start " + ServiceName; // no-loc
         var startInfo = new System.Diagnostics.ProcessStartInfo
         {
            FileName = System.IO.Path.Combine(system, "cmd.exe"), // no-loc
            Arguments = "/d /s /c \"" + command + "\"", // no-loc
            WorkingDirectory = system,
            UseShellExecute = true,
            Verb = "runas", // no-loc
            WindowStyle = System.Diagnostics.ProcessWindowStyle.Hidden
         };

         try
         {
            using (var process = System.Diagnostics.Process.Start(startInfo))
               process?.WaitForExit();
            return true;
         }
         catch (Win32Exception ex) when (ex.NativeErrorCode == 1223)
         {
            return false;
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

      /// <summary>
      /// The hMailServer service through Windows' service controller: its status read
      /// with QueryServiceStatusEx and only the right to query it - which a Control
      /// Panel that is not elevated has, so that it can watch the stop and the start its
      /// helper asked for - a stop and a start asked through ServiceController, which
      /// needs the rights an elevated Control Panel has, and its process opened to wait
      /// for its end, with the right to wait alone (SYNCHRONIZE), as the installer opens it.
      /// </summary>
      private sealed class WindowsService : IServiceControl
      {
         private const uint ScManagerConnect = 0x0001;
         private const uint ServiceQueryStatus = 0x0004;
         private const int ScStatusProcessInfo = 0;
         private const uint Synchronize = 0x00100000;
         private const uint WaitTimeout = 0x00000102;

         public ServiceReading Read()
         {
            IntPtr manager = OpenSCManagerW(null, null, ScManagerConnect);
            if (manager == IntPtr.Zero)
               throw new Win32Exception(Marshal.GetLastWin32Error());
            try
            {
               IntPtr service = OpenServiceW(manager, ServiceName, ServiceQueryStatus);
               if (service == IntPtr.Zero)
                  throw new Win32Exception(Marshal.GetLastWin32Error());
               try
               {
                  var status = new ServiceStatusProcess();
                  if (!QueryServiceStatusEx(service, ScStatusProcessInfo, ref status, Marshal.SizeOf<ServiceStatusProcess>(), out _))
                     throw new Win32Exception(Marshal.GetLastWin32Error());

                  return new ServiceReading((ServiceState)status.CurrentState, (int)status.CheckPoint, (int)Math.Min(status.WaitHint, int.MaxValue), (int)status.ProcessId);
               }
               finally
               {
                  CloseServiceHandle(service);
               }
            }
            finally
            {
               CloseServiceHandle(manager);
            }
         }

         /// <summary>
         /// ServiceController.Stop on a worker: ControlService returns once the service's
         /// handler has, which for this server is the whole of its stop - or, past the 30
         /// seconds the controller gives a handler, with error 1053 as its process ends -
         /// so the flow watches the service meanwhile. The request's error is observed here,
         /// for the request the flow has stopped reading once it has done.
         /// </summary>
         public Task AskStop()
         {
            Task request = Task.Run(() =>
            {
               using var controller = new ServiceController(ServiceName);
               controller.Stop();
            });
            request.ContinueWith(ended => _ = ended.Exception, System.Threading.CancellationToken.None,
               TaskContinuationOptions.OnlyOnFaulted | TaskContinuationOptions.ExecuteSynchronously, TaskScheduler.Default);
            return request;
         }

         public void Start()
         {
            using var controller = new ServiceController(ServiceName);
            controller.Start();
         }

         public IProcessExit WatchProcess(int processId)
         {
            SafeProcessHandle handle = OpenProcess(Synchronize, false, processId);
            if (handle.IsInvalid)
            {
               handle.Dispose();
               return null;
            }

            return new ProcessExit(handle);
         }

         /// <summary>A process opened to wait for: ended when its handle is signalled, or when it can no longer be asked.</summary>
         private sealed class ProcessExit : IProcessExit
         {
            private readonly SafeProcessHandle handle_;

            public ProcessExit(SafeProcessHandle handle) => handle_ = handle;

            public bool HasExited => WaitForSingleObject(handle_, 0) != WaitTimeout;

            public void Dispose() => handle_.Dispose();
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

         [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
         [DefaultDllImportSearchPaths(DllImportSearchPath.System32)]
         private static extern IntPtr OpenSCManagerW(string machineName, string databaseName, uint desiredAccess);

         [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
         [DefaultDllImportSearchPaths(DllImportSearchPath.System32)]
         private static extern IntPtr OpenServiceW(IntPtr manager, string serviceName, uint desiredAccess);

         [DllImport("advapi32.dll", SetLastError = true)]
         [DefaultDllImportSearchPaths(DllImportSearchPath.System32)]
         [return: MarshalAs(UnmanagedType.Bool)]
         private static extern bool QueryServiceStatusEx(IntPtr service, int infoLevel, ref ServiceStatusProcess buffer, int bufferSize, out int bytesNeeded);

         [DllImport("advapi32.dll", SetLastError = true)]
         [DefaultDllImportSearchPaths(DllImportSearchPath.System32)]
         [return: MarshalAs(UnmanagedType.Bool)]
         private static extern bool CloseServiceHandle(IntPtr handle);

         [DllImport("kernel32.dll", SetLastError = true)]
         [DefaultDllImportSearchPaths(DllImportSearchPath.System32)]
         private static extern SafeProcessHandle OpenProcess(uint desiredAccess, [MarshalAs(UnmanagedType.Bool)] bool inheritHandle, int processId);

         [DllImport("kernel32.dll", SetLastError = true)]
         [DefaultDllImportSearchPaths(DllImportSearchPath.System32)]
         private static extern uint WaitForSingleObject(SafeProcessHandle handle, uint milliseconds);
      }
   }
}
