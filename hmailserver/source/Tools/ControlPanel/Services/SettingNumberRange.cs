// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Globalization;
using System.Text.RegularExpressions;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// What the hMailServer.ini pages hold a number to before a save writes it, and
   /// what they say on the field when it is outside (review finding F220).
   ///
   /// The ports, timeouts, cache lifetimes and work factors were free-text boxes whose
   /// save wrote whatever was typed. The server reads each with GetPrivateProfileInt
   /// rules - "30s" is 30, a word is 0 - and then clamps some, reports and replaces
   /// some, and overflows on others; each key's range on the page is the one the
   /// server reads it in (FeatureSettingsView's definitions, which say where each
   /// bound comes from). Held by SettingNumberRangeTests.
   /// </summary>
   public static class SettingNumberRange
   {
      // The spaces a reader groups digits with where the culture's separator is a
      // space: fr writes a narrow no-break space, sv, nb, fi, cs, pl, ru and uk a
      // no-break space, and every one of them types the space bar's.
      private static readonly string[] Spaces = { " ", "\u00A0", "\u202F" };

      /// <summary>
      /// The whole number the text is, in the reader's way of writing one; false for
      /// anything else. <see cref="TryParse(string, CultureInfo, out long)"/> says what is
      /// taken.
      /// </summary>
      public static bool TryParse(string text, out long value)
         => TryParse(text, CultureInfo.CurrentCulture, out value);

      /// <summary>
      /// The whole number the text is: digits alone ("10000"), or digits grouped as
      /// <paramref name="culture"/> groups them, every separator where it belongs
      /// ("10,000" in English, "10.000" in German, "10 000" in French or Swedish);
      /// false for anything else - a fraction ("1.5", "1,5"), a separator out of its
      /// place, the other language's separator, a sign, a unit ("30s"). NumberStyles'
      /// AllowThousands took a separator wherever it fell, so English read "1,5" as
      /// 15 and German "1.5" (review finding F220), and the page saved a number other
      /// than the one typed without a word.
      /// </summary>
      public static bool TryParse(string text, CultureInfo culture, out long value)
      {
         value = 0;
         string trimmed = (text ?? "").Trim();
         if (trimmed.Length == 0)
            return false;

         if (AllDigits_(trimmed))
            return long.TryParse(trimmed, NumberStyles.None, CultureInfo.InvariantCulture, out value);

         NumberFormatInfo format = (culture ?? CultureInfo.InvariantCulture).NumberFormat;
         string separator = format.NumberGroupSeparator;
         if (string.IsNullOrEmpty(separator))
            return false;

         string[] separators = Array.IndexOf(Spaces, separator) >= 0 ? Spaces : new[] { separator };
         string[] groups = trimmed.Split(separators, StringSplitOptions.None);
         if (groups.Length < 2 || !GroupedAs_(groups, format.NumberGroupSizes))
            return false;

         return long.TryParse(string.Concat(groups), NumberStyles.None, CultureInfo.InvariantCulture, out value);
      }

      // Whether the groups, read from the right, are as long as the culture makes them:
      // each as long as its size (the last size repeating), the leftmost one to that
      // long. A size of 0 leaves the digits to its left ungrouped.
      private static bool GroupedAs_(string[] groups, int[] sizes)
      {
         if (sizes == null || sizes.Length == 0)
            return false;

         for (int i = groups.Length - 1, n = 0; i >= 0; i--, n++)
         {
            string group = groups[i];
            int size = sizes[Math.Min(n, sizes.Length - 1)];
            if (group.Length == 0 || !AllDigits_(group))
               return false;

            if (i == 0)
               return size == 0 || group.Length <= size;

            if (group.Length != size)
               return false;
         }

         return false;
      }

      // The digits 0 to 9 and nothing else: the server reads no other digit.
      private static bool AllDigits_(string text)
      {
         foreach (char c in text)
         {
            if (c < '0' || c > '9')
               return false;
         }

         return text.Length > 0;
      }

      /// <summary>
      /// Why the text cannot be saved for a key read in <paramref name="minimum"/> to
      /// <paramref name="maximum"/> - and 0 as well, when <paramref name="zeroIsBuiltIn"/>
      /// - or null when it can.
      /// </summary>
      public static string Error(string text, long minimum, long maximum, bool zeroIsBuiltIn = false)
      {
         if (TryParse(text, out long value) && ((value >= minimum && value <= maximum) || (zeroIsBuiltIn && value == 0)))
            return null;

         return Describe(minimum, maximum, zeroIsBuiltIn);
      }

      /// <summary>The sentence for the range, its numbers written as the reader writes them.</summary>
      public static string Describe(long minimum, long maximum, bool zeroIsBuiltIn = false)
      {
         string from = minimum.ToString("N0", CultureInfo.CurrentCulture);
         string to = maximum.ToString("N0", CultureInfo.CurrentCulture);

         if (zeroIsBuiltIn)
            return F("Enter 0 for the built-in value, or a whole number from {0} to {1}.", from, to);

         if (maximum >= int.MaxValue)
            return F("Enter a whole number of {0} or more.", from);

         return F("Enter a whole number from {0} to {1}.", from, to);
      }
   }

   /// <summary>
   /// What the hMailServer.ini pages hold an address to before a save writes it
   /// (review finding F220).
   /// </summary>
   public static class SettingAddressRule
   {
      // Four decimal parts of 0 to 255 and no leading zeros: the form inet_pton reads,
      // which is how the server reads DNSServer on every platform (DNSResolverWinApi,
      // DnssecResolver, TlsPolicy). The older forms .NET also takes - 8.8.8, or
      // 010.0.0.1 as octal - the server reports as invalid and falls back to the
      // system's name servers.
      private static readonly Regex Ipv4 = new(
         @"^(25[0-5]|2[0-4][0-9]|1[0-9][0-9]|[1-9]?[0-9])(\.(25[0-5]|2[0-4][0-9]|1[0-9][0-9]|[1-9]?[0-9])){3}$",
         RegexOptions.CultureInvariant);

      /// <summary>
      /// The DNS override: empty, which leaves the name servers Windows uses, or one
      /// IPv4 address - the server takes no IPv6 address and no list there. Null when
      /// the value may be saved.
      /// </summary>
      public static string DnsOverride(string value)
      {
         string trimmed = (value ?? "").Trim();
         if (trimmed.Length == 0 || Ipv4.IsMatch(trimmed))
            return null;

         return L("Enter one IPv4 address, such as 1.1.1.1, or leave the box empty for the name servers Windows uses.");
      }
   }
}
