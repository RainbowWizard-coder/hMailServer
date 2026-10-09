// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Collections.Generic;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>The state of the server settings page, as the pill in its header shows it.
   /// Distinct from <see cref="SettingsPageState"/>, which is the lifecycle of an
   /// hMailServer.INI page: that one is read, written and then waits on a service
   /// restart, and this one is read and written over COM and takes effect as it is
   /// saved. Two pages, two lifecycles, two types.</summary>
   public enum ServerSettingsPageState
   {
      /// <summary>What is on screen is what the server holds. Nothing to say, so no pill.</summary>
      Clean,

      /// <summary>An editor has been changed since the page was read or last saved.</summary>
      Unsaved,

      /// <summary>The writes are in progress.</summary>
      Saving,

      /// <summary>Every write succeeded.</summary>
      Saved,

      /// <summary>At least one write failed; the sentence beside the pill says how many.</summary>
      PartlySaved
   }

   /// <summary>
   /// What the status pill and the notice on a settings page say for each state:
   /// a status level (a colour and a shape through <see cref="StatusSemantics"/>)
   /// and a word for the pill, and the sentences the notice carries after a read
   /// or a save. WPF-free so a test can hold every state that shows a pill to a
   /// distinct word and the right severity - the pill is the one thing on the
   /// page that says whether what is on screen has reached the server, and a
   /// state drawn in another state's colour would be a lie told permanently.
   /// </summary>
   public static class ServerSettingsPageStatus
   {
      /// <summary>Every state, in the order a page moves through them. Only used by the tests today.</summary>
      public static IReadOnlyList<ServerSettingsPageState> AllStates { get; } = new[]
      {
         ServerSettingsPageState.Clean,
         ServerSettingsPageState.Unsaved,
         ServerSettingsPageState.Saving,
         ServerSettingsPageState.Saved,
         ServerSettingsPageState.PartlySaved
      };

      /// <summary>
      /// Unsaved is a warning because leaving the page loses the change; Saving
      /// is information because there is nothing to do but wait; a save with a
      /// failure is critical because what is on screen is not what the server
      /// holds and the reader has to find out which rows.
      /// </summary>
      public static StatusLevel LevelFor(ServerSettingsPageState state)
      {
         switch (state)
         {
            case ServerSettingsPageState.Unsaved:
               return StatusLevel.Warning;
            case ServerSettingsPageState.Saving:
               return StatusLevel.Information;
            case ServerSettingsPageState.Saved:
               return StatusLevel.Good;
            case ServerSettingsPageState.PartlySaved:
               return StatusLevel.Critical;
            default:
               return StatusLevel.Normal;
         }
      }

      /// <summary>The word on the pill; empty for the state that shows none.</summary>
      public static string WordFor(ServerSettingsPageState state)
      {
         switch (state)
         {
            case ServerSettingsPageState.Unsaved:
               return L("Unsaved changes");
            case ServerSettingsPageState.Saving:
               return L("Saving…");
            case ServerSettingsPageState.Saved:
               return L("Saved");
            case ServerSettingsPageState.PartlySaved:
               return L("Partly saved");
            default:
               return "";
         }
      }

      /// <summary>Whether the pill is shown at all. A clean page has nothing to say.</summary>
      public static bool ShowsPill(ServerSettingsPageState state) => state != ServerSettingsPageState.Clean;

      /// <summary>
      /// The sentence after a read: null when every value was read, which is the
      /// ordinary case and not worth a notice; otherwise how many were not and
      /// the first reason.
      /// </summary>
      public static string ReadSentence(int failedReads, string diagnosis)
      {
         if (failedReads <= 0)
            return null;

         return PluralF(failedReads, "{0} setting could not be read: {1}", "{0} settings could not be read: {1}", failedReads, diagnosis ?? "");
      }

      /// <summary>
      /// The sentence after a save. A save publishes (docs/SettingsApply.md), so a
      /// row kept in the settings store applies as a COM row does unless the server
      /// marks its key reinitialize or restart and lists it as pending; waiting is
      /// how many of this page's keys it lists, and the sentence claims exactly
      /// that - no restart nobody needs, and no effect that has not happened. Null
      /// waiting is a server that cannot say - one from before a save could publish,
      /// whose hMailServer.ini settings all apply at its next start - and the
      /// sentence then says what it said then rather than claim they applied.
      /// </summary>
      public static string SaveSentence(int saved, int failed, string timeText, int? waiting)
      {
         // Each count is a whole sentence in the forms its language has (Loc.Plural);
         // where there are two, the first is placed whole in the second.
         string time = timeText ?? "";
         if (failed > 0)
            return PluralF(failed, "{0} {1} setting could not be written.", "{0} {1} settings could not be written.",
               Plural(saved, "Saved {0} setting.", "Saved {0} settings."), failed);

         if (waiting == null)
            return PluralF(saved,
               "Saved {0} setting at {1} - server settings applied immediately; hMailServer.ini settings apply after a service restart.",
               "Saved {0} settings at {1} - server settings applied immediately; hMailServer.ini settings apply after a service restart.",
               saved, time);

         if (waiting > 0)
            return PluralF(waiting.Value,
               "{0} {1} of them waits for a reinitialise or a restart; the rest applied immediately.",
               "{0} {1} of them wait for a reinitialise or a restart; the rest applied immediately.",
               PluralF(saved, "Saved {0} setting at {1}.", "Saved {0} settings at {1}.", saved, time), waiting.Value);

         return PluralF(saved, "Saved {0} setting at {1} - applied immediately.", "Saved {0} settings at {1} - applied immediately.", saved, time);
      }
   }
}
