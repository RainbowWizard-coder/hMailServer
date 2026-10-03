// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Net;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// An IP address in text as the server reads one - IPAddress::Parse in
   /// Common/TCPIP/IPAddress.cpp, the same on Windows and on Linux since 6.3.6: an
   /// IPv4 address is four decimal parts, none past 255 and none with a leading
   /// zero; an IPv6 address is RFC 4291's text, with a zone after a '%' if it has
   /// one; and nothing around either - no brackets, no port.
   ///
   /// System.Net.IPAddress.TryParse reads a good deal more, as Windows' own parser
   /// does: 10.1 as 10.0.0.1, 3232235777 as 192.168.1.1, 0x7f.1 as 127.0.0.1, a
   /// part with a leading zero as octal (010.0.0.1 as 8.0.0.1), and an IPv6 address
   /// in brackets with a port after it. A value the Control Panel checks with it
   /// and writes where the server reads it - an API key's source restriction - is
   /// then one the server refuses, and a key restricted so is refused from every
   /// address with nothing to say why. So the Control Panel asks this instead.
   /// </summary>
   public static class ServerAddressText
   {
      /// <summary>Whether text is an address the server reads, and which.</summary>
      public static bool TryParse(string text, out IPAddress address)
      {
         address = null;
         if (text == null)
            return false;

         if (text.IndexOf(':') < 0)
         {
            var v4 = new byte[4];
            if (!ParseIPv4(text, 0, text.Length, v4, 0))
               return false;

            address = new IPAddress(v4);
            return true;
         }

         int percent = text.IndexOf('%');
         int addressLength = percent >= 0 ? percent : text.Length;

         long zone = 0;
         if (percent >= 0 && !ParseZone(text, percent + 1, text.Length - percent - 1, out zone))
            return false;

         var v6 = new byte[16];
         if (!ParseIPv6(text, addressLength, v6))
            return false;

         address = new IPAddress(v6, zone);
         return true;
      }

      /// <summary>Whether text is an address the server reads.</summary>
      public static bool IsAddress(string text) => TryParse(text, out _);

      // Four parts of one to three decimal digits, none past 255 and none with a
      // leading zero, filling text[start, start + length) exactly.
      private static bool ParseIPv4(string text, int start, int length, byte[] bytes, int offset)
      {
         int end = start + length;
         int position = start;

         for (int part = 0; part < 4; part++)
         {
            if (part > 0)
            {
               if (position >= end || text[position] != '.')
                  return false;

               position++;
            }

            int first = position;
            int value = 0;

            while (position < end && position - first < 3 && text[position] >= '0' && text[position] <= '9')
            {
               value = value * 10 + (text[position] - '0');
               position++;
            }

            int digits = position - first;
            if (digits == 0 || value > 255 || (digits > 1 && text[first] == '0'))
               return false;

            bytes[offset + part] = (byte)value;
         }

         return position == end;
      }

      private static int HexDigit(char c)
      {
         if (c >= '0' && c <= '9')
            return c - '0';
         if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
         if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;

         return -1;
      }

      // RFC 4291 section 2.2: eight groups of one to four hexadecimal digits, one
      // run of zero groups written once as ::, and the last two groups as an IPv4
      // address in the form above - the server's ParseIPv6_, line for line.
      private static bool ParseIPv6(string text, int length, byte[] bytes)
      {
         var head = new byte[16];
         var tail = new byte[16];
         int headCount = 0;
         int tailCount = 0;
         bool compressed = false;
         int position = 0;

         if (length >= 2 && text[0] == ':' && text[1] == ':')
         {
            compressed = true;
            position = 2;
         }
         else if (length >= 1 && text[0] == ':')
            return false;

         while (position < length)
         {
            byte[] output = compressed ? tail : head;
            int count = compressed ? tailCount : headCount;

            int end = position;
            bool dotted = false;
            while (end < length && text[end] != ':')
            {
               if (text[end] == '.')
                  dotted = true;
               end++;
            }

            if (dotted)
            {
               if (end != length || headCount + tailCount + 4 > 16)
                  return false;

               if (!ParseIPv4(text, position, length - position, output, count))
                  return false;

               count += 4;
               if (compressed) tailCount = count; else headCount = count;
               position = length;
               break;
            }

            int first = position;
            int value = 0;

            while (position < length && position - first < 4 && HexDigit(text[position]) >= 0)
            {
               value = value * 16 + HexDigit(text[position]);
               position++;
            }

            if (position == first || (position < length && text[position] != ':'))
               return false;

            if (headCount + tailCount + 2 > 16)
               return false;

            output[count++] = (byte)(value >> 8);
            output[count++] = (byte)(value & 0xFF);
            if (compressed) tailCount = count; else headCount = count;

            if (position == length)
               break;

            position++;

            if (position < length && text[position] == ':')
            {
               if (compressed)
                  return false;

               compressed = true;
               position++;
            }
            else if (position == length)
               return false;
         }

         int total = headCount + tailCount;

         if (compressed ? total > 14 : total != 16)
            return false;

         System.Array.Clear(bytes, 0, 16);
         System.Array.Copy(head, 0, bytes, 0, headCount);
         System.Array.Copy(tail, 0, bytes, 16 - tailCount, tailCount);
         return true;
      }

      // The zone after the '%': a number up to 4294967295, or an interface's name
      // of letters, digits, '.', '-' and '_', at most 64 characters. A name is
      // zone 0 here; the server looks it up, and what the Control Panel asks of
      // an address never depends on its zone.
      private static bool ParseZone(string text, int start, int length, out long zone)
      {
         zone = 0;
         if (length == 0 || length > 64)
            return false;

         bool numeric = true;

         for (int i = start; i < start + length; i++)
         {
            char c = text[i];

            if (c >= '0' && c <= '9')
               continue;

            numeric = false;

            bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
            if (!letter && c != '.' && c != '-' && c != '_')
               return false;
         }

         if (!numeric)
            return true;

         long value = 0;
         for (int i = start; i < start + length; i++)
         {
            value = value * 10 + (text[i] - '0');
            if (value > 0xFFFFFFFFL)
               return false;
         }

         zone = value;
         return true;
      }
   }
}
