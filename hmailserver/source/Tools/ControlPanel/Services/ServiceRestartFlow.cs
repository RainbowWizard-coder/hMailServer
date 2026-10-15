// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Diagnostics;
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
   /// The service a restart drives: its state read, a stop and a start asked. Windows'
   /// service controller in the program (ServiceRestart), a fake in the tests, which is
   /// why this file has no WPF, no COM and no System.ServiceProcess in it.
   /// </summary>
   public interface IServiceControl
   {
      ServiceReading Read();

      void Stop();

      void Start();
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
   /// the service is started when it has stopped; and when it cannot be, or has not
   /// finished stopping, the outcome is <see cref="ServiceRestartOutcome.Stopped"/>,
   /// whose sentence says that the service is not running and how to start it. Held by
   /// ServiceRestartFlowTests.
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
      /// process may do itself (an elevated Control Panel): the stop waited for as the
      /// service asks, then the start, then the start waited for. Null as the reason
      /// when the service was restarted.
      /// </summary>
      public static (ServiceRestartOutcome Outcome, string Reason) Run(IServiceControl service, IWaitClock clock)
      {
         ServiceReading before;
         try
         {
            before = service.Read();
            if (before.State != ServiceState.Stopped && before.State != ServiceState.StopPending)
               service.Stop();
         }
         catch (Exception refused) when (!ExceptionPolicy.IsFatal(refused))
         {
            // Nothing was stopped: the service runs as it did.
            return (ServiceRestartOutcome.NotRestarted, MessageOf_(refused));
         }

         Waited stop = WhilePending(service, ServiceState.StopPending, clock);
         if (stop.Reading.State == ServiceState.StopPending)
            return (ServiceRestartOutcome.Stopped, StillStopping(stop));
         if (stop.Reading.State != ServiceState.Stopped)
            return (ServiceRestartOutcome.NotRestarted, DidNotStop(stop.Reading.State));

         return StartStopped_(service, clock, () =>
         {
            service.Start();
            return true;
         });
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
