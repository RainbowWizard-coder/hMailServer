// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Security.Cryptography;
using System.Threading;

namespace RegressionTests.Shared
{
   /// <summary>
   ///    RFC 6238 codes for a test - HMAC-SHA1, thirty-second steps, six digits -
   ///    computed here with .NET's own HMACSHA1 and nothing the server owns, which
   ///    is what an authenticator app shows.
   ///
   ///    The server takes a code once (SecondFactorGuard, RFC 6238 section 5.2): a
   ///    code it has accepted for an owner is refused if it comes again, and so is
   ///    the code of any earlier step, whichever door - COM or the REST API - it
   ///    comes to. So a test that needs the right code more than once asks Fresh,
   ///    which never hands out a step it has handed out before for that secret and
   ///    hands them out in order: the current step, then the next (both inside the
   ///    server's window for the next thirty seconds at least), and when both have
   ///    gone it waits for the clock to reach the next step. The Linux suite's
   ///    ServerApi uses it too, so that the codes a fixture takes and the ones the
   ///    fixture layer takes on its behalf never collide.
   ///
   ///    A test that judges codes against the server's step - the previous step's
   ///    code is taken, the next step's is, one five steps out is not - computes
   ///    them with At (or a CurrentCode of its own) and asks WaitForRoomInTheStep
   ///    first, so that the step the test computes in is the step the server judges
   ///    in.
   /// </summary>
   public static class OneTimeCodes
   {
      private const int PeriodSeconds = 30;

      /// <summary>
      ///    The seconds of a step WaitForRoomInTheStep leaves a test: every code a
      ///    test of the server's window computes, and every sign-in that judges it,
      ///    within one step.
      /// </summary>
      public const int RoomNeededSeconds = 10;

      private static readonly Dictionary<string, long> LastStepHandedOut = new Dictionary<string, long>();

      /// <summary>
      ///    Returns at once when at least RoomNeededSeconds of the current thirty-second
      ///    step remain, and otherwise sleeps until the next step has begun. Asked by a
      ///    test that computes codes relative to the current step and expects the
      ///    server to judge them in that same step: pipeline 2887524317 (27 September
      ///    2026) failed TwoFactorAuthentication.ACodeComputedElsewhereIsAccepted when
      ///    the test began half a second before a step ended, computed the previous
      ///    step's code in step s and had it judged in step s+1, two steps behind the
      ///    server and outside its window.
      /// </summary>
      public static void WaitForRoomInTheStep()
      {
         WaitForRoomInTheStep(() => DateTimeOffset.UtcNow, Thread.Sleep);
      }

      /// <summary>WaitForRoomInTheStep with its clock and its sleep given: the wait it slept, or zero.</summary>
      internal static TimeSpan WaitForRoomInTheStep(Func<DateTimeOffset> clock, Action<TimeSpan> sleep)
      {
         const long periodMilliseconds = PeriodSeconds * 1000L;

         long millisecondsLeft = periodMilliseconds - clock().ToUnixTimeMilliseconds() % periodMilliseconds;
         if (millisecondsLeft >= RoomNeededSeconds * 1000L)
            return TimeSpan.Zero;

         // A quarter of a second past the boundary, so that the step the test reads
         // afterwards is the next one and not the last millisecond of this one.
         TimeSpan wait = TimeSpan.FromMilliseconds(millisecondsLeft + 250);
         sleep(wait);
         return wait;
      }

      /// <summary>A code of the secret that nobody has been handed yet, in step order.</summary>
      public static string Fresh(string base32Secret)
      {
         while (true)
         {
            TimeSpan wait;

            lock (LastStepHandedOut)
            {
               long now = DateTimeOffset.UtcNow.ToUnixTimeSeconds() / PeriodSeconds;

               long last;
               long step = LastStepHandedOut.TryGetValue(base32Secret, out last) ? Math.Max(now, last + 1) : now;

               if (step <= now + 1)
               {
                  LastStepHandedOut[base32Secret] = step;
                  return AtStep(base32Secret, step);
               }

               long nextStepStarts = (now + 1) * PeriodSeconds;
               wait = TimeSpan.FromSeconds(nextStepStarts - DateTimeOffset.UtcNow.ToUnixTimeSeconds()) + TimeSpan.FromMilliseconds(250);
            }

            if (wait > TimeSpan.Zero)
               Thread.Sleep(wait);
         }
      }

      /// <summary>The code of the step this many steps from now, handed out or not.</summary>
      public static string At(string base32Secret, int stepOffset = 0)
      {
         return AtStep(base32Secret, DateTimeOffset.UtcNow.ToUnixTimeSeconds() / PeriodSeconds + stepOffset);
      }

      /// <summary>Six digits that are none of the codes the server would take in the next minute and a half.</summary>
      public static string Wrong(string base32Secret)
      {
         var near = new HashSet<string>();
         for (int step = -2; step <= 3; step++)
            near.Add(At(base32Secret, step));

         for (int candidate = 0; ; candidate += 111111)
         {
            string code = (candidate % 1000000).ToString("000000");
            if (!near.Contains(code))
               return code;
         }
      }

      private static string AtStep(string base32Secret, long counter)
      {
         byte[] key = Base32Decode(base32Secret);

         byte[] counterBytes = BitConverter.GetBytes(counter);
         if (BitConverter.IsLittleEndian)
            Array.Reverse(counterBytes);

         byte[] hash;
         using (var hmac = new HMACSHA1(key))
            hash = hmac.ComputeHash(counterBytes);

         int offset = hash[hash.Length - 1] & 0x0F;
         int binary =
            ((hash[offset] & 0x7F) << 24) |
            ((hash[offset + 1] & 0xFF) << 16) |
            ((hash[offset + 2] & 0xFF) << 8) |
            (hash[offset + 3] & 0xFF);

         return (binary % 1000000).ToString("000000");
      }

      private static byte[] Base32Decode(string input)
      {
         const string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
         var output = new List<byte>();
         int buffer = 0, bitsLeft = 0;

         foreach (char c in input)
         {
            int index = alphabet.IndexOf(char.ToUpperInvariant(c));
            if (index < 0)
               continue;

            buffer = (buffer << 5) | index;
            bitsLeft += 5;
            if (bitsLeft >= 8)
            {
               bitsLeft -= 8;
               output.Add((byte) ((buffer >> bitsLeft) & 0xFF));
            }
         }

         return output.ToArray();
      }
   }
}
