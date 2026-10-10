// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// What a list page's count and its empty state say while its search filters
   /// the rows it read (docs/ControlPanelDesign.md, List pages; review findings
   /// F288 and F210): while a search is typed in over a list that has entries,
   /// the pill beside the title says how many it shows of how many there are,
   /// "Shown: 2 of 40", and the list's own count again once the search is
   /// cleared; where the search hides every entry, the empty state says that no
   /// entry has what was typed, and leaves out the action that adds a first
   /// entry to a list that has entries. The routes page, among others, said "2
   /// routes" beside an empty card while a search hid both.
   ///
   /// Plain counts and texts, so that a test holds the rule; Services/ListSearchState
   /// applies it to a XAML list page's pill and empty state, and the pages that
   /// filter their rows themselves - the allowed and blocked domains, the public
   /// folders, the quarantine - ask it for the same words.
   /// </summary>
   public static class ListSearchCount
   {
      /// <summary>Whether a search is typed in over a list that has entries, which the pill then counts.</summary>
      public static bool Searching(string query, int total)
         => total > 0 && !string.IsNullOrWhiteSpace(query);

      /// <summary>
      /// The pill's text: "Shown: <paramref name="shown"/> of <paramref name="total"/>"
      /// while a search is typed in, and otherwise <paramref name="whole"/>, the
      /// list's own count ("4 routes").
      /// </summary>
      public static string PillText(string query, int shown, int total, string whole)
         => Searching(query, total) ? F("Shown: {0} of {1}", shown, total) : whole;

      /// <summary>
      /// The pill's text while a filter of the page's own narrows the list - the IP
      /// ranges page's Show auto-bans only: what is shown of the whole list's number,
      /// as while a search is typed in, and the list's own count where it is empty.
      /// </summary>
      public static string NarrowedPillText(int shown, int total, string whole)
         => total > 0 ? F("Shown: {0} of {1}", shown, total) : whole;

      /// <summary>Whether the search hides every entry of a list that has some: the empty state then says so.</summary>
      public static bool NothingMatches(string query, int shown, int total)
         => Searching(query, total) && shown == 0;

      /// <summary>The empty state's sentence for a search that hides every entry: "No entry in this list has "x" in it."</summary>
      public static string NothingMatchesText(string query)
         => F("No entry in this list has \"{0}\" in it.", query?.Trim() ?? "");
   }
}
