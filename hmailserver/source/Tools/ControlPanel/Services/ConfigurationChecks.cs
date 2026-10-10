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
   /// </summary>
   public static class ConfigurationChecks
   {
      /// <summary>
      /// The warnings the server's settings and IP ranges give, in the order they
      /// are checked, and how many IP ranges expire - the auto-ban ranges, which
      /// are a statistic and not a warning. Throws when the settings cannot be read:
      /// the caller says the checks did not run, which is neither "no warnings" nor
      /// a finding.
      /// </summary>
      public static IReadOnlyList<ConfigurationWarning> Read(dynamic app, out int autobanRanges)
      {
         var found = new Found();
         int autoban = 0;

         dynamic settings = app.Settings;
         try
         {
            if (((string)settings.HostName).Length == 0)
               // The setup checklist's host-name item holds it too, as a step: the
               // Dashboard lists the step alone while it is open (DashboardAttention).
               found.AddWarning(StatusLevel.Warning, L("No host name is set: 'Host name (HELO/EHLO greeting)' on the Protocols page's SMTP tab is empty."), "protocols", DashboardAttention.HostNameKey);

            if ((bool)settings.DenyMailFromNull)
               found.AddWarning(StatusLevel.Warning, L("Mail from an empty sender address is denied. Many servers send bounces from <>, which will be rejected."), "protocols");

            dynamic ranges = settings.SecurityRanges;
            int rangeCount = (int)ranges.Count;
            for (int i = 0; i < rangeCount; i++)
            {
               dynamic range = ranges.Item[i];
               try
               {
                  if ((bool)range.AllowDeliveryFromRemoteToRemote && !(bool)range.RequireSMTPAuthExternalToExternal)
                     found.AddWarning(StatusLevel.Critical, F("IP range '{0}' allows external-to-external delivery without authentication (open relay risk).", (string)range.Name), "ipranges");

                  if ((string)range.LowerIP == "127.0.0.1" && (string)range.UpperIP == "127.0.0.1" && (bool)range.Expires)
                     found.AddWarning(StatusLevel.Warning, L("Localhost is currently banned in the IP ranges."), "ipranges");

                  if ((bool)range.Expires)
                     autoban++;
               }
               finally
               {
                  ServerSession.Release(range);
               }
            }
            ServerSession.Release(ranges);
         }
         finally
         {
            ServerSession.Release(settings);
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
