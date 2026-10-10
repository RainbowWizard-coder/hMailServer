// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Net;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// Whether an IP range lets the addresses in it relay mail - send it from outside
   /// to outside, external to external - without signing in, and how far that reaches
   /// (review finding F229). The IP ranges page showed "Deliver remote → remote
   /// (relay)" apart from "Require auth: external to external", which only the
   /// range's dialog held, so that unticking the first on the shipped Internet range
   /// stopped roaming users sending mail, and ticking it on a range that did not ask
   /// for authentication made an open relay, with no word either way.
   ///
   /// A range of one address relaying without authentication is a choice about one
   /// machine - My computer, 127.0.0.1, does it as shipped - and is not pointed out.
   /// A wider one inside a network of its own (the private, loopback, link-local and
   /// carrier-grade blocks of RFC 1918, 5735, 6598 and 4193) lets every machine of
   /// that network relay, which suits a printer or an application and is a warning;
   /// any other is an open relay for whoever is in it, which is critical and is
   /// asked about before it is saved.
   ///
   /// No WPF and no COM here, so the tests hold the rules and the words.
   /// </summary>
   public static class RelayExposure
   {
      public enum Reach
      {
         /// <summary>Relaying needs authentication, is not allowed, or is one address's.</summary>
         None,

         /// <summary>Every machine of a private network may relay without signing in.</summary>
         Network,

         /// <summary>Addresses on the Internet may relay without signing in: an open relay.</summary>
         Internet
      }

      /// <summary>How far a range with these permissions lets mail be relayed without signing in.</summary>
      public static Reach Of(string lower, string upper, bool allowExternalToExternal, bool requireAuthenticationExternalToExternal)
      {
         if (!allowExternalToExternal || requireAuthenticationExternalToExternal)
            return Reach.None;

         bool parsed = IPAddress.TryParse((lower ?? "").Trim(), out IPAddress low) & IPAddress.TryParse((upper ?? "").Trim(), out IPAddress high);
         if (!parsed)
            return Reach.Internet;

         if (low.Equals(high))
            return Reach.None;

         return SamePrivateBlock_(low, high) ? Reach.Network : Reach.Internet;
      }

      /// <summary>The level the card's notice takes: critical for an open relay, a warning for a private network's.</summary>
      public static StatusLevel LevelOf(Reach reach) => reach == Reach.Internet ? StatusLevel.Critical : StatusLevel.Warning;

      /// <summary>What the permissions card says of a range that relays without authentication; null for one that does not.</summary>
      public static string Notice(Reach reach) => reach switch
      {
         Reach.Internet => L("Anyone in this range can relay mail through this server to any address without signing in: an open relay. Tick Only after SMTP authentication for External to external, unless the range is a few machines you trust."),
         Reach.Network => L("Every machine in this range can relay mail through this server to any address without signing in. That suits a printer or an application on your own network; otherwise tick Only after SMTP authentication for External to external."),
         _ => null
      };

      /// <summary>The question before permissions that make an open relay are saved.</summary>
      public static string SaveQuestion(string range)
         => F("Save the permissions of {0}? Anyone in it can then relay mail through this server to any address without signing in: an open relay.", range);

      // The blocks inside which every address is a network's own: RFC 1918, loopback,
      // link-local, carrier-grade NAT (RFC 6598), and IPv6's loopback, unique local
      // (RFC 4193) and link-local.
      private static readonly (string Network, int Prefix)[] PrivateBlocks =
      {
         ("10.0.0.0", 8), ("172.16.0.0", 12), ("192.168.0.0", 16), // no-loc: address blocks
         ("127.0.0.0", 8), ("169.254.0.0", 16), ("100.64.0.0", 10), // no-loc: address blocks
         ("::1", 128), ("fc00::", 7), ("fe80::", 10) // no-loc: address blocks
      };

      private static bool SamePrivateBlock_(IPAddress low, IPAddress high)
      {
         if (low.AddressFamily != high.AddressFamily)
            return false;

         foreach ((string network, int prefix) in PrivateBlocks)
         {
            IPAddress block = IPAddress.Parse(network);
            if (block.AddressFamily == low.AddressFamily && InBlock_(low, block, prefix) && InBlock_(high, block, prefix))
               return true;
         }

         return false;
      }

      private static bool InBlock_(IPAddress address, IPAddress block, int prefix)
      {
         byte[] a = address.GetAddressBytes();
         byte[] b = block.GetAddressBytes();
         for (int bit = 0; bit < prefix; bit++)
         {
            int i = bit / 8;
            int mask = 0x80 >> (bit % 8);
            if ((a[i] & mask) != (b[i] & mask))
               return false;
         }

         return true;
      }
   }
}
