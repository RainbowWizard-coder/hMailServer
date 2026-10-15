// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Threading.Tasks;
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
   /// later, stayed stopped with nothing to say so. And Windows gives a control handler 30
   /// seconds, while the server stops inside its handler: from the 30th second the service
   /// controller says Stopped while the process drains on, and answers the stop with error
   /// 1053 as the process ends, which the restart took for a stop refused (the adversarial
   /// check of rf5-rf6-cp, on server1's bench). A fake service plays the server's stop and
   /// the controller's 30 seconds on a fake clock; no service controller is reached.
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
      /// A drain of 45 seconds with one client connected, as on server1's bench: the
      /// controller gives up waiting for the handler at 30 seconds, says Stopped from then
      /// on while the process drains on, and answers the stop with error 1053 once the
      /// process has ended. That is a stop made, not one refused: the service is started
      /// once its process has ended, and is restarted - where the restart said only "The
      /// service did not respond to the start or control request in a timely fashion."
      /// and left the service stopped.
      /// </summary>
      [Fact]
      public void AStopPastTheControllersThirtySeconds_AnsweredWithATimeout_IsARestart_StartedOnceTheProcessHasEnded()
      {
         var clock = new FakeClock();
         var service = new FakeService(clock) { StopTakes = TimeSpan.FromSeconds(45.3), WaitHintMilliseconds = (45 + 90) * 1000, ControllerGivesUpAfter = TimeSpan.FromSeconds(30) };

         (ServiceRestartOutcome outcome, string reason) = ServiceRestartFlow.Run(service, clock);

         Assert.Equal(ServiceRestartOutcome.Restarted, outcome);
         Assert.Null(reason);
         Assert.Equal(ServiceState.Running, service.State);
         Assert.Equal(new[] { "stop", "start" }, service.Asked);
         Assert.Equal(1053, service.StopAnsweredWith);
         Assert.False(service.StartedWhileTheOldProcessRan);
         Assert.InRange(service.StartedAt.TotalSeconds, service.OldProcessEndedAt.TotalSeconds, service.OldProcessEndedAt.TotalSeconds + 2);
      }

      /// <summary>
      /// The controller's Stopped from its 30 seconds on is not the end of the process,
      /// which drains on to 46 seconds: the start waits for the process, though the
      /// controller has answered the stop already, so that a new process does not start
      /// beside the one still stopping.
      /// </summary>
      [Fact]
      public void TheControllerSaysStoppedWhileTheProcessDrainsOn_TheStartWaitsForTheProcess()
      {
         var clock = new FakeClock();
         var service = new FakeService(clock)
         {
            StopTakes = TimeSpan.FromSeconds(45),
            ProcessEndsAfterStop = TimeSpan.FromSeconds(1),
            WaitHintMilliseconds = (45 + 90) * 1000,
            ControllerGivesUpAfter = TimeSpan.FromSeconds(30),
            ControllerAnswersAtItsTimeout = true
         };

         (ServiceRestartOutcome outcome, _) = ServiceRestartFlow.Run(service, clock);

         Assert.Equal(ServiceRestartOutcome.Restarted, outcome);
         Assert.False(service.StartedWhileTheOldProcessRan);
         Assert.InRange(service.StartedAt.TotalSeconds, 46, 47);
      }

      /// <summary>
      /// A process that has not ended within what the service asked for its stop - the
      /// longest wait hint and a minute - is not started over: the service is said to be
      /// not running, and how to start it.
      /// </summary>
      [Fact]
      public void AProcessThatDoesNotEndWithinWhatTheServiceAskedFor_IsNotStartedOver_AndTheServiceIsSaidToBeStopped()
      {
         var clock = new FakeClock();
         var service = new FakeService(clock) { StopTakes = TimeSpan.MaxValue, WaitHintMilliseconds = 90 * 1000, ControllerGivesUpAfter = TimeSpan.FromSeconds(30) };

         (ServiceRestartOutcome outcome, string reason) = ServiceRestartFlow.Run(service, clock);

         Assert.Equal(ServiceRestartOutcome.Stopped, outcome);
         SaysTheServiceIsNotRunning_(reason);
         Assert.Equal(new[] { "stop" }, service.Asked);
         Assert.InRange(clock.Elapsed.TotalSeconds, 90 + 60, 90 + 60 + 1);
      }

      /// <summary>
      /// A process that cannot be opened to wait for leaves the controller's answer to the
      /// stop, which comes as the process ends: the start waits for that.
      /// </summary>
      [Fact]
      public void AProcessThatCannotBeOpened_IsWaitedForByTheStopsAnswer()
      {
         var clock = new FakeClock();
         var service = new FakeService(clock) { StopTakes = TimeSpan.FromSeconds(45.3), WaitHintMilliseconds = (45 + 90) * 1000, ControllerGivesUpAfter = TimeSpan.FromSeconds(30), ProcessCanBeOpened = false };

         (ServiceRestartOutcome outcome, _) = ServiceRestartFlow.Run(service, clock);

         Assert.Equal(ServiceRestartOutcome.Restarted, outcome);
         Assert.False(service.StartedWhileTheOldProcessRan);
         Assert.True(service.StartedAt >= service.StopAnsweredAt, "The service was started before the controller answered its stop.");
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
            _ = service.AskStop();
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

      /// <summary>A stop the controller refused at once leaves the service as it was, in the same process, and is no more than "not restarted".</summary>
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
            _ = service.AskStop();
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
         _ = service.AskStop();
         return service;
      }

      // Time that passes for the service too: the controller answers a stop, and a process
      // ends, as the clock moves, whether or not the restart reads the service meanwhile.
      private sealed class FakeClock : IWaitClock
      {
         public TimeSpan Elapsed { get; private set; }

         public event Action Slept;

         public void Sleep(TimeSpan span)
         {
            Elapsed += span;
            Slept?.Invoke();
         }
      }

      /// <summary>
      /// A service as the server's ServiceStop drives one, and Windows' controller answers
      /// for it: a stop that takes <see cref="StopTakes"/>, reporting
      /// <see cref="WaitHintMilliseconds"/> and moving its check point once a second (until
      /// <see cref="StallAfter"/>, where set), in a process that ends
      /// <see cref="ProcessEndsAfterStop"/> later; a controller that, where
      /// <see cref="ControllerGivesUpAfter"/> is set and the stop takes longer, says Stopped
      /// from then on while the process drains on, and answers the stop with error 1053 as
      /// the process ends (or then, with <see cref="ControllerAnswersAtItsTimeout"/>); and a
      /// start that takes two seconds in a new process.
      /// </summary>
      private sealed class FakeService : IServiceControl
      {
         private readonly FakeClock clock_;
         private TimeSpan since_;
         private int process_ = 100;
         private bool stopping_;
         private int stoppingProcess_;
         private TaskCompletionSource answer_;

         public FakeService(FakeClock clock)
         {
            clock_ = clock;
            clock.Slept += Advance_;
         }

         public ServiceState State = ServiceState.Running;
         public TimeSpan StopTakes = TimeSpan.FromSeconds(1);
         public TimeSpan ProcessEndsAfterStop = TimeSpan.FromSeconds(1.1);
         public TimeSpan ControllerAnswersAfterEnd = TimeSpan.FromSeconds(0.9);
         public TimeSpan? ControllerGivesUpAfter;
         public bool ControllerAnswersAtItsTimeout;
         public bool ProcessCanBeOpened = true;
         public TimeSpan? StallAfter;
         public int WaitHintMilliseconds = 90 * 1000;
         public string StopRefusal;
         public string StartRefusal;
         public bool StopsAsItStarts;
         public readonly List<string> Asked = new();
         public TimeSpan StartedAt;
         public TimeSpan StopAnsweredAt;
         public int StopAnsweredWith = -1;
         public bool StartedWhileTheOldProcessRan;

         /// <summary>When the process the service stopped in ended; TimeSpan.MaxValue while it has not.</summary>
         public TimeSpan OldProcessEndedAt = TimeSpan.MaxValue;

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

         /// <summary>The stop asked of the controller, which answers it as Windows' does; a refusal ends it at once, with the service as it was.</summary>
         public Task AskStop()
         {
            Advance_();
            Asked.Add("stop");
            if (StopRefusal != null)
               return Task.FromException(new InvalidOperationException(StopRefusal));
            if (State != ServiceState.Running && State != ServiceState.Paused)
               return Task.FromException(new InvalidOperationException("The service cannot accept control messages at this time."));

            State = ServiceState.StopPending;
            since_ = clock_.Elapsed;
            stopping_ = true;
            stoppingProcess_ = process_;
            OldProcessEndedAt = TimeSpan.MaxValue;
            answer_ = new TaskCompletionSource();
            return answer_.Task;
         }

         public void Start()
         {
            Advance_();
            if (State != ServiceState.Stopped)
               throw new InvalidOperationException("The service is starting or stopping. Please try again later.");
            Asked.Add("start");
            if (StartRefusal != null)
               throw new InvalidOperationException(StartRefusal);
            if (stopping_ && OldProcessEndedAt == TimeSpan.MaxValue)
               StartedWhileTheOldProcessRan = true;
            State = ServiceState.StartPending;
            since_ = clock_.Elapsed;
            StartedAt = clock_.Elapsed;
         }

         public IProcessExit WatchProcess(int processId)
            => ProcessCanBeOpened ? new FakeProcess(this, processId) : null;

         private TimeSpan StopEnd_() => StopTakes == TimeSpan.MaxValue ? TimeSpan.MaxValue : since_ + StopTakes;

         private TimeSpan ProcessEnd_() => StopTakes == TimeSpan.MaxValue ? TimeSpan.MaxValue : since_ + StopTakes + ProcessEndsAfterStop;

         private bool GivesUp_() => ControllerGivesUpAfter is TimeSpan giveUp && StopTakes > giveUp;

         private void Advance_()
         {
            TimeSpan now = clock_.Elapsed;
            TimeSpan pending = now - since_;
            if (State == ServiceState.StopPending)
            {
               if (StopTakes != TimeSpan.MaxValue && pending >= StopTakes)
                  State = ServiceState.Stopped;
               else if (GivesUp_() && pending >= ControllerGivesUpAfter.Value)
                  State = ServiceState.Stopped; // the controller's word, while the process drains on
            }
            else if (State == ServiceState.StartPending && pending >= TimeSpan.FromSeconds(2))
            {
               State = StopsAsItStarts ? ServiceState.Stopped : ServiceState.Running;
               process_++;
            }

            if (!stopping_ || State == ServiceState.StartPending || State == ServiceState.Running)
               return;

            TimeSpan processEnd = ProcessEnd_();
            if (OldProcessEndedAt == TimeSpan.MaxValue && now >= processEnd)
               OldProcessEndedAt = processEnd;

            if (answer_ == null || answer_.Task.IsCompleted)
               return;

            if (GivesUp_())
            {
               TimeSpan answeredAt = ControllerAnswersAtItsTimeout
                  ? since_ + ControllerGivesUpAfter.Value
                  : processEnd == TimeSpan.MaxValue ? TimeSpan.MaxValue : processEnd + ControllerAnswersAfterEnd;
               if (now >= answeredAt)
                  Answer_(answeredAt, new InvalidOperationException("Cannot stop 'hMailServer' service on computer '.'.", new Win32Exception(1053)));
            }
            else if (StopEnd_() != TimeSpan.MaxValue && now >= StopEnd_())
               Answer_(StopEnd_(), null);
         }

         private void Answer_(TimeSpan at, Exception error)
         {
            StopAnsweredAt = at;
            StopAnsweredWith = error?.InnerException is Win32Exception win32 ? win32.NativeErrorCode : 0;
            if (error == null)
               answer_.SetResult();
            else
               answer_.SetException(error);
         }

         /// <summary>The process the stop drains in, which ends as the fake's clock says.</summary>
         private sealed class FakeProcess : IProcessExit
         {
            private readonly FakeService service_;
            private readonly int processId_;

            public FakeProcess(FakeService service, int processId)
            {
               service_ = service;
               processId_ = processId;
            }

            public bool HasExited
            {
               get
               {
                  service_.Advance_();
                  return processId_ != service_.stoppingProcess_ || service_.OldProcessEndedAt != TimeSpan.MaxValue;
               }
            }

            public void Dispose()
            {
            }
         }
      }
   }
}
