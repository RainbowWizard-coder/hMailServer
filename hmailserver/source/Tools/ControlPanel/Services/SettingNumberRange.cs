// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

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
      private const NumberStyles WholeNumber =
         NumberStyles.AllowLeadingWhite | NumberStyles.AllowTrailingWhite | NumberStyles.AllowLeadingSign | NumberStyles.AllowThousands;

      /// <summary>
      /// The whole number the text is, in the reader's way of writing one ("10,000",
      /// "10.000") or in digits alone; false for anything else, a fraction included.
      /// </summary>
      public static bool TryParse(string text, out long value)
      {
         string trimmed = (text ?? "").Trim();
         if (trimmed.Length == 0)
         {
            value = 0;
            return false;
         }

         return long.TryParse(trimmed, WholeNumber, CultureInfo.CurrentCulture, out value)
            || long.TryParse(trimmed, WholeNumber, CultureInfo.InvariantCulture, out value);
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
