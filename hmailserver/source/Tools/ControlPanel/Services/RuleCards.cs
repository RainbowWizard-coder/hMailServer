// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// The Rules page's words for its IF and THEN cards when they have nothing to list,
   /// and for a rule just added (review finding F225). With no rule selected both
   /// cards were blank, with a rule that had no criteria or actions too, and nothing
   /// said what that meant.
   ///
   /// A rule with no criteria matches no message: the server runs a rule's actions
   /// only once a criterion is met (RuleApplier::ApplyRule_ and RuleMatches_ leave the
   /// rule unmatched over an empty list; PersistentRule.cpp says such a rule is
   /// inert). The review took it to match every message; the card says what the
   /// server does.
   ///
   /// No WPF here, so the tests hold the words.
   /// </summary>
   public static class RuleCards
   {
      /// <summary>The IF card with nothing listed: no rule selected, or a rule without criteria.</summary>
      public static string CriteriaEmpty(bool ruleSelected)
         => ruleSelected
            ? L("No criteria yet, so this rule matches no message and does nothing.")
            : L("Select a rule above to see which messages it matches.");

      /// <summary>The THEN card with nothing listed: no rule selected, or a rule without actions.</summary>
      public static string ActionsEmpty(bool ruleSelected)
         => ruleSelected
            ? L("No actions yet, so this rule does nothing to the messages it matches.")
            : L("Select a rule above to see what it does.");

      /// <summary>What Add rule says of the rule it made, which starts switched off.</summary>
      public static string Added(string name)
         => F("{0} added, switched off: give it its criteria and actions, then Enable switches it on.", name);
   }
}
