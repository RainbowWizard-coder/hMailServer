// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// The bans in force, for the Auto-ban page's Banned now card and the IP ranges
   /// page's filter (review finding F231): which IP ranges are the server's own bans,
   /// why each was written, and when it lifts - with no WPF, so that AutoBanListTests
   /// holds it.
   ///
   /// A ban is an IP range the server writes for itself, named "Auto-ban: ..." and
   /// expiring on its own (docs/AutoBan.md). The server's own test for one of its
   /// bans is both - an expiring range whose name begins with the prefix
   /// (AutoBanWidener, AutoBanFirewall's reconciliation, the expiry sweep) - and this
   /// is the same test, so that the card lists what the server lifts and mirrors
   /// into the firewall, and no range of the administrator's that happens to expire.
   /// Before this, the Auto-ban page listed no ban at all, the IP ranges page drew
   /// them as permanent ranges, and lifting one took four steps across two pages.
   /// </summary>
   public static class AutoBanList
   {
      /// <summary>The prefix of a ban's range name: the server's (AccountLogon), not a caption.</summary>
      public const string Prefix = "Auto-ban: "; // no-loc: the server's own name for its bans

      /// <summary>Whether a range is one of the server's bans: expiring, and named with the prefix.</summary>
      public static bool IsBan(string rangeName, bool expires)
         => expires && rangeName != null && rangeName.StartsWith(Prefix, StringComparison.Ordinal);

      /// <summary>The banned address, or the run of addresses a widened ban covers.</summary>
      public static string Address(string lower, string upper)
         => string.IsNullOrEmpty(upper) || string.Equals(lower, upper, StringComparison.OrdinalIgnoreCase)
            ? lower ?? ""
            : (lower ?? "") + " - " + upper;

      /// <summary>
      /// Why the ban was written, from the rest of its name: the four writers of
      /// docs/AutoBan.md each name theirs their own way - a run of failed sign-ins by
      /// the user name it was guessing at, a spam trap by the trap, the null sender
      /// and AUTH on a delivery port by what was done (the address after it is the
      /// banned one, in the Address column already). A name of any other shape is
      /// shown as it is.
      /// </summary>
      public static string Cause(string rangeName)
      {
         string rest = rangeName != null && rangeName.StartsWith(Prefix, StringComparison.Ordinal)
            ? rangeName.Substring(Prefix.Length).Trim()
            : (rangeName ?? "").Trim();

         // Every writer names its range through AccountLogon::CreateIPRange, which makes
         // the name unique where a ban of that name exists already (GetIPRangeName_:
         // " (1)" to " (10)", then " (Z" and nine random characters, then a GUID in
         // braces) - a counter the reader has no use for, and one a second address that
         // mails the same spam trap while the first is banned is given as readily as a
         // second run of failed sign-ins as one user name. So the suffix goes before the
         // cause is read, for each of the four.
         rest = UniqueSuffix_.Replace(rest, "");

         const string trap = "spam trap "; // no-loc: the server's words (AccountLogon::BanForSpamTrap)
         if (rest.StartsWith(trap, StringComparison.Ordinal))
            return F("Mail to the spam trap {0}", rest.Substring(trap.Length).Trim());

         if (rest.StartsWith("multi-recipient null sender", StringComparison.Ordinal)) // no-loc: the server's words
            return L("Mail from an empty sender to several recipients");

         if (rest.StartsWith("AUTH on a delivery port", StringComparison.Ordinal)) // no-loc: the server's words
            return L("A sign-in attempt on a port that takes no password");

         // A run of failed sign-ins is named for the user name it guessed at.
         return rest.Length == 0 ? "" : F("Failed sign-ins as {0}", rest);
      }

      private static readonly System.Text.RegularExpressions.Regex UniqueSuffix_ =
         new(@" (?:\((?:[0-9]{1,2}|Z[^()\s]{1,9})\)|\{[0-9A-Fa-f-]{36}\})$", System.Text.RegularExpressions.RegexOptions.CultureInvariant); // no-loc: the server's suffixes

      /// <summary>
      /// When the ban ends, as its Ends cell says it: "23 min from now" where the
      /// server's clock is this computer's (<paramref name="sameClock"/>), the
      /// server's time where it is not, and "within a minute" for a ban whose time
      /// has passed - the expiry sweep runs once a minute.
      /// </summary>
      public static string Ends(DateTime expiresAt, DateTime now, bool sameClock)
      {
         if (!sameClock)
            return TimeUntilText.ServerTime(expiresAt);

         TimeSpan wait = expiresAt - now;
         return wait < TimeSpan.FromMinutes(1)
            ? L("within a minute")
            : F("{0} from now", TimeUntilText.Span(wait));
      }
   }
}
