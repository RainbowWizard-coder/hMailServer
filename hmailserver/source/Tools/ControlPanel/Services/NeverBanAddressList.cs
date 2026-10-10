// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Linq;
using System.Net;
using System.Net.Sockets;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>One entry of the never-ban address list, as the Auto-ban page's grid shows it.</summary>
   public sealed class NeverBanAddressRow
   {
      /// <summary>The entry as the setting holds it: an address, or a block in CIDR form.</summary>
      public string Entry { get; init; } = "";

      /// <summary>What it spares: one address, the run a block covers, or that the server ignores it.</summary>
      public string Covers { get; init; } = "";

      /// <summary>Whether the server reads it - an entry it cannot read spares nothing.</summary>
      public bool Readable { get; init; }

      /// <summary>What a screen reader says for the row.</summary>
      public override string ToString() => RowSpeech.Of(Entry, Covers);
   }

   /// <summary>
   /// The never-ban list by address, AutoBanNeverBan, read and written as the server
   /// reads it (AutoBanFirewall::IsNeverBanned_), with no WPF so that
   /// NeverBanAddressListTests holds it: entries apart at a comma, a semicolon or white
   /// space; each an address or an address and a prefix length; an IPv4 address in
   /// four parts of at most three digits with no leading zero (IPAddress::Parse, the
   /// same on every platform); and a block covering the addresses that share its
   /// first bits, whatever the bits after them are.
   ///
   /// The Auto-ban page edited the list as one comma-separated box saved with Save,
   /// beside the never-ban list by reverse name, a table whose buttons write at once
   /// (review finding F231): an entry the server could not read was said only in its
   /// error log, and what a block spared was left to the reader's arithmetic.
   /// </summary>
   public static class NeverBanAddressList
   {
      private static readonly char[] Separators = { ',', ';', ' ', '\t', '\r', '\n' };

      /// <summary>The entries of the setting's value, in its order, as the server splits them.</summary>
      public static List<string> Split(string value)
         => (value ?? "").Split(Separators, StringSplitOptions.RemoveEmptyEntries)
            .Select(entry => entry.Trim())
            .Where(entry => entry.Length > 0)
            .ToList();

      /// <summary>The setting's value for these entries, in the form the page's examples give it.</summary>
      public static string Join(IEnumerable<string> entries)
         => string.Join(", ", (entries ?? Enumerable.Empty<string>()).Select(entry => (entry ?? "").Trim()).Where(entry => entry.Length > 0));

      /// <summary>The grid's rows for the setting's value.</summary>
      public static List<NeverBanAddressRow> Rows(string value)
         => Split(value).Select(entry =>
         {
            bool readable = TryRead(entry, out IPAddress first, out IPAddress last, out bool single);
            return new NeverBanAddressRow
            {
               Entry = entry,
               Readable = readable,
               Covers = !readable ? L("Ignored: not an address or a block")
                  : single ? L("One address")
                  : first + " - " + last
            };
         }).ToList();

      /// <summary>
      /// Why a typed entry is refused before it is written, or null when it is not:
      /// nothing typed, not an address or a CIDR block as the server reads one, or on
      /// the list already.
      /// </summary>
      public static string Refusal(string typed, IEnumerable<string> listed)
      {
         string entry = (typed ?? "").Trim();
         if (entry.Length == 0)
            return L("An address or a block is required.");

         if (entry.IndexOfAny(Separators) >= 0 || !TryRead(entry, out _, out _, out _))
            return F("{0} is not an address or a CIDR block.", entry);

         if ((listed ?? Enumerable.Empty<string>()).Any(other => string.Equals(other, entry, StringComparison.OrdinalIgnoreCase)))
            return F("{0} is on the list already.", entry);

         return null;
      }

      /// <summary>The entries without this one, compared as the grid shows them.</summary>
      public static List<string> Without(IEnumerable<string> entries, string entry)
         => (entries ?? Enumerable.Empty<string>()).Where(other => !string.Equals(other, entry, StringComparison.OrdinalIgnoreCase)).ToList();

      /// <summary>
      /// Reads an entry as the server does: the first and last address it covers, and
      /// whether it is one address; false for an entry the server ignores.
      /// </summary>
      public static bool TryRead(string entry, out IPAddress first, out IPAddress last, out bool single)
      {
         first = last = null;
         single = false;

         string text = (entry ?? "").Trim();
         int prefix = -1;
         int slash = text.IndexOf('/');
         if (slash >= 0)
         {
            string bits = text.Substring(slash + 1).Trim();
            text = text.Substring(0, slash).Trim();
            if (bits.Length == 0 || bits.Length > 3 || !bits.All(c => c >= '0' && c <= '9'))
               return false;
            prefix = int.Parse(bits, System.Globalization.CultureInfo.InvariantCulture);
         }

         if (!TryAddress_(text, out IPAddress address))
            return false;

         byte[] bytes = address.GetAddressBytes();
         int widest = bytes.Length * 8;
         if (prefix < 0)
            prefix = widest;
         if (prefix > widest)
            return false;

         byte[] low = (byte[])bytes.Clone();
         byte[] high = (byte[])bytes.Clone();
         for (int bit = prefix; bit < widest; bit++)
         {
            byte mask = (byte)(0x80 >> (bit % 8));
            low[bit / 8] &= (byte)~mask;
            high[bit / 8] |= mask;
         }

         first = new IPAddress(low);
         last = new IPAddress(high);
         single = prefix == widest;
         return true;
      }

      // An address as IPAddress::Parse reads one: IPv4 strictly - four parts of at most
      // three digits, none with a leading zero, none above 255 - where .NET would take
      // "192.168.1" for 192.168.0.1 and "010.0.0.1" for 10.0.0.1; IPv6 as written, a
      // zone after "%" read and dropped, never in brackets or with a port.
      private static bool TryAddress_(string text, out IPAddress address)
      {
         address = null;
         if (text.Length == 0)
            return false;

         if (text.IndexOf(':') < 0)
         {
            string[] parts = text.Split('.');
            if (parts.Length != 4)
               return false;

            var bytes = new byte[4];
            for (int i = 0; i < 4; i++)
            {
               string part = parts[i];
               if (part.Length == 0 || part.Length > 3 || !part.All(c => c >= '0' && c <= '9') || (part.Length > 1 && part[0] == '0'))
                  return false;
               int value = int.Parse(part, System.Globalization.CultureInfo.InvariantCulture);
               if (value > 255)
                  return false;
               bytes[i] = (byte)value;
            }

            address = new IPAddress(bytes);
            return true;
         }

         if (text.StartsWith("[", StringComparison.Ordinal))
            return false;

         string withoutZone = text;
         int percent = text.IndexOf('%');
         if (percent >= 0)
            withoutZone = text.Substring(0, percent);

         if (!IPAddress.TryParse(withoutZone, out IPAddress parsed) || parsed.AddressFamily != AddressFamily.InterNetworkV6)
            return false;

         address = new IPAddress(parsed.GetAddressBytes());
         return true;
      }
   }
}
