// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Collections.Generic;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>Where an hMailServer.INI settings page stands, as its header pill shows it.</summary>
   public enum SettingsPageState
   {
      /// <summary>
      /// What is on screen is what was read, and nothing has been saved since. Nothing
      /// to say, so no pill: the line under the header already names the file. It was
      /// Editing, a grey pill shown as the page opened with nothing changed.
      /// </summary>
      Clean,

      /// <summary>An editor holds something other than what was read or last saved.</summary>
      Unsaved,

      /// <summary>hMailServer.INI is not on this machine; the page can show nothing and save nothing.</summary>
      Unavailable,

      /// <summary>The save was written, and every setting on the page is in effect.</summary>
      Saved,

      /// <summary>The save was written; some of it waits for a reinitialise, which the window's Apply now does.</summary>
      WaitsForReinitialise,

      /// <summary>
      /// The save was written; some of it waits for the service to be restarted - or the
      /// server cannot say what waits, and applies its settings at its next start.
      /// </summary>
      WaitsForRestart,

      /// <summary>The service is being stopped and started.</summary>
      Restarting,

      /// <summary>The restart failed; the file holds the new values, the running service the old.</summary>
      RestartFailed,

      /// <summary>The service came back but the Control Panel could not reattach to it.</summary>
      NotConnected,

      /// <summary>The service restarted with the saved values.</summary>
      Applied
   }

   /// <summary>
   /// The level and the word of the pill on an hMailServer.INI settings page - the
   /// feature settings pages, Directory authentication and Directory synchronisation -
   /// per state, in the model the server settings page has always used
   /// (<see cref="ServerSettingsPageStatus"/>): no pill while the page shows what it
   /// read, Unsaved changes from the first edit, and after a save Saved, Reinitialise
   /// to apply or Restart to apply until the next edit. WPF-free so a test can hold
   /// every state that shows a pill to a distinct word and the right severity - the
   /// pill is what says whether what is on screen is what the server runs, and a
   /// state drawn in another state's colour would say the wrong thing permanently.
   /// </summary>
   public static class SettingsPageStatus
   {
      /// <summary>Every state, in the order a save moves through them. Only used by the tests today.</summary>
      public static IReadOnlyList<SettingsPageState> AllStates { get; } = new[]
      {
         SettingsPageState.Clean,
         SettingsPageState.Unsaved,
         SettingsPageState.Unavailable,
         SettingsPageState.Saved,
         SettingsPageState.WaitsForReinitialise,
         SettingsPageState.WaitsForRestart,
         SettingsPageState.Restarting,
         SettingsPageState.RestartFailed,
         SettingsPageState.NotConnected,
         SettingsPageState.Applied
      };

      /// <summary>
      /// Unsaved is a warning, because leaving the page or reloading it loses the
      /// change. Saved and Applied are Good. A save that waits for a reinitialise or a
      /// restart is information: it is written, and what is left is a step the reader
      /// takes when it suits, never a fault. Restarting is a warning because the values
      /// on screen are not yet the server's; a failed restart is critical because the
      /// file and the service now disagree; a lost reattach is a warning because the
      /// settings are live but the page cannot see the server.
      /// </summary>
      public static StatusLevel LevelFor(SettingsPageState state)
      {
         switch (state)
         {
            case SettingsPageState.Saved:
            case SettingsPageState.Applied:
               return StatusLevel.Good;
            case SettingsPageState.WaitsForReinitialise:
            case SettingsPageState.WaitsForRestart:
               return StatusLevel.Information;
            case SettingsPageState.Unsaved:
            case SettingsPageState.Restarting:
            case SettingsPageState.NotConnected:
               return StatusLevel.Warning;
            case SettingsPageState.Unavailable:
            case SettingsPageState.RestartFailed:
               return StatusLevel.Critical;
            default:
               return StatusLevel.Normal;
         }
      }

      /// <summary>The word on the pill; empty for the state that shows none.</summary>
      public static string WordFor(SettingsPageState state)
      {
         switch (state)
         {
            case SettingsPageState.Unsaved:
               return L("Unsaved changes");
            case SettingsPageState.Unavailable:
               return L("Unavailable");
            case SettingsPageState.Saved:
               return L("Saved");
            case SettingsPageState.WaitsForReinitialise:
               return L("Reinitialise to apply");
            case SettingsPageState.WaitsForRestart:
               return L("Restart to apply");
            case SettingsPageState.Restarting:
               return L("Restarting…");
            case SettingsPageState.RestartFailed:
               return L("Not restarted");
            case SettingsPageState.NotConnected:
               return L("Not connected");
            case SettingsPageState.Applied:
               return L("Applied");
            default:
               return "";
         }
      }

      /// <summary>Whether the pill is shown at all. A page that shows what it read has nothing to say.</summary>
      public static bool ShowsPill(SettingsPageState state) => state != SettingsPageState.Clean;

      /// <summary>
      /// The state the pill shows: Unsaved changes while an editor differs from what
      /// was read or last saved, and otherwise the state the page settled in after its
      /// last read, save or restart - so an edit undone by hand takes the pill back to
      /// what the save had said, and the first edit after a save replaces Saved.
      /// </summary>
      /// <param name="settled">The state of the last read, save or restart.</param>
      /// <param name="hasUnsavedChanges">Whether an editor now differs from what was read or saved.</param>
      public static SettingsPageState Follow(SettingsPageState settled, bool hasUnsavedChanges)
         => hasUnsavedChanges ? SettingsPageState.Unsaved : settled;

      /// <summary>
      /// The state a save that wrote everything settles in: what still waits, as the
      /// server lists its keys. Null is a server that cannot say - one from before a save
      /// could publish, whose settings all apply at its next start - and is a restart.
      /// </summary>
      /// <param name="waiting">How many of the page's saved keys the server lists as pending; null when it cannot say.</param>
      /// <param name="waitingForRestart">How many of those are marked restart.</param>
      public static SettingsPageState AfterSave(int? waiting, int waitingForRestart)
      {
         if (waiting == null || waitingForRestart > 0)
            return SettingsPageState.WaitsForRestart;

         return waiting > 0 ? SettingsPageState.WaitsForReinitialise : SettingsPageState.Saved;
      }
   }
}
