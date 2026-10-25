// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Collections.Generic;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// The configuration checks, read once for the two pages that show them: the
   /// Server status page, as notices under its header, and the Dashboard, at the
   /// top of its attention card (review finding F242) - the same code, so that the
   /// two cannot disagree, as the setup checklist and the Dashboard share
   /// ExternalSetupChecks. Each check names the page that puts it right.
   ///
   /// Each warning carries its level as a <see cref="StatusLevel"/>, never as a
   /// word: a word passed through the catalogue and matched back against the
   /// English demoted the open relay in German (review finding F163).
   ///
   /// The judging is here, apart from the reading of the server's settings over COM
   /// (ConfigurationChecks.Read.cs), so that ConfigurationChecksTests can hold it to
   /// the ranges it is given.
   /// </summary>
   public static partial class ConfigurationChecks
   {
      /// <summary>
      /// One IP range as the checks read it: its name and ends, whether it expires, and
      /// the two relay directions with whether each needs SMTP authentication.
      /// </summary>
      public sealed record RangeFacts(
         string Name,
         string LowerIP,
         string UpperIP,
         bool Expires,
         bool LocalToExternal,
         bool LocalNeedsAuth,
         bool ExternalToExternal,
         bool ExternalNeedsAuth);

      /// <summary>
      /// The warnings the server's settings and IP ranges give, in the order they are
      /// checked, and how many of the ranges are the server's auto-bans - a statistic,
      /// not a warning. A ban is what the Auto-ban page's Banned now card and the IP
      /// ranges page's filter count as one, and what the server lifts and mirrors into
      /// the firewall: an expiring range named "Auto-ban: ..." (AutoBanList.IsBan). Every
      /// expiring range was counted, and an expiring range of 127.0.0.1 was "Localhost is
      /// currently banned", so that an administrator's own range that expires - a partner
      /// let in until Friday - counted on Server status as a ban the Auto-ban page did not
      /// list (release review finding CP-7).
      /// </summary>
      public static IReadOnlyList<ConfigurationWarning> Judge(string hostName, bool denyMailFromNull, IEnumerable<RangeFacts> ranges, out int autobanRanges)
      {
         var found = new Found();
         int autoban = 0;

         if (string.IsNullOrEmpty(hostName))
            // The setup checklist's host-name item holds it too, as a step: the
            // Dashboard lists the step alone while it is open (DashboardAttention).
            found.AddWarning(StatusLevel.Warning, L("No host name is set: 'Host name (HELO/EHLO greeting)' on the Protocols page's SMTP tab is empty."), "protocols", DashboardAttention.HostNameKey);

         if (denyMailFromNull)
            found.AddWarning(StatusLevel.Warning, L("Mail from an empty sender address is denied. Many servers send bounces from <>, which will be rejected."), "protocols");

         foreach (RangeFacts range in ranges ?? new List<RangeFacts>())
         {
            // An open relay as the IP ranges page counts one (Services/RelayExposure,
            // review finding F229): either direction to an external address open
            // without authentication, on a range that reaches the Internet. One
            // address - My computer relays without it as shipped - and a private
            // network's range are the page's to mark, not a warning here.
            if (RelayExposure.Of(range.LowerIP, range.UpperIP, range.LocalToExternal, range.LocalNeedsAuth, range.ExternalToExternal, range.ExternalNeedsAuth) == RelayExposure.Reach.Internet)
            {
               if ((RelayExposure.OpenDirections(range.LocalToExternal, range.LocalNeedsAuth, range.ExternalToExternal, range.ExternalNeedsAuth) & RelayExposure.Open.ExternalToExternal) != 0)
                  found.AddWarning(StatusLevel.Critical, F("IP range '{0}' allows external-to-external delivery without authentication (open relay risk).", range.Name), "ipranges");
               else
                  found.AddWarning(StatusLevel.Critical, F("IP range '{0}' allows local-to-external delivery without authentication, so anyone in it who gives one of this server's domains as the sender can relay mail through this server (open relay risk).", range.Name), "ipranges");
            }

            bool ban = AutoBanList.IsBan(range.Name, range.Expires);
            if (ban && range.LowerIP == "127.0.0.1" && range.UpperIP == "127.0.0.1")
               found.AddWarning(StatusLevel.Warning, L("Localhost is currently banned in the IP ranges."), "ipranges");

            if (ban)
               autoban++;
         }

         autobanRanges = autoban;
         return found;
      }

      /// <summary>
      /// The warnings as they are found, each with its level, the page that puts it
      /// right and the subject it shares with an item of the setup checklist, if any.
      /// </summary>
      private sealed class Found : List<ConfigurationWarning>
      {
         public void AddWarning(StatusLevel level, string text, string page, string key = null)
            => Add(new ConfigurationWarning { Level = level, Text = text, Page = page, Key = key });
      }
   }
}
