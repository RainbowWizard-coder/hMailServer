// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Globalization;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// The Allowed and blocked domains page's words for an entry, and the request
   /// that changes one (review finding F266).
   ///
   /// The Action column spent about 250 pixels on "Allowed: not greylisted, not
   /// refused on score" in every row, which cut the Added column to "2026-" and
   /// the note to "Always accept, whatev" in a 1280 window: the column says
   /// Allowed or Blocked, and what each does is the cell's tool tip and the row's
   /// words to a screen reader. Added is the reader's short date and time, the
   /// server's clock (the owner's decision 13). And an entry could only be
   /// changed by removing it and adding it again, which gave it today's date and
   /// a new id: the server changes an entry in place over the REST API's PUT
   /// /api/v1/domain-policy-list/{id} (RestApiDomainPolicyList.cpp), the one
   /// interface that has the change - AntiSpam over COM adds and removes.
   ///
   /// No WPF here, so the tests hold the words and the request.
   /// </summary>
   public static class DomainPolicyWords
   {
      /// <summary>The Action column's word for the server's action, white or black.</summary>
      public static string Action(string action) => action == "black" ? L("Blocked") : L("Allowed");

      /// <summary>What the action does, for the Action cell's tool tip and the row's words.</summary>
      public static string ActionTip(string action)
         => action == "black" ? L("Blocked: refused at MAIL FROM") : L("Allowed: not greylisted, not refused on score");

      /// <summary>
      /// When the entry was added, as the server wrote it (yyyy-MM-dd HH:mm:ss, its
      /// own clock) in the reader's short date and time; the text as it came where
      /// it is not such a time, and nothing for nothing.
      /// </summary>
      public static string Added(string created)
      {
         if (string.IsNullOrWhiteSpace(created))
            return "";

         return DateTime.TryParseExact(created.Trim(), "yyyy-MM-dd HH:mm:ss", CultureInfo.InvariantCulture, DateTimeStyles.None, out DateTime at) // no-loc: the server's format
            ? at.ToString("g", CultureInfo.CurrentCulture)
            : created.Trim();
      }

      /// <summary>The Matched on list's value for the server's word: 0 the envelope sender's domain, 1 the HELO name, 2 either.</summary>
      public static int MatchOnChoice(string matchOn) => matchOn switch
      {
         "envelope" => 0,
         "helo" => 1,
         _ => 2
      };

      /// <summary>The server's word for the Matched on list's value.</summary>
      public static string MatchOnWord(int choice) => choice switch
      {
         0 => "envelope",
         1 => "helo",
         _ => "both"
      };

      /// <summary>The Action list's value for the server's word: 0 allowed, 1 blocked.</summary>
      public static int ActionChoice(string action) => action == "black" ? 1 : 0;

      /// <summary>The server's word for the Action list's value.</summary>
      public static string ActionWord(int choice) => choice == 1 ? "black" : "white";

      /// <summary>The REST API's address of one entry.</summary>
      public static string EntryPath(int id) => "api/v1/domain-policy-list/" + id.ToString(CultureInfo.InvariantCulture); // no-loc

      /// <summary>
      /// The body of the PUT that changes an entry: all four of its fields, so that
      /// the entry is what the dialog shows. The server refuses the whole body or
      /// takes it, and keeps the entry's id and the date it was added.
      /// </summary>
      public static string ChangeBody(string pattern, string matchOn, string action, string note)
         => RestSettings.Body(new[]
         {
            new KeyValuePair<string, object>("pattern", pattern ?? ""), // no-loc
            new KeyValuePair<string, object>("match_on", matchOn ?? "both"), // no-loc
            new KeyValuePair<string, object>("action", action ?? "white"), // no-loc
            new KeyValuePair<string, object>("note", note ?? "") // no-loc
         });
   }
}
