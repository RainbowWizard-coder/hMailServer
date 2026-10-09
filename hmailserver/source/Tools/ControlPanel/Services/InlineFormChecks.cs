// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Globalization;
using System.Net;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// What the Control Panel's forms check of a value before they send it, so
   /// that a value the server would refuse - or worse, take and use in a way
   /// nobody meant - is said on its field instead (review findings F129 and
   /// F212). Without WPF, so the tests can hold each rule.
   ///
   /// The forms read a number with int.TryParse and, where it failed, put in a
   /// value of their own: a priority that was no number became 15 and a route's
   /// port "25a" became 25, without a word. A number is a number here or it is
   /// refused. An address is read as the server reads one
   /// (<see cref="ServerAddressText"/>): System.Net.IPAddress.TryParse takes
   /// 10.1 and 010.0.0.1, which the server does not.
   /// </summary>
   public static class InlineFormChecks
   {
      /// <summary>How the two ends of an address range stand to each other, where both are addresses.</summary>
      public enum RangeOrder
      {
         /// <summary>The first is the last or below it, both of one family.</summary>
         InOrder,

         /// <summary>One is IPv4, the other IPv6: the range holds no address.</summary>
         MixedFamilies,

         /// <summary>The last is below the first: the range holds no address.</summary>
         Reversed
      }

      /// <summary>
      /// A whole number typed into a box - decimal digits with an optional sign,
      /// white space around them - within <paramref name="min"/> and
      /// <paramref name="max"/>. Nothing else is a number: not "25a", not "1e3",
      /// not "1,000", and not an empty box.
      /// </summary>
      public static bool TryWholeNumber(string text, int min, int max, out int value)
      {
         string trimmed = (text ?? "").Trim();
         if (!int.TryParse(trimmed, NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out value))
            return false;

         return value >= min && value <= max;
      }

      /// <summary>A TCP port typed into a box: a whole number from 1 to 65535.</summary>
      public static bool TryPort(string text, out int port) => TryWholeNumber(text, 1, 65535, out port);

      /// <summary>Whether the text is an IPv4 or IPv6 address the server reads, white space around it aside.</summary>
      public static bool IsAddress(string text) => ServerAddressText.IsAddress((text ?? "").Trim());

      /// <summary>
      /// How the ends of a range stand - the same family and the first not above
      /// the last, compared as the numbers the addresses are - or null where
      /// either end is no address (<see cref="IsAddress"/> says which).
      /// </summary>
      public static RangeOrder? OrderOf(string lower, string upper)
      {
         if (!ServerAddressText.TryParse((lower ?? "").Trim(), out IPAddress first) ||
             !ServerAddressText.TryParse((upper ?? "").Trim(), out IPAddress last))
         {
            return null;
         }

         if (first.AddressFamily != last.AddressFamily)
            return RangeOrder.MixedFamilies;

         byte[] a = first.GetAddressBytes();
         byte[] b = last.GetAddressBytes();
         for (int i = 0; i < a.Length; i++)
         {
            if (a[i] != b[i])
               return a[i] < b[i] ? RangeOrder.InOrder : RangeOrder.Reversed;
         }

         return RangeOrder.InOrder;
      }

      /// <summary>
      /// A host to deliver to: an IPv4 or IPv6 address the server reads, or a host
      /// name as the server takes one - letters, digits and hyphens in labels of up
      /// to 63, a single label such as "mailhub" included
      /// (<see cref="ApiKeyStore.IsValidDomainName"/>, StringParser::IsValidDomainName).
      /// </summary>
      public static bool IsHost(string text)
      {
         string host = (text ?? "").Trim();
         return host.Length > 0 && (ServerAddressText.IsAddress(host) || ApiKeyStore.IsValidDomainName(host));
      }
   }
}
