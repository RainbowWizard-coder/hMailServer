// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Text;
using System.Text.RegularExpressions;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// Who a server sendout reaches, worked out as the server works it out, so that
   /// the question asked before it is sent can say how many accounts that is (review
   /// finding F184): Utilities.EmailAllAccounts sends to every active account of an
   /// active domain whose address matches the wildcard (EmailAllUsers::Start), and
   /// the match is StringParser::WildcardMatchNoCase - the whole address, ignoring
   /// case, "*" for any run of characters and "?" for one.
   /// </summary>
   public static class SendoutAudience
   {
      /// <summary>Whether the address matches the wildcard, as the server matches it.</summary>
      public static bool Matches(string wildcard, string address)
      {
         var pattern = new StringBuilder("^");
         foreach (char c in wildcard ?? "")
         {
            if (c == '*')
               pattern.Append(".*");
            else if (c == '?')
               pattern.Append('.');
            else
               pattern.Append(Regex.Escape(c.ToString()));
         }

         pattern.Append('$');
         return Regex.IsMatch(address ?? "", pattern.ToString(), RegexOptions.IgnoreCase | RegexOptions.Singleline | RegexOptions.CultureInvariant);
      }

      /// <summary>Whether the account is one the sendout reaches.</summary>
      public static bool Reaches(string wildcard, string address, bool accountActive, bool domainActive)
         => accountActive && domainActive && Matches(wildcard, address);

      /// <summary>Whether the message has a body worth sending: something other than white space.</summary>
      public static bool HasBody(string body)
         => !string.IsNullOrWhiteSpace(body);
   }
}
