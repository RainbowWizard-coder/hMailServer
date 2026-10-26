// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Linq;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// AcmeDomains read as the server reads it, with no WPF so that
   /// AcmeCertificateStateTests holds it. AcmeClient::ParseDomainList splits the list at
   /// commas, trims each name, puts it in lower case, takes one final dot off, drops the
   /// empty ones and leaves out a name no certificate it orders can carry
   /// (AcmeClient::IsUsableHostName: a wildcard, an IP address, a name past ASCII, one
   /// holding a space or a semicolon); with no name left the server logs "ACME: No
   /// domains configured" and asks the CA for nothing, so the Automatic certificates
   /// (ACME) page says that before anything about the network (review finding F237),
   /// and the Setup checklist asks for no DNS record or open port for a name the server
   /// leaves out.
   /// </summary>
   public static class AcmeHostNames
   {
      // What the server's Trim takes off: isspace in the C locale.
      private static readonly char[] Space = { ' ', '\t', '\n', '\v', '\f', '\r' };

      /// <summary>
      /// True when the list holds a name the server reads as one: a host it orders for, or
      /// a name it leaves out and names (<see cref="Unusable"/>). A name of blanks and one
      /// dot is empty once the dot is off, and the server drops it as it drops a blank one
      /// (AcmeClient::ParseDomainList, UnusableDomains), so that a list of only such names
      /// is an empty list: counted as a name, "." passed for one and the page went on to say
      /// that no certificate could name "", which the server leaves out.
      /// </summary>
      public static bool Any(string domains)
         => Usable(domains).Count > 0 || Unusable(domains).Count > 0;

      /// <summary>
      /// The hosts the server orders a certificate for, as it reads them: trimmed, in
      /// lower case (ASCII alone, as the server's is), one final dot off, each once, in
      /// the order written.
      /// </summary>
      public static IReadOnlyList<string> Usable(string domains)
      {
         var usable = new List<string>();

         foreach (string written in Split(domains))
         {
            string name = Normalise(written);
            if (name.Length > 0 && IsUsableHostName(name) && !usable.Contains(name, StringComparer.Ordinal))
               usable.Add(name);
         }

         return usable;
      }

      /// <summary>
      /// The names of the list the server leaves out because no certificate can carry
      /// them, trimmed and as they are written, each once, in the order written
      /// (AcmeClient::UnusableDomains).
      /// </summary>
      public static IReadOnlyList<string> Unusable(string domains)
      {
         var unusable = new List<string>();

         foreach (string written in Split(domains))
         {
            string name = Normalise(written);
            if (name.Length > 0 && !IsUsableHostName(name) && !unusable.Contains(written, StringComparer.Ordinal))
               unusable.Add(written);
         }

         return unusable;
      }

      /// <summary>
      /// Whether a name, in lower case and with no final dot, is one an ACME order of type
      /// dns with an http-01 challenge can be made for: labels of a-z, 0-9 and '-', none
      /// starting or ending with '-', 1 to 63 characters each, 253 in all, the last not all
      /// digits (AcmeClient::IsUsableHostName).
      /// </summary>
      public static bool IsUsableHostName(string name)
      {
         if (string.IsNullOrEmpty(name) || name.Length > 253)
            return false;

         string[] labels = name.Split('.');

         foreach (string label in labels)
         {
            if (label.Length < 1 || label.Length > 63 || label[0] == '-' || label[label.Length - 1] == '-')
               return false;

            if (!label.All(c => (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
               return false;
         }

         // 192.0.2.1 is four labels of digits: an address, which an order of type dns
         // does not name.
         return !labels[labels.Length - 1].All(c => c >= '0' && c <= '9');
      }

      private static IEnumerable<string> Split(string domains)
         => (domains ?? "").Split(',').Select(name => name.Trim(Space));

      private static string Normalise(string trimmed)
      {
         var lower = new string(trimmed.Select(c => c >= 'A' && c <= 'Z' ? (char)(c + ('a' - 'A')) : c).ToArray());
         return lower.EndsWith(".", StringComparison.Ordinal) ? lower.Substring(0, lower.Length - 1) : lower;
      }
   }
}
