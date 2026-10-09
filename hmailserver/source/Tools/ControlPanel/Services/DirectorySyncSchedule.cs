// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// Whether the directory synchronisation schedule in hMailServer.INI waits for the
   /// server to start again (review findings F218 and F120). The server registers its
   /// scheduled synchronisation once, as it builds its scheduler - at a service start,
   /// and again at a reinitialise (Application::CreateScheduledTasks_) - and both set
   /// the start time its status reports. What it registers is not [LDAP]
   /// SyncScheduleMinutes alone: DirectorySyncScheduleTask::ScheduleMinutes() answers 0,
   /// and no task is registered, while [LDAP] Enabled is 0, and LdapSettings reads the
   /// minutes with GetPrivateProfileInt and clamps them, 1 to 14 to 15 and anything over
   /// a week to a week. A task once registered fires at its interval and runs only while
   /// directory access is on, which it reads again at every tick
   /// (DirectorySyncScheduleTask::DoWork).
   ///
   /// So the schedule waits while directory access is on and the interval the file
   /// gives is not the one the running server registered - none, when it started with
   /// directory access off. While directory access is off nothing runs, as the file
   /// says, whatever either interval is: a schedule changed then waits for nothing,
   /// and switching directory access on is what makes a changed interval wait. The
   /// minutes compared are the server's interval, not the text: 5 and 15 are one
   /// schedule, and so are an empty value and 0. It compared the minutes as text and
   /// nothing else, so with directory access off a changed schedule said Restart to
   /// apply over a restart that registers nothing, and switching it on from the
   /// Directory authentication page, after a start with it off, said no restart was
   /// needed while no synchronisation would run until one.
   ///
   /// Measured from the start, and not from the schedule as last saved: a comparison
   /// with the last save moved with every save, so a second save after a schedule
   /// change, with no restart between, said "no service restart is needed" over a
   /// change that still waited. Nor from the schedule as a page last read it, since a
   /// page is read again on every visit, and a visit is not a restart. One record for
   /// the process (<see cref="Shared"/>), which the Directory synchronisation and
   /// Directory authentication pages read and save through, since each writes half of
   /// what the server registers; this part is WPF- and COM-free, so that a test can
   /// hold it to the sequences the pages go through. The part that asks the server
   /// when it started and reads the file is DirectorySyncSchedule.Session.cs.
   /// </summary>
   public sealed partial class DirectorySyncSchedule
   {
      /// <summary>The record the directory pages keep, for as long as the Control Panel runs.</summary>
      public static DirectorySyncSchedule Shared { get; } = new();

      /// <summary>The fewest minutes between two runs the server takes (LdapSettings.cpp, kMinSyncScheduleMinutes).</summary>
      public const int FewestMinutes = 15;

      /// <summary>The most minutes between two runs the server takes, a week (LdapSettings.cpp, kMaxSyncScheduleMinutes).</summary>
      public const int MostMinutes = 7 * 24 * 60;

      // When the server last started, as its status said it, that registered_ belongs
      // to; null while it could not say.
      private string started_;

      // The interval the running server registered, as far as this record knows - 0
      // for none; null before the first read.
      private int? registered_;

      // [LDAP] Enabled and the interval [LDAP] SyncScheduleMinutes gives, as this
      // Control Panel last read or wrote the file; the interval is null before the
      // first read.
      private bool enabledInFile_;
      private int? intervalInFile_;

      /// <summary>
      /// The minutes between two runs the server takes from [LDAP] SyncScheduleMinutes
      /// as GetPrivateProfileInt reads it, 0 for none: 0 is none - an empty value, a
      /// word and a missing key read as 0 - 1 to 14 is 15, and anything over a week is a
      /// week (LdapSettings::Load_).
      /// </summary>
      public static int Interval(uint minutes)
         => minutes == 0 ? 0
            : minutes < FewestMinutes ? FewestMinutes
            : minutes > MostMinutes ? MostMinutes
            : (int)minutes;

      /// <summary>
      /// After a page read the file: <paramref name="enabled"/> is whether [LDAP] Enabled
      /// reads as other than 0, and <paramref name="minutes"/> [LDAP] SyncScheduleMinutes,
      /// both as GetPrivateProfileInt reads them. <paramref name="started"/> is when the
      /// server last started or reinitialised (Status.StartTime), or null when it could
      /// not say; a start other than the one on record registered what the file holds
      /// now.
      /// </summary>
      public void Read(string started, bool enabled, uint minutes)
      {
         Observe_(started, enabled, Interval(minutes));
         enabledInFile_ = enabled;
         intervalInFile_ = Interval(minutes);
      }

      /// <summary>
      /// After a save wrote the file, which now holds <paramref name="enabled"/> and
      /// <paramref name="minutes"/> as <see cref="Read"/> takes them: whether the
      /// schedule waits for the server to start again. A start since the last read or
      /// save registered what the file held before this save.
      /// </summary>
      public bool Saved(string started, bool enabled, uint minutes)
      {
         Observe_(started, enabledInFile_, intervalInFile_);
         enabledInFile_ = enabled;
         intervalInFile_ = Interval(minutes);
         return Waits;
      }

      /// <summary>
      /// Whether the schedule in the file is not the one the running server keeps:
      /// directory access is on, and the interval the file gives is not the one the
      /// server registered as it started.
      /// </summary>
      public bool Waits => registered_ != null && intervalInFile_ != null && enabledInFile_ && intervalInFile_ != registered_;

      private void Observe_(string started, bool enabledThen, int? intervalThen)
      {
         started = string.IsNullOrWhiteSpace(started) ? null : started.Trim();

         // What a start registers from the file: its interval while directory access
         // is on, and nothing while it is off.
         int? registers = intervalThen == null ? null : enabledThen ? intervalThen : 0;

         if (registered_ == null)
         {
            registered_ = registers;
            started_ = started;
            return;
         }

         // A start the server could not report clears nothing: the wait is kept rather
         // than a change still waiting called applied. A start reported for the first
         // time is taken to be the run already on record, for the same reason.
         if (started == null)
            return;
         if (started_ == null)
         {
            started_ = started;
            return;
         }

         if (!string.Equals(started, started_, StringComparison.Ordinal))
         {
            started_ = started;
            registered_ = registers;
         }
      }
   }
}
