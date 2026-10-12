// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// The Update status row of the Updates page: one line from Status.UpdateState and its
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
      // checked as a whole text, so that a translation can place the date. The
      // feed's day of publication is the reader's short date, and the time of the
      // last check the reader's short date and time marked as the server's (the
      // owner's decision 13), where both were the server's own "2026-10-01" and
      // "2026-10-12 08:00:00"; a text in another form is shown as it came.
      private static string Line_(int state, string version, string published, string checkedAt, string error, string preparationStep)
      {
         bool dated = !string.IsNullOrEmpty(published);
         published = ServerTimeText.Parse(published) is DateTime day ? ServerTimeText.Date(day) : published;
         checkedAt = ServerTimeText.Parse(checkedAt) is DateTime when ? ServerTimeText.Marked(when) : checkedAt;
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
            default: return L("Not checked yet (Check for updates, or turn on the daily check below)");
         }
      }

      /// <summary>
      /// The one line Server status, the Dashboard and About say while a newer
      /// release is waiting - known, or downloaded and verified - and null while
      /// none is, or its version is not known: the Updates page says the rest.
      /// </summary>
      public static string Waiting(int state, string version)
      {
         if (string.IsNullOrEmpty(version))
            return null;

         switch (state)
         {
            case 2: return F("hMailServer {0} is available.", version);
            case 3:
            case 7: return F("hMailServer {0} is downloaded and verified, ready to install.", version);
            default: return null;
         }
      }

      /// <summary>
      /// The line Server status, the Dashboard and About say, and its level: the last
      /// apply's outcome until the Updates page has shown it to this reader
      /// (<paramref name="seenOutcome"/>), an update step that failed, and a release that
      /// is waiting - each a sentence of its own, in that order. Before the update
      /// steps moved to the Updates page, Server status's Update status row said the
      /// first two, and the line that took its place said only the third: an unattended
      /// apply that was rolled back, or a check that failed, showed on a page under
      /// Maintenance the administrator had no reason to open, and the health page said
      /// nothing. An outcome or a failure is a warning, but an apply that went as asked
      /// - ok, or an administrator's install cancelled - and a waiting release are
      /// information. A null text, at Normal, when there is nothing to say.
      /// </summary>
      public static (StatusLevel Level, string Text) WaitingOrFailed(int state, string version, string error, string applyOutcome, string seenOutcome)
      {
         StatusLevel level = StatusLevel.Information;

         string outcome = null;
         if (!string.IsNullOrWhiteSpace(applyOutcome) && !string.Equals(applyOutcome.Trim(), seenOutcome?.Trim(), StringComparison.Ordinal))
         {
            outcome = F("Last update: {0}.", applyOutcome.Trim());
            if (!WentAsAsked_(applyOutcome))
               level = StatusLevel.Warning;
         }

         string failed = null;
         if (state == 5)
         {
            failed = F("The last update step failed: {0}", error ?? "");
            level = StatusLevel.Warning;
         }

         string text = Sentences(outcome, failed, Waiting(state, version));
         return text.Length == 0 ? (StatusLevel.Normal, null) : (level, text);
      }

      // The Updater's status word, the outcome's first: ok and cancelled are an apply
      // that went as it was asked to; every other word it writes is one that did not.
      private static bool WentAsAsked_(string applyOutcome)
      {
         string first = applyOutcome.Trim();
         int space = first.IndexOf(' ');
         string status = space < 0 ? first : first.Substring(0, space);
         return status == "ok" || status == "cancelled"; // no-loc: the Updater's words
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
