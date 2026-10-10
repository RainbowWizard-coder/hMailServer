// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// The IP ranges page's Expires and Relay cells (review finding F231), with no WPF
   /// so that IpRangeCellsTests holds them. The page drew the server's bans - ranges
   /// that end on their own - as the permanent ranges are drawn, and whether a range
   /// lets mail from outside go on to outside, with or without a sign-in, was on the
   /// permissions card of the one range selected.
   /// </summary>
   public static class IpRangeCells
   {
      /// <summary>
      /// When the range ends: Never for a permanent range, and for one that expires
      /// what the Auto-ban page's Banned now says of a ban - "23 min from now", or the
      /// server's time where its clock is not this computer's.
      /// </summary>
      public static string Expires(bool expires, DateTime expiresAt, DateTime now, bool sameClock)
         => expires ? AutoBanList.Ends(expiresAt, now, sameClock) : L("Never");

      /// <summary>
      /// Whether the range relays - delivers mail from an external sender to an
      /// external recipient - and on what terms: "No"; "After sign-in" where the
      /// sender must authenticate first; and, where it need not, "Open relay" for a
      /// range of more than one address, which anyone in it can send through, and
      /// "Without sign-in" for one address, such as the shipped "My computer"
      /// (127.0.0.1), which only that machine can. <c>Open</c> is the first of the
      /// three, which the page marks Critical, as Server status's open-relay check is.
      /// </summary>
      public static (string Word, bool Open) Relay(bool remoteToRemote, bool requiresAuth, string lower, string upper)
      {
         if (!remoteToRemote)
            return (L("No"), false);

         if (requiresAuth)
            return (L("After sign-in"), false);

         bool oneAddress = string.Equals((lower ?? "").Trim(), (upper ?? "").Trim(), StringComparison.OrdinalIgnoreCase);
         return oneAddress ? (L("Without sign-in"), false) : (L("Open relay"), true);
      }
   }
}
