// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// The Update row of the Status page: one line from Status.UpdateState and its
   /// companions. The last apply's outcome, reported when the service came back,
   /// comes first when there is one: it is what the administrator who clicked
   /// Install update is waiting for. Out of StatusView so that every state is
   /// tested, the administrator's install's two (6 and 7) with the rest.
   /// </summary>
   public static class UpdateStatusText
   {
      public static string For(int state, string version, string published, string checkedAt, string error, string applyOutcome = null, string preparationStep = null)
      {
         // The last outcome is a sentence of its own, before the state's line.
         string outcome = string.IsNullOrEmpty(applyOutcome) ? null : F("Last update: {0}.", applyOutcome);
         return Sentences(outcome, Line_(state, version, published, checkedAt, error, preparationStep));
      }

      // The state's line, each with and without the date it was published or
      // checked as a whole text, so that a translation can place the date.
      private static string Line_(int state, string version, string published, string checkedAt, string error, string preparationStep)
      {
         bool dated = !string.IsNullOrEmpty(published);
         switch (state)
         {
            case 1: return string.IsNullOrEmpty(checkedAt) ? L("This is the latest release") : F("This is the latest release, checked {0}", checkedAt);
            case 2: return dated ? F("{0} is available (published {1})", version, published) : F("{0} is available", version);
            case 3:
               return dated
                  ? F("{0} is downloaded and verified (published {1}); Install update applies it", version, published)
                  : F("{0} is downloaded and verified; Install update applies it", version);
            case 4: return F("Installing {0}: the service will stop and start", version);
            case 5: return F("The last update step failed: {0}", error);
            case 6: return F("Preparing the install of {0}: {1}", version, PreparationStep(preparationStep));
            case 7: return F("{0} is ready to be installed; the install goes on in its window", version);
            default: return L("Not checked yet (Check for updates, or turn on the daily check under Updates on the API & monitoring page)");
         }
      }

      /// <summary>The service's preparation step (UpdatePreparation's "step") in words.</summary>
      public static string PreparationStep(string step)
      {
         switch (step)
         {
            case "backup": return L("taking the backup");
            case "rollback": return L("fetching the version to go back to");
            case "ready": return L("ready");
            default: return L("checking the downloaded installer");
         }
      }
   }
}
