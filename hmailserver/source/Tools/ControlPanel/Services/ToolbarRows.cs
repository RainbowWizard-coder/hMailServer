// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// Whether a list page's toolbar puts its filters on a row of their own under
   /// the search box, and its actions on a row of their own under those, rather
   /// than in the width the parts before them leave (Views/Scaffold/Toolbar). Plain
   /// numbers, so that a test holds the rule.
   ///
   /// The actions are a list page's verbs - Edit, Delete, Add… - since the list
   /// page pattern of 10 October 2026 (review finding F288), and beside a search
   /// box and a filter as wide as the suppression list's sending domain they had
   /// no width left in the narrowest window and were not drawn at all, which is why
   /// that page had put them in its header. They stay beside the search while all of
   /// them fit there on one line; once they do not, all of them go under, on a row of
   /// their own at the toolbar's right edge, the Primary last. Beside the search they
   /// wrapped while the widest fitted, which stacked the IP ranges' three in a column
   /// at 760 and left the delivery queue's Primary alone on a second line, at the left,
   /// in German at 1280 (the final check of Tier 2). Actions that cannot wrap - a
   /// StackPanel of them - are measured as one, as before. And where the search box and the
   /// filters are wider than the toolbar together - the German suppression list's
   /// "Sendende Domain" beside its box in the narrowest window - the filters go
   /// under the search, from the left, and the actions beside the search if they
   /// fit there.
   /// </summary>
   public static class ToolbarRows
   {
      /// <summary>The gap the toolbar keeps between its parts (DesignTokens.Space.Md), and between its two rows.</summary>
      public const double Gap = DesignTokens.Space.Md;

      /// <summary>
      /// True when the actions, on one line, do not fit beside the search box and the
      /// filters: <paramref name="searchWidth"/> and <paramref name="filtersWidth"/>
      /// are what those parts take, each with its own margin, and nothing for a part
      /// the toolbar does not show; <paramref name="actionsWidth"/> is every action
      /// shown, side by side with its margins.
      /// </summary>
      public static bool ActionsUnder(double toolbarWidth, double searchWidth, double filtersWidth, double actionsWidth)
      {
         if (actionsWidth <= 0 || (searchWidth <= 0 && filtersWidth <= 0))
            return false;

         return toolbarWidth - searchWidth - filtersWidth - Gap < actionsWidth;
      }

      /// <summary>
      /// True when the filters do not fit beside the search box: <paramref name="filtersWidth"/>
      /// with its margin, which is the gap between the two.
      /// </summary>
      public static bool FiltersUnder(double toolbarWidth, double searchWidth, double filtersWidth)
         => searchWidth > 0 && filtersWidth > 0 && toolbarWidth - searchWidth < filtersWidth;
   }
}
