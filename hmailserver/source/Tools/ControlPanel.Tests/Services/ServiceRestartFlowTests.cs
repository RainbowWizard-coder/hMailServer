// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// "Restart the service" waits for the stop and the start as the service asks, and
   /// says plainly when it leaves the service stopped (release review finding CP-1). The
   /// restart gave a stop 60 seconds, while the server's budget for one is
   /// ShutdownDrainSeconds plus 90 - the wait hint its ServiceStop reports, with the
   /// check point moved about once a second while it drains - so a drain of 75 seconds
   /// with one client connected ended the restart in "the operation has not been
   /// completed within the specified time interval", and the service, stopped a moment
   /// later, stayed stopped with nothing to say so. A fake service plays the server's
   /// stop on a fake clock; no service controller is reached.
   /// </summary>
   public class ServiceRestartFlowTests
   {
      [Fact]
      public void AStopThatDrainsForSeventyFiveSeconds_IsWaitedFor_AndTheServiceIsStartedOnceItHasStopped()
      {
         var clock = new FakeClock();
         var service = new FakeService(clock) { StopTakes = TimeSpan.FromSeconds(75.3), WaitHintMilliseconds = (75 + 90) * 1000 };

         (ServiceRestartOutcome outcome, string reason) = ServiceRestartFlow.Run(service, clock);

         Assert.Equal(ServiceRestartOutcome.Restarted, outcome);
         Assert.Null(reason);
         Assert.Equal(ServiceState.Running, service.State);
         Assert.Equal(new[] { "stop", "start" }, service.Asked);
         // Started once it had stopped, not while it was stopping, and not much later.
         Assert.InRange(service.StartedAt.TotalSeconds, 75.3, 77);
      }

      /// <summary>
      /// A Control Panel that is not elevated hands the stop and the start to net.exe
      /// behind one prompt and watches the service. A net stop that gave up waiting
      /// leaves net start refused while the service still stops, and the service then
      /// stopped: it is started once it has stopped, behind a second prompt.
      /// </summary>
      [Fact]
      public void ThroughTheHelper_AStopNetGaveUpOn_IsWaitedFor_AndTheServiceIsStartedBehindASecondPrompt()
      {
         var clock = new FakeClock();
         var service = new FakeService(clock) { StopTakes = TimeSpan.FromSeconds(75.3), WaitHintMilliseconds = (75 + 90) * 1000 };
         var prompts = new List<bool>();

         (ServiceRestartOutcome outcome, string reason) = ServiceRestartFlow.RunThroughHelper(service, clock, startOnly =>
         {
            prompts.Add(startOnly);
            return NetGivingUpAfterThirtySeconds_(service, clock, startOnly);
         });

         Assert.Equal(ServiceRestartOutcome.Restarted, outcome);
         Assert.Null(reason);
         Assert.Equal(new[] { false, true }, prompts);
         Assert.Equal(ServiceState.Running, service.State);
         Assert.InRange(service.StartedAt.TotalSeconds, 75.3, 77);
      }

      [Fact]
      public void ThroughTheHelper_ASecondPromptCancelled_LeavesTheServiceStopped_AndSaysSo()
      {
         var clock = new FakeClock();
         var service = new FakeService(clock) { StopTakes = TimeSpan.FromSeconds(75.3), WaitHintMilliseconds = (75 + 90) * 1000 };

         (ServiceRestartOutcome outcome, string reason) = ServiceRestartFlow.RunThroughHelper(service, clock,
            startOnly => !startOnly && NetGivingUpAfterThirtySeconds_(service, clock, startOnly));

         Assert.Equal(ServiceRestartOutcome.Stopped, outcome);
         Assert.Equal(ServiceState.Stopped, service.State);
         SaysTheServiceIsNotRunning_(reason);
         Assert.Contains("cancelled", reason, StringComparison.Ordinal);
      }

      /// <summary>
      /// A net stop that waited for the stop, as net.exe does while the check point
      /// moves, and a net start that followed: restarted, behind the one prompt.
      /// </summary>
      [Fact]
      public void ThroughTheHelper_AStopAndStartTheHelperFinished_IsARestart()
      {
         var clock = new FakeClock();
         var service = new FakeService(clock) { StopTakes = TimeSpan.FromSeconds(75.3), WaitHintMilliseconds = (75 + 90) * 1000 };
         var prompts = new List<bool>();

         (ServiceRestartOutcome outcome, _) = ServiceRestartFlow.RunThroughHelper(service, clock, startOnly =>
         {
            prompts.Add(startOnly);
            service.Stop();
            while (service.Read().State != ServiceState.Stopped)
               clock.Sleep(TimeSpan.FromSeconds(1));
            service.Start();
            return true;
         });

         Assert.Equal(ServiceRestartOutcome.Restarted, outcome);
         Assert.Equal(new[] { false }, prompts);
      }

      /// <summary>
      /// A stop whose check point stops moving: given the step's wait hint and no
      /// more, then said as a stop that has not finished - the service not running, and
      /// how to start it - and never started while it still stops.
      /// </summary>
      [Fact]
      public void AStopThatStopsMoving_EndsTheWait_AndSaysTheServiceIsNotRunning()
      {
         var clock = new FakeClock();
         var service = new FakeService(clock) { StopTakes = TimeSpan.MaxValue, StallAfter = TimeSpan.FromSeconds(10), WaitHintMilliseconds = 90 * 1000 };

         (ServiceRestartOutcome outcome, string reason) = ServiceRestartFlow.Run(service, clock);

         Assert.Equal(ServiceRestartOutcome.Stopped, outcome);
         SaysTheServiceIsNotRunning_(reason);
         Assert.Equal(new[] { "stop" }, service.Asked);
         Assert.InRange(clock.Elapsed.TotalSeconds, 10 + 90, 10 + 90 + ServiceRestartFlow.StepSlack.TotalSeconds + 1);
      }

      /// <summary>
      /// A stop whose check point moves for ever is still bounded: by the longest wait
      /// hint the service named, and a minute.
      /// </summary>
      [Fact]
      public void AStopThatMovesForEver_IsWaitedForNoLongerThanItsLongestHintAndAMinute()
      {
         var clock = new FakeClock();
         var service = new FakeService(clock) { StopTakes = TimeSpan.MaxValue, WaitHintMilliseconds = 90 * 1000 };

         ServiceRestartFlow.Waited waited = ServiceRestartFlow.WhilePending(StopNow_(service), ServiceState.StopPending, clock);

         Assert.True(waited.RanOut);
         Assert.Equal(ServiceState.StopPending, waited.Reading.State);
         Assert.InRange(waited.Took.TotalSeconds, 90 + 60, 90 + 60 + 1);
         Assert.Equal(TimeSpan.FromSeconds(90), waited.LongestHint);
      }

      [Fact]
      public void AStartRefusedAfterTheStop_LeavesTheServiceStopped_AndSaysSo()
      {
         var clock = new FakeClock();
         var service = new FakeService(clock) { StopTakes = TimeSpan.FromSeconds(3), WaitHintMilliseconds = 90 * 1000, StartRefusal = "The service did not start because of a logon failure." };

         (ServiceRestartOutcome outcome, string reason) = ServiceRestartFlow.Run(service, clock);

         Assert.Equal(ServiceRestartOutcome.Stopped, outcome);
         SaysTheServiceIsNotRunning_(reason);
         Assert.Contains("logon failure", reason, StringComparison.Ordinal);
      }

      [Fact]
      public void AServiceThatStopsAgainAsItStarts_IsStopped_AndSaidToBe()
      {
         var clock = new FakeClock();
         var service = new FakeService(clock) { StopTakes = TimeSpan.FromSeconds(3), WaitHintMilliseconds = 90 * 1000, StopsAsItStarts = true };

         (ServiceRestartOutcome outcome, string reason) = ServiceRestartFlow.Run(service, clock);

         Assert.Equal(ServiceRestartOutcome.Stopped, outcome);
         SaysTheServiceIsNotRunning_(reason);
      }

      /// <summary>A stop the controller refused leaves the service as it was, and is no more than "not restarted".</summary>
      [Fact]
      public void AStopThatIsRefused_LeavesTheServiceRunning_AndIsNotRestarted()
      {
         var clock = new FakeClock();
         var service = new FakeService(clock) { StopRefusal = "Access is denied." };

         (ServiceRestartOutcome outcome, string reason) = ServiceRestartFlow.Run(service, clock);

         Assert.Equal(ServiceRestartOutcome.NotRestarted, outcome);
         Assert.Equal("Access is denied.", reason);
         Assert.Equal(ServiceState.Running, service.State);
      }

      /// <summary>A service stopped before the restart is started, and that is a restart.</summary>
      [Fact]
      public void AStoppedService_IsStarted()
      {
         var clock = new FakeClock();
         var service = new FakeService(clock) { State = ServiceState.Stopped };

         (ServiceRestartOutcome outcome, _) = ServiceRestartFlow.Run(service, clock);

         Assert.Equal(ServiceRestartOutcome.Restarted, outcome);
         Assert.Equal(new[] { "start" }, service.Asked);
      }

      /// <summary>
      /// The helper's stop that net refused - the same process running as before - is
      /// not a restart, though the service runs.
      /// </summary>
      [Fact]
      public void ThroughTheHelper_TheSameProcessRunningAsBefore_IsNotARestart()
      {
         var clock = new FakeClock();
         var service = new FakeService(clock);

         (ServiceRestartOutcome outcome, string reason) = ServiceRestartFlow.RunThroughHelper(service, clock, startOnly => true);

         Assert.Equal(ServiceRestartOutcome.NotRestarted, outcome);
         Assert.Equal(ServiceRestartFlow.DidNotStop(ServiceState.Running), reason);
      }

      // The service is not running, and how to start it is said.
      private static void SaysTheServiceIsNotRunning_(string reason)
      {
         Assert.False(string.IsNullOrEmpty(reason));
         Assert.Contains("It is not running", reason, StringComparison.Ordinal);
         Assert.Contains("Services", reason, StringComparison.Ordinal);
      }

      // net stop that waits thirty seconds and gives up, then net start, refused while the
      // service still stops.
      private static bool NetGivingUpAfterThirtySeconds_(FakeService service, FakeClock clock, bool startOnly)
      {
         if (!startOnly)
         {
            service.Stop();
            clock.Sleep(TimeSpan.FromSeconds(30));
         }

         try
         {
            service.Start();
         }
         catch (InvalidOperationException)
         {
            // "The service is starting or stopping. Please try again later."
         }

         return true;
      }

      private static FakeService StopNow_(FakeService service)
      {
         service.Stop();
         return service;
      }

      private sealed class FakeClock : IWaitClock
      {
         public TimeSpan Elapsed { get; private set; }

         public void Sleep(TimeSpan span) => Elapsed += span;
      }

      /// <summary>
      /// A service as the server's ServiceStop drives one: a stop that takes
      /// <see cref="StopTakes"/>, reporting <see cref="WaitHintMilliseconds"/> and moving
      /// its check point once a second (until <see cref="StallAfter"/>, where set), and a
      /// start that takes two seconds in a new process.
      /// </summary>
      private sealed class FakeService : IServiceControl
      {
         private readonly FakeClock clock_;
         private TimeSpan since_;
         private int process_ = 100;

         public FakeService(FakeClock clock) => clock_ = clock;

         public ServiceState State = ServiceState.Running;
         public TimeSpan StopTakes = TimeSpan.FromSeconds(1);
         public TimeSpan? StallAfter;
         public int WaitHintMilliseconds = 90 * 1000;
         public string StopRefusal;
         public string StartRefusal;
         public bool StopsAsItStarts;
         public readonly List<string> Asked = new();
         public TimeSpan StartedAt;

         public ServiceReading Read()
         {
            Advance_();
            int checkPoint = 0;
            if (State == ServiceState.StopPending || State == ServiceState.StartPending)
            {
               TimeSpan pending = clock_.Elapsed - since_;
               if (StallAfter is TimeSpan stall && pending > stall)
                  pending = stall;
               checkPoint = 1 + (int)pending.TotalSeconds;
            }

            int hint = State == ServiceState.StopPending ? WaitHintMilliseconds : State == ServiceState.StartPending ? 30000 : 0;
            return new ServiceReading(State, checkPoint, hint, State == ServiceState.Stopped ? 0 : process_);
         }

         public void Stop()
         {
            Advance_();
            Asked.Add("stop");
            if (StopRefusal != null)
               throw new InvalidOperationException(StopRefusal);
            if (State != ServiceState.Running && State != ServiceState.Paused)
               throw new InvalidOperationException("The service cannot accept control messages at this time.");
            State = ServiceState.StopPending;
            since_ = clock_.Elapsed;
         }

         public void Start()
         {
            Advance_();
            if (State != ServiceState.Stopped)
               throw new InvalidOperationException("The service is starting or stopping. Please try again later.");
            Asked.Add("start");
            if (StartRefusal != null)
               throw new InvalidOperationException(StartRefusal);
            State = ServiceState.StartPending;
            since_ = clock_.Elapsed;
            StartedAt = clock_.Elapsed;
         }

         private void Advance_()
         {
            TimeSpan pending = clock_.Elapsed - since_;
            if (State == ServiceState.StopPending && StopTakes != TimeSpan.MaxValue && pending >= StopTakes)
               State = ServiceState.Stopped;
            else if (State == ServiceState.StartPending && pending >= TimeSpan.FromSeconds(2))
            {
               State = StopsAsItStarts ? ServiceState.Stopped : ServiceState.Running;
               process_++;
            }
         }
      }
   }
}
