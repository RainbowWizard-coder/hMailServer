// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Linq;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// One searchable setting: what it is called on screen, the underlying
   /// hMailServer.INI key or COM property path, the navigation page that hosts
   /// it, and where on that page it is - the tab, and the card.
   /// </summary>
   public sealed class SettingEntry
   {
      public SettingEntry(string label, string key, string page, string tab = null, string card = null)
      {
         Label = label;
         Key = key;
         Page = page;
         Tab = string.IsNullOrEmpty(tab) ? null : tab;
         Card = string.IsNullOrEmpty(card) ? null : card;
      }

      public string Label { get; }
      public string Key { get; }
      public string Page { get; }

      /// <summary>
      /// The header of the page's tab that holds the setting, as the page's source
      /// writes it (a catalogue key), or null on a page with no tabs. A setting
      /// found by search opens on its tab, where it opened the page on its first
      /// (review finding F274).
      /// </summary>
      public string Tab { get; }

      /// <summary>The title of the card that holds the setting (a catalogue key), or null where none is known.</summary>
      public string Card { get; }

      /// <summary>
      /// Where on its page the setting is, in the reader's language: the tab on a
      /// page of tabs, and the card on a page of one scroll; null where neither is
      /// known.
      /// </summary>
      public string Within => Tab != null ? L(Tab) : Card != null ? L(Card) : null;
   }

   /// <summary>
   /// A setting that matched a search, with everything a caller needs to rank it
   /// against results from other sources and to tell the user where it lives.
   /// </summary>
   public sealed class SettingMatch
   {
      internal SettingMatch(SettingEntry entry, int rank)
      {
         Entry = entry;
         Rank = rank;
      }

      /// <summary>The indexed setting.</summary>
      public SettingEntry Entry { get; }

      /// <summary>Match score, lower being better. See <see cref="SearchTerms"/>.</summary>
      public int Rank { get; }

      public string Label => Entry.Label;
      public string Key => Entry.Key;
      public string Page => Entry.Page;

      /// <summary>
      /// The full navigation trail to the page hosting the setting, and on to its
      /// tab or card - "Sign-in &amp; access &gt; Auto-ban &gt; Countries" - so that a
      /// result can say where it will take the reader before they go there, which
      /// "Sign-in &amp; access &gt; Auto-ban" did not for a setting three tabs in
      /// (review finding F274). The page's trail is resolved through
      /// <see cref="NavigationMap"/> rather than stored, so a page that moves in
      /// the navigation cannot leave stale trails behind in the generated index.
      /// </summary>
      public string Location
      {
         get
         {
            string page = NavigationMap.LocationOf(Page);
            string within = Entry.Within;
            return string.IsNullOrEmpty(within) || page.Length == 0 ? page : page + " > " + within;
         }
      }
   }

   /// <summary>
   /// Lets the command palette find an individual setting rather than only a
   /// page name. Settings are spread across pages by topic, and a setting whose
   /// page an administrator does not guess is effectively invisible - searching
   /// for the setting itself removes that failure mode, and keeps removing it
   /// as settings are added.
   ///
   /// The entries are generated from the page definitions by
   /// build/generate-settings-index.ps1 (see SettingsSearchIndex.g.cs), so the
   /// index cannot drift away from what the pages actually show.
   ///
   /// Ranking is delegated to <see cref="SearchQuery"/> rather than done here.
   /// It used to be a private prefix/substring ladder, which meant the palette
   /// scored settings by one rule and page names by another; a single list whose
   /// rows were ranked by two incomparable scales put results in an order that
   /// could not be reasoned about. Page titles for results come from
   /// <see cref="NavigationMap"/> for the same reason - a second hand-maintained
   /// table of page titles drifts from the navigation the moment either changes.
   /// </summary>
   public static partial class SettingsSearchIndex
   {
      /// <summary>
      /// Added to a match found only in the INI key or COM path, so that a
      /// setting matched by the words on screen outranks one matched by an
      /// identifier. Somebody typing "delete logs" means the label; somebody
      /// typing "LogDeleteDays" gets an exact key match, which still wins.
      /// </summary>
      private const int KeyPenalty = 20;

      /// <summary>
      /// Search results, most relevant first. Each result carries the page the
      /// caller should navigate to.
      /// </summary>
      public static IEnumerable<SettingMatch> Search(string query) => Search(new SearchQuery(query));

      /// <summary>
      /// Overload for a caller that is already searching several sources with
      /// one query and should not pay to normalise it again per source.
      /// </summary>
      public static IEnumerable<SettingMatch> Search(SearchQuery query)
      {
         if (query == null || query.IsEmpty)
            return Array.Empty<SettingMatch>();

         var matches = new List<SettingMatch>();

         foreach (SettingEntry entry in Entries)
         {
            int byLabel = query.Score(L(entry.Label));
            int byKey = query.Score(entry.Key);
            if (byKey != SearchTerms.NoMatch)
               byKey += KeyPenalty;

            int rank = Math.Min(byLabel, byKey);
            if (rank == SearchTerms.NoMatch)
               continue;

            matches.Add(new SettingMatch(entry, rank));
         }

         // Label as the tie-break rather than the key: two settings that score
         // the same are told apart by the reader on their wording, so listing
         // them in wording order is what makes the list scannable.
         return matches
            .OrderBy(m => m.Rank)
            .ThenBy(m => m.Label, StringComparer.OrdinalIgnoreCase)
            .ThenBy(m => m.Key, StringComparer.OrdinalIgnoreCase)
            .ToList();
      }
   }
}
