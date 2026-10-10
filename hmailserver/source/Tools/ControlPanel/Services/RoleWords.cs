// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Linq;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// What the Administrator roles page says of a role and an assignment, in the
   /// reader's language (review findings F084 and F086).
   ///
   /// A role's "What it is for" was the description hm_roles holds, which the
   /// database scripts write in English for the four built-in roles, so German and
   /// Japanese read "Every permission, in every domain." among translated captions.
   /// The four are named by the server's code (RoleManager::BuiltInPermissions_)
   /// and their descriptions by the scripts, so the page shows each built-in role's
   /// purpose from the catalogue, by the role's name, and a custom role's as its
   /// author typed it. The English is the scripts' own, word for word.
   ///
   /// An assignment's domains came as the server lists them, "example.com,example.net",
   /// beside permissions written with ", "; and what is typed for one is sent with
   /// each name trimmed and the empty ones dropped.
   /// </summary>
   public static class RoleWords
   {
      private static readonly Dictionary<string, string> BuiltInPurposes = new(StringComparer.OrdinalIgnoreCase)
      {
         // CreateTablesMSSQL.sql and the other scripts' hm_roles rows.
         ["serveradmin"] = N("Every permission, in every domain."),
         ["domainadmin"] = N("The accounts, mailboxes and quarantine of named domains."),
         ["helpdesk"] = N("Resets a password, unlocks an account and reads a message trace."),
         ["auditor"] = N("Reads everything and changes nothing, the audit trail included."),
      };

      /// <summary>The four built-in roles' names, as the server's code has them.</summary>
      public static IReadOnlyCollection<string> BuiltInNames => BuiltInPurposes.Keys;

      /// <summary>
      /// What the role is for: a built-in role's purpose in the reader's language,
      /// and any other role's description as the server holds it.
      /// </summary>
      public static string Purpose(string name, bool builtIn, string stored)
         => builtIn && name != null && BuiltInPurposes.TryGetValue(name, out string purpose) ? L(purpose) : stored ?? "";

      /// <summary>An assignment's list of domains as the grid shows it: "example.com, example.net".</summary>
      public static string DomainsText(string listed)
         => string.Join(", ", Names_(listed));

      /// <summary>
      /// What is typed in the form's Domains box as the server takes it: each name
      /// trimmed, the empty ones dropped, joined with commas - "example.com,example.net"
      /// for " example.com , example.net, ". Empty stays empty, which the server reads
      /// as the account's own domain, or every domain for serveradmin and auditor.
      /// </summary>
      public static string DomainsToSend(string typed)
         => string.Join(",", Names_(typed));

      private static IEnumerable<string> Names_(string list)
         => (list ?? "").Split(',').Select(name => name.Trim()).Where(name => name.Length > 0);
   }
}
