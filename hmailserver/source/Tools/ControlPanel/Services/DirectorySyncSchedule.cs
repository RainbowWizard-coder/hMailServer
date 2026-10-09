// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// Whether the directory synchronisation schedule in hMailServer.INI waits for the
   /// server to start again (review findings F218 and F120). The server reads [LDAP]
   /// SyncScheduleMinutes once, as it builds its scheduler - at a service start, and
   /// again at a reinitialise (Server/Common/LDAP/DirectorySyncTask.h,
   /// Application::CreateScheduledTasks_) - and both set the start time its status
   /// reports. So the schedule in the file waits for as long as it differs from the one
   /// the file held when the server last started, as far as this Control Panel saw the
   /// file.
   ///
   /// Measured from that, and not from the schedule as last saved: a comparison with
   /// the last save moved with every save, so a second save after a schedule change,
   /// with no restart between, said "no service restart is needed" over a change that
   /// still waited. Nor from the schedule as the page last read it, since the page is
   /// read again on every visit, and a visit is not a restart. One record for the
   /// process (<see cref="Shared"/>), which a page reads and saves through; WPF- and
   /// COM-free, so that a test can hold it to the sequences a page goes through.
   /// </summary>
   public sealed class DirectorySyncSchedule
   {
      /// <summary>The record the Directory synchronisation page keeps, for as long as the Control Panel runs.</summary>
      public static DirectorySyncSchedule Shared { get; } = new();

      // When the server last started, as its status said it, that registered_ belongs
      // to; null while it could not say.
      private string started_;

      // The schedule the running server registered, as far as this record knows; null
      // before the first read.
      private string registered_;

      // The schedule in the file, as this Control Panel last read or wrote it.
      private string inFile_;

      /// <summary>
      /// After the page read the file. <paramref name="started"/> is when the server last
      /// started or reinitialised (Status.StartTime), or null when it could not say; a
      /// start other than the one on record registered the schedule the file holds now.
      /// </summary>
      public void Read(string started, string schedule)
      {
         Observe_(started, Normalise_(schedule));
         inFile_ = Normalise_(schedule);
      }

      /// <summary>
      /// After a save wrote <paramref name="schedule"/> to the file: whether it waits for
      /// the server to start again. A start since the last read or save registered what
      /// the file held before this save.
      /// </summary>
      public bool Saved(string started, string schedule)
      {
         Observe_(started, inFile_);
         inFile_ = Normalise_(schedule);
         return Waits;
      }

      /// <summary>Whether the schedule in the file is not the one the running server registered.</summary>
      public bool Waits => registered_ != null && inFile_ != null && !string.Equals(inFile_, registered_, StringComparison.Ordinal);

      private void Observe_(string started, string fileThen)
      {
         started = string.IsNullOrWhiteSpace(started) ? null : started.Trim();

         if (registered_ == null)
         {
            registered_ = fileThen;
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
            registered_ = fileThen;
         }
      }

      private static string Normalise_(string schedule) => (schedule ?? "").Trim();
   }
}
