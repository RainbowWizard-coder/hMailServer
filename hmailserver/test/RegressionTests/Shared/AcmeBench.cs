// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Threading;
using NUnit.Framework;
using RegressionTests.Infrastructure;

namespace RegressionTests.Shared
{
   /// <summary>
   ///    How the fixtures that let the server deploy an ACME certificate over COM -
   ///    TlsAcmeIssuance and TlsAcmeDeployment - leave the bench as they found it.
   ///
   ///    A deployment restarts the servers through Reinitializator, on a thread of its
   ///    own, and the start it makes runs a check of its own. A teardown that deleted
   ///    the ACME (automatic) record while ACME was still on - AcmeEnabled is applied
   ///    at a reinitialise, not when it is saved - could meet that check after the
   ///    deletion: it found a pair no record names, deployed it again, and its restart
   ///    landed in the next fixture, whose first COM call was then refused ("The server
   ///    has not loaded its configuration"). So ACME is switched off where it applies
   ///    first, by a reinitialise, and the bench waited for until it is quiet; only
   ///    then are the ports and the records put back. With ACME off no check is
   ///    scheduled at a start, and a save of AcmeDomains starts none.
   ///
   ///    Windows only: the COM fixtures use it, and the Linux suite links neither.
   /// </summary>
   internal static class AcmeBench
   {
      // How long the bench must answer, with ACME off and nothing running, before
      // it is taken to be quiet: a restart a deployment asked for before the
      // teardown's own reinitialise waits for that one to finish and then runs at
      // once, and it shows as COM refusing calls for its moment.
      private const int QuietSeconds = 3;

      public static Dictionary<string, object> Status(hMailServer.Application application)
      {
         return MiniJson.Object(application.Utilities.GetAcmeStatus());
      }

      /// <summary>
      ///    The status once COM answers and condition holds, polled for seconds; a
      ///    COMException is the servers restarting and is waited through. Fails the
      ///    test with the last status read when the condition never held.
      /// </summary>
      public static Dictionary<string, object> WaitFor(hMailServer.Application application, Func<Dictionary<string, object>, bool> condition,
                                                       string what, int seconds = 60)
      {
         DateTime until = DateTime.UtcNow.AddSeconds(seconds);
         string last = "";

         while (DateTime.UtcNow < until)
         {
            try
            {
               last = application.Utilities.GetAcmeStatus();
               Dictionary<string, object> state = MiniJson.Object(last);
               if (condition(state))
                  return state;
            }
            catch (COMException)
            {
               // A deployment restarts the servers; COM answers again a moment later.
            }

            Thread.Sleep(250);
         }

         Assert.Fail("Waited " + seconds + " seconds for " + what + ". Last status: " + last);
         return null;
      }

      /// <summary>
      ///    Until COM answers and no check is queued or running. A COMException is
      ///    not idle: it is the servers restarting, and a check runs at that start.
      /// </summary>
      public static void WaitUntilIdle(hMailServer.Application application, int seconds = 90)
      {
         WaitFor(application, s => MiniJson.At(s, "running") == null, "no ACME check to be queued or running", seconds);
      }

      /// <summary>
      ///    ACME off where it applies: whatever check runs ended, AcmeEnabled
      ///    removed, the server reinitialised so that it reads that, and the bench
      ///    then answering, with ACME off and no check running, for QuietSeconds in a
      ///    row. After this no check runs on this bench until a fixture turns ACME on
      ///    again, so that the ports and the records can be put back.
      /// </summary>
      public static void SwitchOff(hMailServer.Application application)
      {
         WaitUntilIdle(application);

         RetryHelper.TryAction(TimeSpan.FromSeconds(60), () => IniFileSetting.Write("AcmeEnabled", null));
         RetryHelper.TryAction(TimeSpan.FromSeconds(60), () => application.Reinitialize());

         DateTime until = DateTime.UtcNow.AddSeconds(90);
         DateTime quietSince = DateTime.MinValue;
         string last = "";

         while (DateTime.UtcNow < until)
         {
            bool quiet = false;
            try
            {
               last = application.Utilities.GetAcmeStatus();
               Dictionary<string, object> state = MiniJson.Object(last);
               quiet = !MiniJson.Flag(state, "enabled") && MiniJson.At(state, "running") == null;
            }
            catch (COMException)
            {
               // A restart under way.
            }

            if (!quiet)
               quietSince = DateTime.MinValue;
            else if (quietSince == DateTime.MinValue)
               quietSince = DateTime.UtcNow;
            else if (DateTime.UtcNow - quietSince >= TimeSpan.FromSeconds(QuietSeconds))
               return;

            Thread.Sleep(250);
         }

         Assert.Fail("ACME was not off and quiet after the reinitialise within 90 seconds. Last status: " + last);
      }
   }
}
