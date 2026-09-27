// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Linq;
using System.Net;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// Whether the host name this server announces in HELO/EHLO (Settings.HostName)
   /// is a name the Internet can look up - the question the setup checklist asks of
   /// it. Every server this one delivers to is told the name and compares it with
   /// DNS: its address, and the PTR record of the address the mail came from. An
   /// empty setting makes the server announce its Windows computer name
   /// (Utilities::ComputerName), which no public DNS knows.
   /// </summary>
   public static class HostNameVerdict
   {
      public enum Kind
      {
         /// <summary>Nothing is set: the server announces its Windows computer name.</summary>
         Empty,

         /// <summary>A single label, an address, or a name with a character DNS does not take.</summary>
         NotADnsName,

         /// <summary>A name under a suffix that is never public: .local, .lan, .test and the like.</summary>
         PrivateName,

         /// <summary>A full DNS name, which the Internet may look up.</summary>
         PublicName
      }

      // Suffixes no public DNS answers for: RFC 2606 and RFC 6761's reserved names,
      // RFC 8375's home.arpa, and the private ones installations commonly use.
      private static readonly string[] PrivateSuffixes =
      {
         "local", "localhost", "localdomain", "lan", "home", "internal", "intranet", "corp", "private",
         "test", "example", "invalid", "home.arpa"
      };

      public static Kind Judge(string hostName)
      {
         string name = (hostName ?? "").Trim().TrimEnd('.');
         if (name.Length == 0)
            return Kind.Empty;

         if (IPAddress.TryParse(name.Trim('[', ']'), out _))
            return Kind.NotADnsName;

         string[] labels = name.Split('.');
         if (labels.Length < 2 || name.Length > 253)
            return Kind.NotADnsName;

         foreach (string label in labels)
         {
            if (label.Length == 0 || label.Length > 63 || label.StartsWith("-", StringComparison.Ordinal) || label.EndsWith("-", StringComparison.Ordinal))
               return Kind.NotADnsName;

            if (!label.All(c => (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-'))
               return Kind.NotADnsName;
         }

         string lower = name.ToLowerInvariant();
         if (PrivateSuffixes.Any(suffix => lower == suffix || lower.EndsWith("." + suffix, StringComparison.Ordinal)))
            return Kind.PrivateName;

         return Kind.PublicName;
      }
   }
}
