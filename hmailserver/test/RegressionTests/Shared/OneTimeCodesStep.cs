// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using NUnit.Framework;
using RegressionTests.Shared;

// Not RegressionTests.*: the suite's SetUpFixture (SuiteDns) signs in to the
// server over COM before any fixture of that namespace runs, and this one needs
// no server. It runs alone with
//    nunit3-console RegressionTests.dll --where "namespace == ServerFreeTests"
// on a machine whose hMailServer must not be touched, and in every full run.
namespace ServerFreeTests
{
   /// <summary>
   ///    OneTimeCodes.WaitForRoomInTheStep against a clock the test holds: the run of
   ///    pipeline 2887524317 replayed, and the boundary of the room it leaves.
   /// </summary>
   [TestFixture]
   public class OneTimeCodesStep
   {
      private const int PeriodSeconds = 30;

      // 27 September 2026 23:37:29.5 UTC, when TwoFactorAuthentication.
      // ACodeComputedElsewhereIsAccepted began on the Windows gate of pipeline
      // 2887524317: half a second before the step that began at 23:37:00 ended.
      private static readonly DateTimeOffset TheRunThatFailed =
         new DateTimeOffset(2026, 9, 27, 23, 37, 29, 500, TimeSpan.Zero);

      private sealed class Clock
      {
         public DateTimeOffset Now;
         public int Sleeps;

         public void Sleep(TimeSpan wait)
         {
            Now += wait;
            Sleeps++;
         }
      }

      private static long Step(DateTimeOffset at)
      {
         return at.ToUnixTimeSeconds() / PeriodSeconds;
      }

      /// <summary>
      ///    The test's first two sign-ins as they ran: the previous step's code computed
      ///    0.4 seconds in, judged by the server at 0.523 seconds (23:37:30.023) in a
      ///    window of one step either side of the server's own. True when it was taken.
      /// </summary>
      private static bool PreviousStepTaken(Clock clock, bool waitFirst)
      {
         if (waitFirst)
            OneTimeCodes.WaitForRoomInTheStep(() => clock.Now, clock.Sleep);

         long codeStep = Step(clock.Now.AddMilliseconds(400)) - 1;
         long serverStep = Step(clock.Now.AddMilliseconds(523));

         return Math.Abs(serverStep - codeStep) <= 1;
      }

      [Test]
      [Description("The run of pipeline 2887524317 replayed: begun half a second before a step ended, the previous step's code is two steps behind the server without the wait - the negative control, the failure the gate saw - and taken with it.")]
      public void ThePreviousStepsCodeIsJudgedInTheStepItWasComputedIn()
      {
         Assert.That((TheRunThatFailed.ToUnixTimeMilliseconds() % (PeriodSeconds * 1000L)), Is.EqualTo(29500),
            "The replay begins 29.5 seconds into a step.");

         Assert.That(PreviousStepTaken(new Clock { Now = TheRunThatFailed }, waitFirst: false), Is.False,
            "Without the wait the previous step's code must be two steps behind the server, as it was on the gate.");

         var clock = new Clock { Now = TheRunThatFailed };
         Assert.That(PreviousStepTaken(clock, waitFirst: true), Is.True,
            "After the wait the previous step's code must be judged in the step it was computed in.");
         Assert.That(clock.Sleeps, Is.EqualTo(1));
         Assert.That(Step(clock.Now), Is.EqualTo(Step(TheRunThatFailed) + 1), "The wait must end in the next step.");
         Assert.That(PeriodSeconds * 1000L - clock.Now.ToUnixTimeMilliseconds() % (PeriodSeconds * 1000L),
            Is.GreaterThanOrEqualTo(OneTimeCodes.RoomNeededSeconds * 1000L),
            "...with the room the test asked for.");
      }

      [Test]
      [Description("A step with RoomNeededSeconds or more left is not waited for; one with a millisecond less is, into the next step.")]
      public void OnlyAStepWithLessRoomThanNeededIsWaitedFor()
      {
         DateTimeOffset stepBegins = TheRunThatFailed.AddMilliseconds(-29500);

         foreach (int secondsIn in new[] { 0, 1, 15, PeriodSeconds - OneTimeCodes.RoomNeededSeconds })
         {
            var clock = new Clock { Now = stepBegins.AddSeconds(secondsIn) };
            Assert.That(OneTimeCodes.WaitForRoomInTheStep(() => clock.Now, clock.Sleep), Is.EqualTo(TimeSpan.Zero),
               secondsIn + " seconds into the step, " + (PeriodSeconds - secondsIn) + " left, is room enough.");
            Assert.That(clock.Sleeps, Is.EqualTo(0));
         }

         foreach (int millisecondsIn in new[] { (PeriodSeconds - OneTimeCodes.RoomNeededSeconds) * 1000 + 1, 25000, 29999 })
         {
            var clock = new Clock { Now = stepBegins.AddMilliseconds(millisecondsIn) };
            TimeSpan waited = OneTimeCodes.WaitForRoomInTheStep(() => clock.Now, clock.Sleep);
            Assert.That(waited, Is.GreaterThan(TimeSpan.Zero), millisecondsIn + " ms into the step is too little room.");
            Assert.That(clock.Sleeps, Is.EqualTo(1));
            Assert.That(Step(clock.Now), Is.EqualTo(Step(stepBegins) + 1), "The wait must end in the next step.");
            Assert.That(waited, Is.LessThanOrEqualTo(TimeSpan.FromSeconds(OneTimeCodes.RoomNeededSeconds + 1)),
               "...and no later than a moment into it.");
         }
      }
   }
}
