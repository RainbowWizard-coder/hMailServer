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

      /// <summary>
      /// What Add rule says of the rule it made, which starts switched off, in the notice
      /// that carries Enable now - pressed only once the rule has a criterion and an
      /// action (<see cref="CanSwitchOn"/>), which the notice says.
      /// </summary>
      public static string Added(string name)
         => F("{0} added, switched off: Enable now switches it on once it has a criterion and an action.", name);

      /// <summary>What the notice says once the new rule has a criterion and an action, and Enable now can be pressed.</summary>
      public static string Ready(string name)
         => F("{0} has a criterion and an action: Enable now switches it on.", name);

      /// <summary>What the notice says once its Enable now has switched the new rule on.</summary>
      public static string SwitchedOn(string name)
         => F("{0} switched on.", name);

      /// <summary>
      /// Whether the notice's Enable now may switch the new rule on: once it has at
      /// least one criterion and one action. A rule switched on before them acts on
      /// live mail while the rest is still being added - under AND, on more mail than
      /// it is meant to - which is why it starts switched off.
      /// </summary>
      public static bool CanSwitchOn(int criteria, int actions) => criteria > 0 && actions > 0;
   }
}
