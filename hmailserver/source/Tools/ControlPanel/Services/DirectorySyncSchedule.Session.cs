// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// The part of the schedule record that asks the server when it started and reads
   /// hMailServer.INI, which the Directory synchronisation and Directory authentication
   /// pages share; the rule itself is DirectorySyncSchedule.cs, which the tests compile.
   /// </summary>
   public sealed partial class DirectorySyncSchedule
   {
      private const string Section = "LDAP";   // no-loc

      /// <summary>
      /// After a page read the file at <paramref name="iniPath"/>: see <see cref="Read"/>.
      /// Nothing is read while the session is to a server on another computer, whose
      /// schedule is in its own file and not this computer's (IniFeatureStore.
      /// ServerIsOnThisComputer; the directory pages read neither then).
      /// </summary>
      public void ReadFile(string iniPath)
      {
         if (IniFeatureStore.ServerIsOnThisComputer)
            Read(ServerStarted(), EnabledIn_(iniPath), MinutesIn_(iniPath));
      }

      /// <summary>
      /// After a page wrote the file at <paramref name="iniPath"/>: whether the schedule
      /// it holds now waits for the server to start again (see <see cref="Saved"/>). No,
      /// for a file that is not the server's, which the pages write no more.
      /// </summary>
      public bool SavedFile(string iniPath)
         => IniFeatureStore.ServerIsOnThisComputer && Saved(ServerStarted(), EnabledIn_(iniPath), MinutesIn_(iniPath));

      /// <summary>
      /// When the server last started or reinitialised - the moment it registered the
      /// schedule - as its status reports it, or null when it cannot say.
      /// </summary>
      public static string ServerStarted()
      {
         dynamic status = null;
         try
         {
            ServerSession session = ServerSession.Current;
            if (session == null)
               return null;

            status = session.Application.Status;
            return (string)status.StartTime;
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            return null;
         }
         finally
         {
            ServerSession.Release((object)status);
         }
      }

      // Both read as LdapSettings::Load_ reads them, with GetPrivateProfileInt and 0 for
      // a missing key: Enabled is on when it reads as anything but 0, and the minutes are
      // the API's reading, which takes 0x10 as 16, -1 as 4294967295 and a word as 0.
      private static bool EnabledIn_(string iniPath) => ProfileApi.ReadInt(Section, "Enabled", 0, iniPath) != 0;   // no-loc

      private static uint MinutesIn_(string iniPath) => ProfileApi.ReadInt(Section, "SyncScheduleMinutes", 0, iniPath);   // no-loc
   }
}
