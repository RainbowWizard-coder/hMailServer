// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// The addresses typed on the Domains &amp; accounts page, apart from the window, so
   /// that HostedAddressTests can hold them (review findings F092 and F096).
   ///
   /// The account, alias and list boxes are addresses in the selected domain. The
   /// Address box held "user@" and the domain as text, in the primary colour, so an
   /// untouched Create account made a mailbox called user and typing a name meant
   /// selecting "user" first; the box is empty now, with "name@" and the domain as its
   /// placeholder, and a name typed without an @ is completed with the domain.
   /// And the account search filters the selected domain's accounts alone: typing a
   /// whole address of another hosted domain found nothing, so the page selects that
   /// domain first (<see cref="DomainToShow"/>).
   /// </summary>
   public static class HostedAddress
   {
      /// <summary>
      /// What was typed in one of the page's address boxes, as the address it names in
      /// <paramref name="domain"/>: the text as it is where it holds an @, and
      /// otherwise the text, an @ and the domain. Empty stays empty, so that the box is
      /// still refused as one left empty.
      /// </summary>
      public static string Complete(string typed, string domain)
      {
         string text = (typed ?? "").Trim();
         if (text.Length == 0 || text.Contains('@') || string.IsNullOrEmpty(domain))
            return text;

         return text + "@" + domain;
      }

      /// <summary>
      /// The placeholder of an address box of the selected domain: the example name, an
      /// @ and the domain - "name@example.com" - or nothing with no domain selected,
      /// where the forms wait for one.
      /// </summary>
      public static string Placeholder(string exampleName, string domain)
         => string.IsNullOrEmpty(domain) ? "" : exampleName + "@" + domain;

      /// <summary>
      /// The hosted domain an account search names after its @, where it is a domain
      /// other than the selected one - "user01@example.net" while example.com is
      /// selected names example.net - or null: no @, nothing after it, the selected
      /// domain, or no hosted domain of that name (a part of a name names none, so
      /// that the page does not change domains while a name is typed).
      /// </summary>
      public static string DomainToShow(string search, IEnumerable<string> hostedDomains, string selectedDomain)
      {
         string text = (search ?? "").Trim();
         int at = text.LastIndexOf('@');
         if (at < 0 || at == text.Length - 1 || hostedDomains == null)
            return null;

         string named = text[(at + 1)..];
         if (string.Equals(named, selectedDomain, StringComparison.OrdinalIgnoreCase))
            return null;

         foreach (string domain in hostedDomains)
         {
            if (string.Equals(domain, named, StringComparison.OrdinalIgnoreCase))
               return domain;
         }

         return null;
      }

      /// <summary>The domain of an address - the part after its last @ - or null where it has none.</summary>
      public static string DomainOf(string address)
      {
         string text = (address ?? "").Trim();
         int at = text.LastIndexOf('@');
         return at < 0 || at == text.Length - 1 ? null : text[(at + 1)..];
      }
   }
}
