// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// The Server status page's State, as a word and a level for its pill, and the step
   /// the Updates page offers next, with no WPF so that ServerStatusFlowTests holds them
   /// (review finding F246). State was body text - "Stopped" for a server paused from
   /// that very page - and a paused engine raised no notice; the update steps were three
   /// equal buttons, two of them greyed with no reason given.
   /// </summary>
   public static class ServerStateText
   {
      /// <summary>Application.ServerState's values (eServerState on the server).</summary>
      public const int Stopped = 1, Starting = 2, Running = 3, Stopping = 4, Draining = 5;

      /// <summary>
      /// The engine's state in a word, at its level: Running Good; Paused - the engine
      /// stopped with the Windows service running, which is what Pause on this page
      /// does - and Draining, the maintenance mode that closes the listeners, Warning;
      /// Starting and Stopping, a change under way, Information; anything else Unknown.
      /// </summary>
      public static (string Word, StatusLevel Level) For(int state) => state switch
      {
         Running => (L("Running"), StatusLevel.Good),
         Stopped => (L("Paused"), StatusLevel.Warning),
         Draining => (L("Draining"), StatusLevel.Warning),
         Starting => (L("Starting"), StatusLevel.Information),
         Stopping => (L("Stopping"), StatusLevel.Information),
         _ => (L("Unknown"), StatusLevel.Normal)
      };

      /// <summary>Whether the page says, under its header, that the engine is paused.</summary>
      public static bool IsPaused(int state) => state == Stopped;
   }

   /// <summary>The one update step the Updates page offers as its Primary button, or none.</summary>
   public enum UpdateStep
   {
      /// <summary>Nothing to press: an install under way, or one going on in its own window.</summary>
      None,
      Check,
      Download,
      Install
   }

   /// <summary>
   /// Which step follows Status.UpdateState (review finding F246): Check for updates
   /// until a newer release is known; Download update once one is - or after a step
   /// that failed with a release known, which a download retries; Install update once
   /// it is downloaded and verified; and none while an install is under way (4), being
   /// prepared (6) or going on in the administrator's window (7). With a release known,
   /// checking again is the secondary beside it.
   /// </summary>
   public static class UpdateSteps
   {
      public static UpdateStep Next(int state, string version)
      {
         bool known = !string.IsNullOrEmpty(version);
         return state switch
         {
            2 => known ? UpdateStep.Download : UpdateStep.Check,
            5 => known ? UpdateStep.Download : UpdateStep.Check,
            3 => UpdateStep.Install,
            4 or 6 or 7 => UpdateStep.None,
            _ => UpdateStep.Check
         };
      }
   }
}
