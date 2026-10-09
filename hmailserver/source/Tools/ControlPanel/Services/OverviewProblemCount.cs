// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// The count beside the title of the transport encryption, spam filtering and
   /// virus scanning overviews, as a level and its words: how many problems were
   /// found, at the worst one's level; that none was, where every value was read;
   /// and nothing at all where a value could not be read and no problem was found.
   ///
   /// The overviews leave out every note that follows from a value they could not
   /// read, so a partial read finds no problem in what it could not see, and the
   /// count said "No problems found", in green, beside "Some values could not be
   /// read: ... The sections below may be incomplete." and "Where this server
   /// stands cannot be said: the listeners could not be read." - the claim that
   /// nothing that follows from a value not read is said, broken by the one word
   /// at the top of the page. A problem that was found is counted whatever else
   /// could not be read: it is there.
   ///
   /// Free of WPF, so that it can be tested (OverviewNotices draws it).
   /// </summary>
   public static class OverviewProblemCount
   {
      /// <summary>
      /// The count's level and words, or null where none is said: no problem
      /// found while <paramref name="everythingRead"/> is false.
      /// </summary>
      public static (StatusLevel Level, string Text)? Of(int problems, StatusLevel worst, bool everythingRead)
      {
         if (problems == 0)
            return everythingRead ? (StatusLevel.Good, L("No problems found")) : null;

         return (worst, Plural(problems, "{0} problem", "{0} problems"));
      }
   }
}
