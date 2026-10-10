// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// The Journaling and dual delivery page's words for a rule's state and for
   /// what an act on a rule did (review finding F255). The On column read "Yes"
   /// for a rule that copies and nothing at all for one that does not, and
   /// adding, switching and removing a rule said nothing: the list was read
   /// again and the reader had to find the row to see whether anything had
   /// happened. The switch's own caption, Turn off or Turn on, is the page's
   /// (JournalingView.ShowToggle_), where the access-key checker reads it with
   /// the page's other keys.
   ///
   /// No WPF here, so the tests hold the words.
   /// </summary>
   public static class JournalRuleText
   {
      /// <summary>A rule's state in the On column: On or Off, never an empty cell.</summary>
      public static string State(bool enabled) => enabled ? L("On") : L("Off");

      /// <summary>After an add, naming whose mail the new rule copies.</summary>
      public static string Added(string target) => F("Rule for {0} added.", target);

      /// <summary>After a switch, naming the rule and the way it went.</summary>
      public static string Switched(string target, bool nowEnabled)
         => nowEnabled ? F("Rule for {0} turned on.", target) : F("Rule for {0} turned off.", target);

      /// <summary>After a removal; <paramref name="removed"/> false when the server had no such rule any more.</summary>
      public static string Removed(string target, bool removed)
         => removed ? F("Rule for {0} removed.", target) : F("The rule for {0} was not there any more.", target);

      /// <summary>
      /// The example in the Whose mail box, by the choice beside it: a domain's
      /// name or an account's address. The box's placeholder repeated its own
      /// caption ("A domain, or an account address"), which said nothing more.
      /// </summary>
      public static string TargetExample(bool account) => account ? "user@example.com" : "example.com"; // no-loc: examples, the same in every language
   }
}
