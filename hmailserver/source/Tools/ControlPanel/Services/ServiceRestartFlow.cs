// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Diagnostics;
using System.Threading.Tasks;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>What a restart of the service came to.</summary>
   public enum ServiceRestartOutcome
   {
      /// <summary>The service stopped, started and the session is attached to it again.</summary>
      Restarted,

      /// <summary>The service could not be stopped and started, and is as it was; the reason says why.</summary>
      NotRestarted,

      /// <summary>The service came back, and the session could not attach to it; the reason says why.</summary>
      NotReattached,

      /// <summary>
      /// The stop was made, or is still under way, and the service is not running: it
      /// could not be started again, or it has not finished stopping. The reason says so
      /// plainly, and how to start it - nothing starts it by itself.
      /// </summary>
      Stopped
   }

   /// <summary>A service's state as the service controller reports it: the Win32 SERVICE_* values.</summary>
   public enum ServiceState
   {
      Unknown = 0,
      Stopped = 1,
      StartPending = 2,
      StopPending = 3,
      Running = 4,
      ContinuePending = 5,
      PausePending = 6,
      Paused = 7
   }

   /// <summary>
   /// What the service controller says of the service at one moment
   /// (QueryServiceStatusEx): its state, the check point a pending service moves on as
   /// it makes progress, the wait hint it gives for its next step, and its process.
   /// </summary>
   public readonly record struct ServiceReading(ServiceState State, int CheckPoint, int WaitHintMilliseconds, int ProcessId);

   /// <summary>
   /// The service a restart drives: its state read, a stop and a start asked, and the
   /// process it ran in watched for its end. Windows' service controller in the program
   /// (ServiceRestart), a fake in the tests, which is why this file has no WPF, no COM and
   /// no System.ServiceProcess in it.
   /// </summary>
   public interface IServiceControl
   {
      ServiceReading Read();

      /// <summary>
      /// Asks the service to stop and returns at once with the request, which ends when
      /// Windows' service controller answers it. The controller answers a stop once the
      /// service's control handler has returned, and the server's handler runs the whole
      /// of its stop (hMailServer.cpp, ServiceController and TerminateServer): past the 30
      /// seconds the controller gives a handler, it says the service is stopped while the
      /// process still drains, and answers the request with ERROR_SERVICE_REQUEST_TIMEOUT
      /// (1053) as the process ends. A stop refused - no right to stop the service, or a
      /// service that takes no stop then - ends the request at once, with the service as
      /// it was. The answer is read while the restart waits, or never, once it has done.
      /// </summary>
      Task AskStop();

      void Start();

      /// <summary>
      /// The process <paramref name="processId"/>, opened so that its end can be waited
      /// for, or null where it cannot be opened. Opened before the stop is asked, as the
      /// installer's StopHMailServerAndWaitForExit opens it: once the service has stopped,
      /// the controller names its process no more, and the id could be another's.
      /// </summary>
      IProcessExit WatchProcess(int processId);
   }

   /// <summary>A process a restart waits to end: the one the service ran in before its stop.</summary>
   public interface IProcessExit : IDisposable
   {
      /// <summary>Whether the process has ended - or can no longer be asked, which leaves the stop's answer to wait for.</summary>
      bool HasExited { get; }
   }

   /// <summary>The time a wait is measured in and slept through: the computer's in the program, a test's own in the tests.</summary>
   public interface IWaitClock
   {
      TimeSpan Elapsed { get; }

      void Sleep(TimeSpan span);
   }

   /// <summary>The computer's clock: a stopwatch, and the thread put to sleep.</summary>
   public sealed class SystemWaitClock : IWaitClock
   {
      private readonly Stopwatch watch_ = Stopwatch.StartNew();

      public TimeSpan Elapsed => watch_.Elapsed;

      public void Sleep(TimeSpan span) => System.Threading.Thread.Sleep(span);
   }

   /// <summary>
   /// Restarting the hMailServer service as the service itself asks to be waited for
   /// (release review finding CP-1). The restart waited a fixed 60 seconds for the
   /// stop, while the server's own budget for a stop is ShutdownDrainSeconds plus 90 -
   /// the wait hint its ServiceStop reports (hMailServer.cpp), and the installer's
   /// GetServiceStopBudgetMs - with its check point moved about once a second for as
   /// long as the drain and the message indexer's minute last. A drain of a minute or
   /// more with one client connected ran past the 60, the restart said only that the
   /// operation had not completed in time, and the service, stopped a moment later, was
   /// left stopped: nothing started it, and nothing said so.
   ///
   /// Now the wait is the service's own: as long as the check point moves, each step
   /// within the wait hint the service names, and the whole within the longest hint it
   /// named and a minute more. So the budget is read in one place - the server's - and
   /// a change to it reaches this wait without a change here. Once a stop has been made,
   /// the service is started when it has stopped and the process it ran in has ended;
   /// and when it cannot be, or has not finished stopping, the outcome is
   /// <see cref="ServiceRestartOutcome.Stopped"/>, whose sentence says that the service
   /// is not running and how to start it.
   ///
   /// Windows gives a service's control handler 30 seconds, and the server runs its
   /// whole stop inside its handler. A stop longer than that - a drain of 30 seconds or
   /// more with one client connected, or the message indexer's minute - is one the
   /// service controller gives up waiting for at the 30th second (System event 7011): it
   /// says Stopped from then on while the process still drains, and answers the stop it
   /// was asked with ERROR_SERVICE_REQUEST_TIMEOUT (1053) once the process has ended. The
   /// restart asked the stop and waited for that answer, and took any error of it for a
   /// stop refused - "the service runs as it did" - so that an elevated Control Panel, the
   /// usual one on a server, ended such a restart in a bare 1053 with the service stopped,
   /// as before this flow (the adversarial check of release-fix unit rf5-rf6-cp, on
   /// server1's bench, with a drain of 45 seconds). So the stop is asked on a worker, and
   /// the service watched meanwhile: a stop is refused only while the service is as it was,
   /// in the same process, when its request ends; and the start waits for the process the
   /// service ran in to end, within what the service asked for its stop, since the
   /// controller's Stopped does not say it has. Held by ServiceRestartFlowTests.
   /// </summary>
   public static class ServiceRestartFlow
   {
      /// <summary>The least a step is given where the service names no wait hint.</summary>
      public static readonly TimeSpan HintFloor = TimeSpan.FromSeconds(10);

      /// <summary>What a step may take beyond its wait hint before the service is taken to have stopped moving.</summary>
      public static readonly TimeSpan StepSlack = TimeSpan.FromSeconds(5);

      /// <summary>What a whole wait may take beyond the longest wait hint the service named.</summary>
      public static readonly TimeSpan WholeSlack = TimeSpan.FromMinutes(1);

      /// <summary>How often a pending service is read: a cheap call, and a wait that ends close to when the service does.</summary>
      public static readonly TimeSpan PollInterval = TimeSpan.FromMilliseconds(500);

      /// <summary>
      /// What Windows' service controller gives a service's control handler before it
      /// gives up waiting for it (the Handler function's 30 seconds): the most a stop the
      /// service has not yet begun to report is waited for.
      /// </summary>
      public static readonly TimeSpan HandlerTimeout = TimeSpan.FromSeconds(30);

      /// <summary>How a wait while the service was pending ended: the last reading, how long it took and whether it ran out.</summary>
      public readonly record struct Waited(ServiceReading Reading, TimeSpan Took, TimeSpan LongestHint, bool RanOut);

      /// <summary>
      /// Waits while the service is <paramref name="pending"/>, as the service asks: as
      /// long as its check point moves, each step within the wait hint it names (and
      /// <see cref="StepSlack"/>), and the whole within the longest hint it named and
      /// <see cref="WholeSlack"/>. The Windows rule for a service control program, and
      /// the server's: its stop names ShutdownDrainSeconds + 90 seconds and moves the
      /// check point while it drains and waits.
      /// </summary>
      public static Waited WhilePending(IServiceControl service, ServiceState pending, IWaitClock clock)
      {
         TimeSpan started = clock.Elapsed;
         ServiceReading reading = service.Read();
         int checkPoint = reading.CheckPoint;
         TimeSpan progressed = started;
         TimeSpan longest = Hint_(reading);

         while (reading.State == pending)
         {
            TimeSpan hint = Hint_(reading);
            if (hint > longest)
               longest = hint;

            TimeSpan now = clock.Elapsed;
            if (now - progressed > hint + StepSlack || now - started > longest + WholeSlack)
               return new Waited(reading, now - started, longest, true);

            clock.Sleep(PollInterval);
            reading = service.Read();
            if (reading.CheckPoint != checkPoint)
            {
               checkPoint = reading.CheckPoint;
               progressed = clock.Elapsed;
            }
         }

         return new Waited(reading, clock.Elapsed - started, longest, false);
      }

      /// <summary>
      /// Stops and starts the service through the service controller, which this
      /// process may do itself (an elevated Control Panel): the stop asked and the service
      /// watched while it stops, as the service asks to be waited for, then the process it
      /// ran in waited for, then the start, then the start waited for. Null as the reason
      /// when the service was restarted.
      /// </summary>
      public static (ServiceRestartOutcome Outcome, string Reason) Run(IServiceControl service, IWaitClock clock)
      {
         ServiceReading before;
         try
         {
            before = service.Read();
         }
         catch (Exception refused) when (!ExceptionPolicy.IsFatal(refused))
         {
            return (ServiceRestartOutcome.NotRestarted, MessageOf_(refused));
         }

         if (before.State == ServiceState.Stopped)
            return StartStopped_(service, clock, StartNow_(service));

         // Opened before the stop is asked, while the controller still names the process.
         using IProcessExit process = before.ProcessId != 0 ? service.WatchProcess(before.ProcessId) : null;
         TimeSpan asked = clock.Elapsed;
         Task request = null;
         if (before.State != ServiceState.StopPending)
         {
            try
            {
               request = service.AskStop();
            }
            catch (Exception refused) when (!ExceptionPolicy.IsFatal(refused))
            {
               // Nothing was asked of the service: it runs as it did.
               return (ServiceRestartOutcome.NotRestarted, MessageOf_(refused));
            }

            string refusal = UntilTheStopBegins_(service, before, request, clock);
            if (refusal != null)
               return (ServiceRestartOutcome.NotRestarted, refusal);
         }

         Waited stop = WhilePending(service, ServiceState.StopPending, clock);
         if (stop.Reading.State == ServiceState.StopPending)
            return (ServiceRestartOutcome.Stopped, StillStopping(stop));
         if (stop.Reading.State == ServiceState.Running && before.ProcessId != 0 && stop.Reading.ProcessId != 0 && stop.Reading.ProcessId != before.ProcessId)
            return (ServiceRestartOutcome.Restarted, null); // started again meanwhile, by another hand, in a new process
         if (stop.Reading.State != ServiceState.Stopped)
            return (ServiceRestartOutcome.NotRestarted, DidNotStop(stop.Reading.State));

         Waited ended = UntilTheProcessEnds_(process, request, clock, asked, stop.LongestHint);
         if (ended.RanOut)
            return (ServiceRestartOutcome.Stopped, StillStopping(ended));

         return StartStopped_(service, clock, StartNow_(service));
      }

      private static Func<bool> StartNow_(IServiceControl service)
         => () =>
         {
            service.Start();
            return true;
         };

      /// <summary>
      /// Watches a stop asked of the service until it has begun - the service has left the
      /// state it was in, or the process it ran in - and answers null then. A request that
      /// ends with the service as it was, in the same process, is a stop the service
      /// refused, and its reason is answered: the error it ended with, or that the service
      /// did not stop. So is a stop that has not begun within the 30 seconds the controller
      /// gives a handler (<see cref="HandlerTimeout"/>) and <see cref="StepSlack"/>, as the
      /// server reports its stop pending first thing (TerminateServer). An error that ends
      /// the request after the stop has begun - 1053, the controller's 30 seconds past -
      /// is not a refusal: the stop was made, and is watched to its end.
      /// </summary>
      private static string UntilTheStopBegins_(IServiceControl service, ServiceReading before, Task request, IWaitClock clock)
      {
         TimeSpan started = clock.Elapsed;
         while (true)
         {
            // Whether the request had ended is read before the state, so that a refusal is
            // one whose service was as it was after the request ended.
            bool ended = request.IsCompleted;
            ServiceReading reading = service.Read();
            if (reading.State != before.State || reading.ProcessId != before.ProcessId)
               return null;

            if (ended)
               return request.IsFaulted && request.Exception != null ? MessageOf_(request.Exception) : DidNotStop(reading.State);

            if (clock.Elapsed - started > HandlerTimeout + StepSlack)
               return DidNotStop(reading.State);

            clock.Sleep(PollInterval);
         }
      }

      /// <summary>
      /// Waits, once the controller says the service is stopped, for the process the
      /// service ran in to end - which from the controller's 30 seconds on it has not, while
      /// the stop drains on - and for the controller's answer to the stop, which comes as the
      /// process ends, though no longer than <see cref="StepSlack"/> after it. All within the
      /// longest wait hint the service named for its stop and <see cref="WholeSlack"/>,
      /// counted from the stop's request; a wait that runs out is a service that has not
      /// finished stopping. A process that could not be opened leaves the answer alone to
      /// wait for; with neither - a stop someone else asked, of a process that cannot be
      /// opened - the controller's Stopped is all there is.
      /// </summary>
      private static Waited UntilTheProcessEnds_(IProcessExit process, Task request, IWaitClock clock, TimeSpan asked, TimeSpan longestHint)
      {
         TimeSpan? exitedAt = null;
         while (true)
         {
            TimeSpan now = clock.Elapsed;
            bool answered = request == null || request.IsCompleted;
            if (process == null)
            {
               if (answered)
                  return new Waited(default, now - asked, longestHint, false);
            }
            else
            {
               if (exitedAt == null && process.HasExited)
                  exitedAt = now;
               if (exitedAt is TimeSpan exited && (answered || now - exited > StepSlack))
                  return new Waited(default, now - asked, longestHint, false);
            }

            if (now - asked > longestHint + WholeSlack)
               return new Waited(default, now - asked, longestHint, true);

            clock.Sleep(PollInterval);
         }
      }

      /// <summary>
      /// The same through a helper this process starts elevated (a Control Panel the
      /// administrator did not run as administrator: net.exe behind one UAC prompt),
      /// which stops and starts the service and is then watched here, where only the
      /// service's state can be read. <paramref name="runElevated"/> runs the helper
      /// that stops and starts the service (startOnly false) or starts it alone (true),
      /// waits for it, and answers false when its elevation prompt was cancelled. A stop
      /// the helper gave up waiting for leaves the service stopped once it ends - its
      /// start was refused while the service was still stopping - and is started then,
      /// behind a second prompt: the administrator asked for the service to run again.
      /// </summary>
      public static (ServiceRestartOutcome Outcome, string Reason) RunThroughHelper(IServiceControl service, IWaitClock clock, Func<bool, bool> runElevated)
      {
         ServiceReading before;
         try
         {
            before = service.Read();
            if (!runElevated(false))
               return (ServiceRestartOutcome.NotRestarted, L("the elevation prompt was cancelled."));
         }
         catch (Exception refused) when (!ExceptionPolicy.IsFatal(refused))
         {
            return (ServiceRestartOutcome.NotRestarted, MessageOf_(refused));
         }

         Waited stop = WhilePending(service, ServiceState.StopPending, clock);
         if (stop.Reading.State == ServiceState.StopPending)
            return (ServiceRestartOutcome.Stopped, StillStopping(stop));

         Waited start = WhilePending(service, ServiceState.StartPending, clock);
         switch (start.Reading.State)
         {
            case ServiceState.Running:
               // The same process, running as before: nothing was stopped, so nothing was
               // restarted - net stop refused the stop, or gave up on it before it began.
               bool wasRunning = before.State != ServiceState.Stopped && before.State != ServiceState.Unknown;
               if (wasRunning && before.ProcessId != 0 && start.Reading.ProcessId == before.ProcessId)
                  return (ServiceRestartOutcome.NotRestarted, DidNotStop(ServiceState.Running));
               return (ServiceRestartOutcome.Restarted, null);

            case ServiceState.StartPending:
               return (ServiceRestartOutcome.NotRestarted, StillStarting(start));

            case ServiceState.Stopped:
               return StartStopped_(service, clock, () => runElevated(true));

            default:
               return (ServiceRestartOutcome.NotRestarted, DidNotStop(start.Reading.State));
         }
      }

      // The service is stopped, by this restart: started, and the start waited for. A
      // start that fails, or a service that stops again as it starts, leaves it stopped,
      // which the outcome says.
      private static (ServiceRestartOutcome Outcome, string Reason) StartStopped_(IServiceControl service, IWaitClock clock, Func<bool> start)
      {
         try
         {
            if (!start())
               return (ServiceRestartOutcome.Stopped, StoppedAndNotStarted(L("the elevation prompt to start it was cancelled.")));
         }
         catch (Exception refused) when (!ExceptionPolicy.IsFatal(refused))
         {
            return (ServiceRestartOutcome.Stopped, StoppedAndNotStarted(MessageOf_(refused)));
         }

         Waited started = WhilePending(service, ServiceState.StartPending, clock);
         switch (started.Reading.State)
         {
            case ServiceState.Running:
               return (ServiceRestartOutcome.Restarted, null);
            case ServiceState.StartPending:
               return (ServiceRestartOutcome.NotRestarted, StillStarting(started));
            default:
               return (ServiceRestartOutcome.Stopped,
                  StoppedAndNotStarted(L("it stopped again as it started; the hMailServer error log and the Windows System log say why.")));
         }
      }

      /// <summary>The stop was made and the start was not: the service is not running, and nothing starts it by itself.</summary>
      public static string StoppedAndNotStarted(string why)
         => F("The hMailServer service stopped and was not started again: {0} It is not running, so no mail is received or delivered until it is started - with Restart the service… where it is offered, or in Services.", why);

      /// <summary>The stop has not finished in the time the service asked for: it is not running, and will not start by itself once it has stopped.</summary>
      public static string StillStopping(Waited stop)
         => F("The hMailServer service has not finished stopping after {0} seconds, though it asked for {1}. It is not running, and it will not start by itself once it has stopped: start it then, with Restart the service… where it is offered, or in Services.",
            WholeSeconds_(stop.Took), WholeSeconds_(stop.LongestHint));

      /// <summary>A start that has not finished in the time the service asked for.</summary>
      public static string StillStarting(Waited start)
         => F("the service was started again and has not said it is running after {0} seconds; Services shows whether it does.", WholeSeconds_(start.Took));

      /// <summary>A stop the service did not make: it is as it was. Each state its own sentence, which a translation can put in order.</summary>
      public static string DidNotStop(ServiceState state)
      {
         switch (state)
         {
            case ServiceState.Running:
               return L("the service did not stop, and runs as it did.");
            case ServiceState.Paused:
               return L("the service did not stop, and is paused as it was.");
            case ServiceState.PausePending:
               return L("the service did not stop, and is pausing.");
            case ServiceState.ContinuePending:
               return L("the service did not stop, and is resuming.");
            case ServiceState.StartPending:
               return L("the service did not stop, and is starting.");
            default:
               return L("the service did not stop, and Windows reports it in a state it does not name.");
         }
      }

      private static TimeSpan Hint_(ServiceReading reading)
      {
         TimeSpan hint = TimeSpan.FromMilliseconds(Math.Max(0, reading.WaitHintMilliseconds));
         return hint < HintFloor ? HintFloor : hint;
      }

      private static string WholeSeconds_(TimeSpan span)
         => ((long)Math.Round(span.TotalSeconds)).ToString("N0", System.Globalization.CultureInfo.CurrentCulture);

      private static string MessageOf_(Exception refused)
         => refused.GetBaseException().Message;
   }
}
