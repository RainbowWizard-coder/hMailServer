// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text.Json;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// When a saved setting reaches the running server, in the words the pages show
   /// beside each field kept in the settings store that was hMailServer.ini's
   /// [Settings] section, and in the sentence the window's pending bar says.
   ///
   /// The server decides the mark, not this program: every such key carries one in
   /// build/ini-settings-baseline.txt, answered over COM for one key by
   /// Settings.IniSettingApplies(name) and for every key at once by
   /// Settings.IniSettingAppliesAll, and over REST as applies
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
      /// The small mark beside a field: "Applies on save", "Applies after a
      /// reinitialise" or "Applies after a restart", the three in one form (review
      /// finding F311: "Applies on save" stood beside "After reinitialise" and "After
      /// a restart"). Null for a mark this program does not know - an older
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
               return L("Applies after a reinitialise");
            case Restart:
               return L("Applies after a restart");
            default:
               return null;
         }
      }

      /// <summary>
      /// Settings.IniSettingAppliesAll's answer - one JSON object from key to mark -
      /// as a map matched without case, as the server matches keys. Empty for null
      /// or blank; a JsonException for anything that is not such an object, which
      /// the caller treats as a failed read and does not remember.
      /// </summary>
      public static Dictionary<string, string> ParseMarks(string json)
      {
         var marks = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
         if (string.IsNullOrWhiteSpace(json))
            return marks;

         using JsonDocument document = JsonDocument.Parse(json);
         if (document.RootElement.ValueKind != JsonValueKind.Object)
            throw new JsonException("The marks are not a JSON object."); // no-loc

         foreach (JsonProperty property in document.RootElement.EnumerateObject())
         {
            if (property.Value.ValueKind == JsonValueKind.String)
               marks[property.Name] = property.Value.GetString();
         }

         return marks;
      }

      /// <summary>
      /// Whether a COM call failed because the server has no such member - a server
      /// from before 22 September 2026, which marks nothing and lists nothing as
      /// pending because it applies every [Settings] key at its next start. The
      /// late-bound call answers DISP_E_UNKNOWNNAME for a name the object's
      /// IDispatch does not know (DISP_E_MEMBERNOTFOUND for a property it has
      /// without that accessor), and the binder may say it in its own words. Told
      /// apart from any other failure, which may be a moment's - a reinitialise in
      /// progress, a dropped link - and is never remembered.
      /// </summary>
      public static bool IsMissingMember(Exception failure)
      {
         const int UnknownName = unchecked((int)0x80020006);     // DISP_E_UNKNOWNNAME
         const int MemberNotFound = unchecked((int)0x80020003);  // DISP_E_MEMBERNOTFOUND

         for (Exception e = failure; e != null; e = e.InnerException)
         {
            if (e is COMException com && (com.HResult == UnknownName || com.HResult == MemberNotFound))
               return true;
            if (e is MissingMemberException)
               return true;
            if (e.GetType().FullName == "Microsoft.CSharp.RuntimeBinder.RuntimeBinderException") // no-loc
               return true;
         }

         return false;
      }

      /// <summary>
      /// What Apply now says once the server is Running again. A reinitialise takes
      /// the keys marked reinitialize and not those marked restart, so when some of
      /// those are still pending it says how many rather than that everything saved
      /// is in effect.
      /// </summary>
      public static string AppliedSentence(int stillWaitingForRestart)
         => stillWaitingForRestart <= 0
            ? L("Reinitialised - the saved settings are in effect.")
            : Plural(stillWaitingForRestart,
               "Reinitialised - the settings that waited for a reinitialise are in effect; {0} still waits for a service restart.",
               "Reinitialised - the settings that waited for a reinitialise are in effect; {0} still wait for a service restart.");

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
      /// shown. waitingForReinitialize is what the bar's Apply now takes, and the
      /// sentence says so; waitingForRestart is said as well, because a reinitialise
      /// does not take it, and the bar's Restart the service does (review finding
      /// F010). One whole sentence per case and per count of one, so that every
      /// language counts its own way, where "{0} saved setting(s)" put the English
      /// plural ending in brackets after a translated noun.
      /// </summary>
      public static string Banner(int waitingForReinitialize, int waitingForRestart)
      {
         if (waitingForReinitialize <= 0 && waitingForRestart <= 0)
            return null;

         string reinitialise = waitingForReinitialize <= 0 ? null
            : Plural(waitingForReinitialize,
               "{0} saved setting waits for a reinitialise - Apply now takes it.",
               "{0} saved settings wait for a reinitialise - Apply now takes them.");

         if (waitingForRestart <= 0)
            return reinitialise;

         if (reinitialise == null)
            return Plural(waitingForRestart, "{0} saved setting waits for a service restart.", "{0} saved settings wait for a service restart.");

         // Both, in two sentences, each counting its own way; the second's subject
         // is the first's, as "3 saved settings wait ... 1 waits for a service restart."
         return Sentences(reinitialise,
            Plural(waitingForRestart, "{0} waits for a service restart.", "{0} wait for a service restart."));
      }

      /// <summary>
      /// What the pending bar's Restart the service asks before it stops the service:
      /// how many saved settings wait for it, and what a restart does to the clients
      /// connected - the question the feature settings pages ask after a save, for
      /// the settings any page saved.
      /// </summary>
      public static string RestartQuestion(int waitingForRestart)
         => Plural(waitingForRestart,
            "{0} saved setting takes effect only when the hMailServer service restarts. Restart the service now? Every client connected to the server is disconnected while it stops and starts again.",
            "{0} saved settings take effect only when the hMailServer service restarts. Restart the service now? Every client connected to the server is disconnected while it stops and starts again.");

      /// <summary>
      /// Which settings the pending bar counts, for its tool tip and its help text:
      /// one line per setting, under a heading for what it waits for - its caption
      /// and the page it is on, as the Ctrl+K palette finds it
      /// (<see cref="SettingsSearchIndex"/>), or its key where no page shows it. The
      /// bar said how many settings waited and never which. Null when none of them
      /// carries a mark the bar counts.
      /// </summary>
      /// <param name="pending">The keys the server lists as saved and not yet running.</param>
      /// <param name="markOf">The mark of a key: now, reinitialize or restart.</param>
      public static string PendingDetail(IEnumerable<string> pending, Func<string, string> markOf)
      {
         var reinitialise = new List<string>();
         var restart = new List<string>();
         foreach (string key in pending ?? Enumerable.Empty<string>())
         {
            string mark = markOf?.Invoke(key);
            if (mark == Reinitialize)
               reinitialise.Add(DescribePending(key));
            else if (mark == Restart)
               restart.Add(DescribePending(key));
         }

         var lines = new List<string>();
         if (reinitialise.Count > 0)
         {
            lines.Add(L("Waiting for a reinitialise:"));
            lines.AddRange(reinitialise);
         }

         if (restart.Count > 0)
         {
            lines.Add(L("Waiting for a service restart:"));
            lines.AddRange(restart);
         }

         return lines.Count == 0 ? null : string.Join("\n", lines);
      }

      /// <summary>A pending key in the reader's words: its caption and its page, or the key itself.</summary>
      public static string DescribePending(string key)
      {
         SettingEntry entry = SettingsSearchIndex.Entries.FirstOrDefault(e => string.Equals(e.Key, key, StringComparison.OrdinalIgnoreCase));
         if (entry == null)
            return key ?? "";

         return F("{0}, on the {1} page", L(entry.Label), L(NavigationMap.TitleOf(entry.Page)));
      }

      /// <summary>
      /// What Apply now asks before it reinitialises. A reinitialise ends
      /// maintenance mode, so a server that is draining is told so first: the
      /// listeners open again and delivery resumes.
      /// </summary>
      public static string ApplyNowQuestion(bool draining)
         => draining
            ? L("The server is draining. A reinitialise ends maintenance mode: the listeners open again and delivery resumes. Reinitialise the server now?")
            : L("Reinitialise the server now? Every service stops and starts again in place, so connected clients are disconnected and reconnect.");
   }
}
