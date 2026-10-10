// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// The setup checklist's tally in words: the sentences the Dashboard's setup card
   /// and the Setup checklist page count the same states with (review finding F034).
   /// The Dashboard said "11 things need attention" and the checklist "11 need action,
   /// 1 cannot be told from here, 9 done, 4 not needed." - two names for one state
   /// beside pills that say Action needed on both pages, and a first clause with no
   /// noun. Both pages now say each state in one sentence of its own here, a whole
   /// sentence in the forms its language has for the count (Loc.Plural), in the
   /// words of the state's pill: needs action, cannot be checked from here, done,
   /// not needed.
   ///
   /// Counts and text only, without WPF or the checks, so that the tests hold the
   /// sentences and the pages read them from one place.
   /// </summary>
   public static class SetupTally
   {
      /// <summary>"3 items need action."</summary>
      public static string NeedAction(int count)
         => Plural(count, "{0} item needs action.", "{0} items need action.");

      /// <summary>"1 item cannot be checked from here."</summary>
      public static string CannotBeChecked(int count)
         => Plural(count, "{0} item cannot be checked from here.", "{0} items cannot be checked from here.");

      /// <summary>"9 items are done."</summary>
      public static string Done(int count)
         => Plural(count, "{0} item is done.", "{0} items are done.");

      /// <summary>"4 items are not needed."</summary>
      public static string NotNeeded(int count)
         => Plural(count, "{0} item is not needed.", "{0} items are not needed.");

      /// <summary>
      /// The Dashboard's card: what needs action and what cannot be checked, or
      /// that nothing needs action, out of how many checks.
      /// </summary>
      public static string Summary(int action, int unknown, int total)
      {
         if (action > 0)
            return Sentences(NeedAction(action), unknown > 0 ? CannotBeChecked(unknown) : null);

         if (unknown > 0)
            return Plural(unknown, "Nothing needs action, but {0} item cannot be checked from here.", "Nothing needs action, but {0} items cannot be checked from here.");

         return Plural(total,
            "Nothing needs action - the {0} external setup check is done or not needed.",
            "Nothing needs action - all {0} external setup checks are done or not needed.");
      }

      /// <summary>
      /// The Setup checklist page's tally: what needs action, or that nothing does,
      /// then each other state that some item is in, then the next step where there
      /// is one (the first item, in the checklist's order, that needs action).
      /// </summary>
      public static string Tally(int action, int unknown, int done, int notNeeded, string nextStep)
         => Sentences(
            action > 0 ? NeedAction(action) : L("Nothing needs action."),
            unknown > 0 ? CannotBeChecked(unknown) : null,
            done > 0 ? Done(done) : null,
            notNeeded > 0 ? NotNeeded(notNeeded) : null,
            string.IsNullOrEmpty(nextStep) ? null : F("Next step: {0}.", nextStep));
   }
}
