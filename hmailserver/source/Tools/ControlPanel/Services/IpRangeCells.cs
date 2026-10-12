// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// The IP ranges page's Expires and Relay cells (review finding F231), with no WPF
   /// so that BannedNowTests holds them. The page drew the server's bans - ranges
   /// that end on their own - as the permanent ranges are drawn, and whether a range
   /// lets mail go on to an external address, with or without a sign-in, was on the
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
      /// Whether the range relays - sends mail on to an external address, from a
      /// sender of this server's domains (Local to external) or any other (External
      /// to external) - and on what terms, as RelayExposure counts it for the range's
      /// permissions card (review finding F229): "No" where neither is allowed;
      /// "After sign-in" where each allowed needs authentication; "Without sign-in"
      /// where one need not, for one address, such as the shipped "My computer"
      /// (127.0.0.1), which only that machine can send through, unmarked, and for a
      /// private network's range, which every machine of it can, marked Warning; and
      /// "Open relay" for any other, marked Critical, as the card's notice and Server
      /// status's open-relay check mark it. <c>Level</c> is the mark, Normal for none.
      /// </summary>
      public static (string Word, StatusLevel Level) Relay(string lower, string upper, bool allowLocalToExternal, bool requireAuthenticationLocalToExternal,
                                                           bool allowExternalToExternal, bool requireAuthenticationExternalToExternal)
      {
         if (!allowLocalToExternal && !allowExternalToExternal)
            return (L("No"), StatusLevel.Normal);

         return RelayExposure.Of(lower, upper, allowLocalToExternal, requireAuthenticationLocalToExternal, allowExternalToExternal, requireAuthenticationExternalToExternal) switch
         {
            RelayExposure.Reach.Internet => (L("Open relay"), StatusLevel.Critical),
            RelayExposure.Reach.Network => (L("Without sign-in"), StatusLevel.Warning),
            _ when RelayExposure.OpenDirections(allowLocalToExternal, requireAuthenticationLocalToExternal, allowExternalToExternal, requireAuthenticationExternalToExternal) == RelayExposure.Open.None
               => (L("After sign-in"), StatusLevel.Normal),
            _ => (L("Without sign-in"), StatusLevel.Normal)
         };
      }
   }
}
