// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Collections.Generic;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>The configuration checks' read of the server's settings and IP ranges over COM; the judging is ConfigurationChecks.cs's.</summary>
   public static partial class ConfigurationChecks
   {
      /// <summary>
      /// The warnings the server's settings and IP ranges give, in the order they
      /// are checked, and how many IP ranges are the server's auto-bans, which are a
      /// statistic and not a warning (<see cref="Judge"/>). Throws when the settings
      /// cannot be read: the caller says the checks did not run, which is neither "no
      /// warnings" nor a finding.
      /// </summary>
      public static IReadOnlyList<ConfigurationWarning> Read(dynamic app, out int autobanRanges)
      {
         dynamic settings = app.Settings;
         try
         {
            string hostName = (string)settings.HostName;
            bool denyMailFromNull = (bool)settings.DenyMailFromNull;

            var facts = new List<RangeFacts>();
            dynamic ranges = settings.SecurityRanges;
            try
            {
               int rangeCount = (int)ranges.Count;
               for (int i = 0; i < rangeCount; i++)
               {
                  dynamic range = ranges.Item[i];
                  try
                  {
                     facts.Add(new RangeFacts(
                        (string)range.Name,
                        (string)range.LowerIP,
                        (string)range.UpperIP,
                        (bool)range.Expires,
                        (bool)range.AllowDeliveryFromLocalToRemote,
                        (bool)range.RequireSMTPAuthLocalToExternal,
                        (bool)range.AllowDeliveryFromRemoteToRemote,
                        (bool)range.RequireSMTPAuthExternalToExternal));
                  }
                  finally
                  {
                     ServerSession.Release(range);
                  }
               }
            }
            finally
            {
               ServerSession.Release(ranges);
            }

            return Judge(hostName, denyMailFromNull, facts, out autobanRanges);
         }
         finally
         {
            ServerSession.Release(settings);
         }
      }
   }
}
