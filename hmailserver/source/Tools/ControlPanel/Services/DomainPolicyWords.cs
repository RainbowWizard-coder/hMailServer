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
         return string.IsNullOrWhiteSpace(created) ? "" : ServerTimeText.Cell(created);
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
      /// Why an entry cannot be changed in place from here, in this page's words,
      /// from what the REST client's Open refused with: the REST API off, or bound
      /// to a loopback address of the server while this Control Panel runs on
      /// another machine, each with what to do; otherwise the client's own
      /// sentence and that the entry can be deleted and added again meanwhile.
      /// The sentences RestSettings.Endpoint gives those two are the settings
      /// pages' - "these settings are reached over it: the server keeps them, but
      /// no COM property carries them", "Change these settings in the Control
      /// Deck" - which said of this list, which COM does carry, what is not so
      /// (review finding F266, found re-checking a0ceb4014). Checked in Open's
      /// order: the password first, then the listener, by its rules there.
      /// </summary>
      public static string Unchangeable(bool hasPassword, string port, string bindAddress, bool localSession, string refusal)
      {
         if (hasPassword)
         {
            string portText = (port ?? "").Trim();
            if (!int.TryParse(portText, NumberStyles.Integer, CultureInfo.InvariantCulture, out int number) || number <= 0 || number > 65535)
               return F("An entry is changed in place over the REST API, which is off (its port on the API & monitoring page is {0}). Give it a port there and reinitialise the server, or delete the entry and add it again.",
                  portText.Length == 0 ? "0" : portText);

            string bind = string.IsNullOrWhiteSpace(bindAddress) ? "127.0.0.1" : bindAddress.Trim(); // no-loc
            bool loopback = bind == "127.0.0.1" || bind == "::1" || bind.Equals("localhost", StringComparison.OrdinalIgnoreCase); // no-loc
            if (!localSession && loopback)
               return F("An entry is changed in place over the REST API, which listens on {0} only, out of reach of a Control Panel on another machine. Change the entry in the Control Deck on the server, or delete it and add it again.", bind);
         }

         return Sentences(refusal, L("An entry is changed in place over the REST API; until it can be used from here, delete the entry and add it again instead."));
      }

      /// <summary>
      /// The one-time code asked for and not given, in this page's words: before
      /// the dialog opens, Edit… asks again; in the dialog, Save does.
      /// </summary>
      public static string CodeNotGiven(bool saving)
         => saving
            ? L("Nothing was saved: the REST API asks for the administrator's one-time code before it answers, and none was given. Press Save to be asked again.")
            : L("An entry is changed in place over the REST API, which asks for the administrator's one-time code before it answers, and none was given. Press Edit… to be asked again, or delete the entry and add it again.");

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
