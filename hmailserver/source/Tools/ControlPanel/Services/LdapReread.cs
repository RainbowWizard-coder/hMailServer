// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// How long a directory synchronisation preview waits after the [LDAP] section was
   /// saved from this Control Panel, so that what it previews is what was saved (review
   /// finding F121).
   ///
   /// The server reads the section again only when two seconds have passed since it
   /// last looked (LdapSettings::MaybeRefresh_, kSettingsRecheckSeconds), and every
   /// sign-in of a directory-linked account looks, so on a server in use its copy is at
   /// most about two seconds old at any moment. A preview started straight after a save
   /// - Save and preview, or Preview pressed a moment after Save - ran on the selection
   /// before it (DirectorySync::Run reads LdapSettings::GetConfiguration), while the page
   /// recorded the preview as one of the options it reads from the file, the new ones,
   /// and enabled Apply, which then applied what no preview had listed. Two and a half
   /// seconds after the save, the server's copy is of the file as saved. Held by
   /// DirectorySyncUnsavedSelectionTests.
   /// </summary>
   public static class LdapReread
   {
      /// <summary>The server's two seconds, and half a second for the clock it counts them on.</summary>
      public static readonly TimeSpan AfterSave = TimeSpan.FromMilliseconds(2500);

      // When this process last wrote the [LDAP] section (Environment.TickCount64), or null.
      private static long? savedAt_;

      /// <summary>Notes that the [LDAP] section was just written: the Directory authentication and the Directory synchronisation pages' saves.</summary>
      public static void Saved() => savedAt_ = Environment.TickCount64;

      /// <summary>How much longer a preview started now waits for the server to read the last save.</summary>
      public static TimeSpan Wait() => Wait(savedAt_, Environment.TickCount64);

      /// <summary>
      /// How much longer a preview started at <paramref name="now"/> waits for a save made
      /// at <paramref name="savedAt"/>, both in milliseconds of one clock: what is left of
      /// <see cref="AfterSave"/>, never more; nothing when there was no save.
      /// </summary>
      public static TimeSpan Wait(long? savedAt, long now)
      {
         if (savedAt == null)
            return TimeSpan.Zero;

         long window = (long)AfterSave.TotalMilliseconds;
         long left = window - (now - savedAt.Value);
         if (left <= 0)
            return TimeSpan.Zero;

         return TimeSpan.FromMilliseconds(Math.Min(left, window));
      }
   }
}
