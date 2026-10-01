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
         string when = string.IsNullOrEmpty(published) ? "" : F(" (published {0})", published);
         string outcome = string.IsNullOrEmpty(applyOutcome) ? "" : F("Last update: {0}. ", applyOutcome);
         switch (state)
         {
            case 1: return outcome + L("This is the latest release") + (string.IsNullOrEmpty(checkedAt) ? "" : F(", checked {0}", checkedAt));
            case 2: return outcome + F("{0} is available", version) + when;
            case 3: return outcome + F("{0} is downloaded and verified", version) + when + L("; Install update applies it");
            case 4: return outcome + F("Installing {0}: the service will stop and start", version);
            case 5: return outcome + F("The last update step failed: {0}", error);
            case 6: return outcome + F("Preparing the install of {0}: {1}", version, PreparationStep(preparationStep));
            case 7: return outcome + F("{0} is ready to be installed; the install goes on in its window", version);
            default: return outcome + L("Not checked yet (Check for updates, or turn on the daily check under Updates on the API & monitoring page)");
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
