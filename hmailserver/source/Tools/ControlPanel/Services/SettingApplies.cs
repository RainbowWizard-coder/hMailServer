// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Linq;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// When a saved setting reaches the running server, in the words the pages show
   /// beside each field kept in the settings store that was hMailServer.ini's
   /// [Settings] section, and in the sentence the window's pending bar says.
   ///
   /// The server decides the mark, not this program: every such key carries one in
   /// build/ini-settings-baseline.txt, answered over COM by
   /// Settings.IniSettingApplies(name) and over REST as applies
   /// (docs/SettingsApply.md). Three words:
   ///
   ///   now           a save applies it - most keys;
   ///   reinitialize  it is bound into something the servers build when they
   ///                 start (a listener, a TLS context, a work queue's size), so it
   ///                 waits for a reinitialise - the pending bar's Apply now;
   ///   restart       it is read once per process, so it waits for the service to
   ///                 be restarted.
   ///
   /// Until 22 September 2026 every one of these pages said "applies after a
   /// service restart", because every key did. Plain data and pure functions, no
   /// WPF and no COM, so ControlPanel.Tests can hold the words to the marks.
   /// </summary>
   public static class SettingApplies
   {
      public const string Now = "now";                    // no-loc
      public const string Reinitialize = "reinitialize";  // no-loc
      public const string Restart = "restart";            // no-loc

      /// <summary>
      /// The small mark beside a field: "Applies on save", "After reinitialize" or
      /// "After a restart". Null for a mark this program does not know - an older
      /// server that answers nothing, or a word a newer one adds - so that a page
      /// shows no mark rather than a wrong one.
      /// </summary>
      public static string Caption(string mark)
      {
         switch ((mark ?? "").Trim().ToLowerInvariant())
         {
            case Now:
               return L("Applies on save");
            case Reinitialize:
               return L("After reinitialize");
            case Restart:
               return L("After a restart");
            default:
               return null;
         }
      }

      /// <summary>
      /// The names Settings.PendingIniSettings lists, one per line as
      /// IniSettingNames lists names. Empty for null or blank.
      /// </summary>
      public static IReadOnlyList<string> ParsePending(string list)
      {
         if (string.IsNullOrWhiteSpace(list))
            return Array.Empty<string>();

         return list.Split(new[] { "\r\n", "\n" }, StringSplitOptions.RemoveEmptyEntries)
                    .Select(name => name.Trim())
                    .Where(name => name.Length > 0)
                    .ToArray();
      }

      /// <summary>
      /// The pending bar's sentence, or null when nothing waits and the bar is not
      /// shown. waitingForReinitialize is what the bar's Apply now takes;
      /// waitingForRestart is said as well, because a reinitialise does not take it
      /// and an administrator who pressed Apply now and still saw a count would
      /// otherwise be owed an explanation the bar never gave.
      /// </summary>
      public static string Banner(int waitingForReinitialize, int waitingForRestart)
      {
         if (waitingForReinitialize <= 0 && waitingForRestart <= 0)
            return null;

         if (waitingForRestart <= 0)
            return F("{0} saved setting(s) wait for a reinitialize.", waitingForReinitialize);

         if (waitingForReinitialize <= 0)
            return F("{0} saved setting(s) wait for a service restart.", waitingForRestart);

         return F("{0} saved setting(s) wait for a reinitialize, and {1} for a service restart.", waitingForReinitialize, waitingForRestart);
      }

      /// <summary>
      /// What Apply now asks before it reinitialises. A reinitialise ends
      /// maintenance mode, so a server that is draining is told so first: the
      /// listeners open again and delivery resumes.
      /// </summary>
      public static string ApplyNowQuestion(bool draining)
         => draining
            ? L("The server is draining. A reinitialize ends maintenance mode: the listeners open again and delivery resumes. Reinitialize the server now?")
            : L("Reinitialize the server now? Every service stops and starts again in place, so connected clients are disconnected and reconnect.");
   }
}
