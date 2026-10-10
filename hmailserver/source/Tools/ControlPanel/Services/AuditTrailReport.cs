// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Globalization;
using System.Text.Json;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>One row of the audit trail, as Utilities.GetAuditTrail answers it.</summary>
   public sealed class AuditRow
   {
      public long Id { get; init; }
      public long Time { get; init; }
      public string Actor { get; init; } = "";
      /// <summary>administrator, account, apikey or system - the server's words.</summary>
      public string ActorType { get; init; } = "";
      public string Interface { get; init; } = "";
      public string Address { get; init; } = "";
      public string ObjectType { get; init; } = "";
      public string ObjectName { get; init; } = "";
      public string Action { get; init; } = "";
      public string Detail { get; init; } = "";

      /// <summary>
      /// A setting's change carries the value it had, so it can be put back - all but
      /// a secret's, whose row says "(changed)" and never the values, which the server
      /// refuses to put back (docs/AuditTrail.md, Putting a change back).
      /// </summary>
      public bool CanPutBack => ObjectType == "setting" && (Action == "updated" || Action == "deleted")
         && !Detail.EndsWith(": (changed)", StringComparison.Ordinal);
   }

   /// <summary>
   /// The documents the server answers for the audit trail - Utilities.GetAuditTrail
   /// and Utilities.VerifyAuditTrail over COM, which are what GET /api/v1/audit and
   /// /api/v1/audit/verify answer - read into rows a grid can bind to. Pure, so
   /// the shape the page depends on is pinned by a test rather than by looking at
   /// a screen; malformed input is an error string and no rows, never an
   /// exception, because the page shows the string where the table would be.
   /// </summary>
   public sealed class AuditTrailReport
   {
      public IReadOnlyList<AuditRow> Entries { get; private init; } = Array.Empty<AuditRow>();
      public int Total { get; private init; }
      public int Offset { get; private init; }
      public string Error { get; private init; }

      public static AuditTrailReport Parse(string json)
      {
         try
         {
            using JsonDocument document = JsonDocument.Parse(json ?? "");
            JsonElement root = document.RootElement;
            var rows = new List<AuditRow>();
            if (root.TryGetProperty("entries", out JsonElement entries) && entries.ValueKind == JsonValueKind.Array)
            {
               foreach (JsonElement e in entries.EnumerateArray())
               {
                  rows.Add(new AuditRow
                  {
                     Id = Long(e, "id"),
                     Time = Long(e, "time"),
                     Actor = Text(e, "actor"),
                     ActorType = Text(e, "actor_type"),
                     Interface = Text(e, "interface"),
                     Address = Text(e, "address"),
                     ObjectType = Text(e, "object_type"),
                     ObjectName = Text(e, "object"),
                     Action = Text(e, "action"),
                     Detail = Text(e, "detail")
                  });
               }
            }

            return new AuditTrailReport { Entries = rows, Total = (int)Long(root, "total"), Offset = (int)Long(root, "offset") };
         }
         catch (Exception ex) when (ex is JsonException || ex is InvalidOperationException)
         {
            return new AuditTrailReport { Error = ex.Message };
         }
      }

      /// <summary>What Utilities.VerifyAuditTrail answered.</summary>
      public sealed class Verification
      {
         public bool Intact { get; init; }
         public long RowsChecked { get; init; }
         public long FirstBrokenId { get; init; }
         public string Reason { get; init; } = "";
         public string Error { get; init; }
      }

      public static Verification ParseVerification(string json)
      {
         try
         {
            using JsonDocument document = JsonDocument.Parse(json ?? "");
            JsonElement root = document.RootElement;
            return new Verification
            {
               Intact = root.TryGetProperty("intact", out JsonElement intact) && intact.ValueKind == JsonValueKind.True,
               RowsChecked = Long(root, "rows_checked"),
               FirstBrokenId = Long(root, "first_broken_id"),
               Reason = Text(root, "reason")
            };
         }
         catch (Exception ex) when (ex is JsonException || ex is InvalidOperationException)
         {
            return new Verification { Error = ex.Message };
         }
      }

      // ---- the server's words, as the reader's (review finding F254) -----------------

      /// <summary>
      /// The interface a change came in by, in the reader's words: the server writes
      /// COM, REST, Deck, Portal, server, Scheduler, command line and service start
      /// (AuditTrail::Actor), and the page showed them as written; a word it does not
      /// know is shown humanised (<see cref="Humanise"/>), never hidden.
      /// </summary>
      public static string InterfaceWord(string interfaceName) => (interfaceName ?? "").Trim().ToLowerInvariant() switch
      {
         // COM is the Control Panel and every script alike: the server cannot tell them apart.
         "com" => L("Control Panel or script"), // no-loc: the server's token
         "control_panel" => L("Control Panel"), // no-loc: the server's token
         "rest" => L("REST API"), // no-loc: the server's token
         "deck" => L("Control Deck"), // no-loc: the server's token
         "portal" => L("Self-service portal"), // no-loc: the server's token
         "server" => L("Server"), // no-loc: the server's token
         "scheduler" => L("Scheduled task"), // no-loc: the server's token
         "command line" => L("Command line"), // no-loc: the server's token
         "service start" => L("Service start"), // no-loc: the server's token
         _ => Humanise(interfaceName)
      };

      /// <summary>What a change was made to, in the reader's words; one the page does not name is humanised.</summary>
      public static string ObjectWord(string objectType) => (objectType ?? "").Trim().ToLowerInvariant() switch
      {
         "setting" => L("Setting"), // no-loc: the server's token
         "domain" => L("Domain"), // no-loc: the server's token
         "domain alias" => L("Domain alias"), // no-loc: the server's token
         "account" => L("Account"), // no-loc: the server's token
         "alias" => L("Alias address"), // no-loc: the server's token
         "distribution list" => L("Distribution list"), // no-loc: the server's token
         "rule" => L("Mail rule"), // no-loc: the server's token
         "route" => L("Route"), // no-loc: the server's token
         "ip range" => L("IP range"), // no-loc: the server's token
         "tcp/ip port" => L("TCP/IP port"), // no-loc: the server's token
         "ssl certificate" => L("SSL certificate"), // no-loc: the server's token
         "incoming relay" => L("Incoming relay"), // no-loc: the server's token
         "group" => L("Group"), // no-loc: the server's token
         "role" => L("Role"), // no-loc: the server's token
         "role assignment" or "role_assignment" => L("Role assignment"), // no-loc: the server's token
         "backup" => L("Backup"), // no-loc: the server's token
         "queue" => L("Delivery queue"), // no-loc: the server's token
         "update" => L("Software update"), // no-loc: the server's token
         _ => Humanise(objectType)
      };

      /// <summary>What was done, in the reader's words; an act the page does not name is humanised.</summary>
      public static string ActionWord(string action) => (action ?? "").Trim().ToLowerInvariant() switch
      {
         "created" => L("Created"), // no-loc: the server's token
         "updated" => L("Updated"), // no-loc: the server's token
         "deleted" => L("Deleted"), // no-loc: the server's token
         "unlocked" => L("Unlocked"), // no-loc: the server's token
         "cleared" => L("Cleared"), // no-loc: the server's token
         "revoked" => L("Revoked"), // no-loc: the server's token
         "applied" => L("Applied"), // no-loc: the server's token
         "password_reset" or "password reset" => L("Password reset"), // no-loc: the server's token
         _ => Humanise(action)
      };

      /// <summary>
      /// A token of the server's that the page has no word for, readable: its
      /// underscores and hyphens as spaces and its first letter a capital -
      /// "mailbox-key-forfeited" is "Mailbox key forfeited" - in the server's own
      /// language, since the page cannot translate a word it does not know.
      /// </summary>
      public static string Humanise(string token)
      {
         string words = (token ?? "").Replace('_', ' ').Replace('-', ' ').Trim();
         while (words.Contains("  ", StringComparison.Ordinal))
            words = words.Replace("  ", " ", StringComparison.Ordinal);
         return words.Length == 0 ? "" : char.ToUpper(words[0], CultureInfo.CurrentCulture) + words.Substring(1);
      }

      /// <summary>
      /// The values a setting's change records, "name: old -&gt; new" (the server's
      /// AuditTrail::RecordSettingChange), each side as written, the name before
      /// them taken off; false where the detail is not that shape, or holds the
      /// arrow more than once and so does not say where the old value ends.
      /// </summary>
      public static bool TrySplitChange(AuditRow row, out string from, out string to)
      {
         from = to = null;
         if (row == null || row.ObjectType != "setting")
            return false;

         string detail = row.Detail ?? "";
         string prefix = row.ObjectName + ": ";
         if (row.ObjectName.Length > 0 && detail.StartsWith(prefix, StringComparison.Ordinal))
            detail = detail.Substring(prefix.Length);

         int arrow = detail.IndexOf(ChangeArrow, StringComparison.Ordinal);
         if (arrow < 0 || detail.IndexOf(ChangeArrow, arrow + ChangeArrow.Length, StringComparison.Ordinal) >= 0)
            return false;

         from = detail.Substring(0, arrow);
         to = detail.Substring(arrow + ChangeArrow.Length);
         return true;
      }

      /// <summary>The server's arrow between a setting's old and new value.</summary>
      public const string ChangeArrow = " -> "; // no-loc: the server's own separator

      /// <summary>
      /// A row's detail as the page shows it: the server's arrow between two values
      /// drawn as one, "60 → 120", where it read "60 -&gt;".
      /// </summary>
      public static string ShownDetail(string detail)
         => (detail ?? "").Replace(ChangeArrow, " " + (char)0x2192 + " ", StringComparison.Ordinal);

      /// <summary>
      /// Whether a row is one a search finds: every word typed in is in who made the
      /// change, the interface, the address, what it was made to, its name, what was
      /// done or its detail - as the server wrote each and as the page shows it,
      /// whatever the case. The search matched the administrator alone.
      /// </summary>
      public static bool Matches(AuditRow row, string query)
      {
         if (row == null)
            return false;
         if (string.IsNullOrWhiteSpace(query))
            return true;

         string[] fields =
         {
            row.Actor, row.Interface, InterfaceWord(row.Interface), row.Address, row.ObjectType, ObjectWord(row.ObjectType),
            row.ObjectName, row.Action, ActionWord(row.Action), row.Detail
         };
         foreach (string word in query.Split((char[])null, StringSplitOptions.RemoveEmptyEntries))
         {
            bool found = false;
            foreach (string field in fields)
            {
               if ((field ?? "").Contains(word, StringComparison.CurrentCultureIgnoreCase))
               {
                  found = true;
                  break;
               }
            }
            if (!found)
               return false;
         }

         return true;
      }

      private static string Text(JsonElement element, string name)
         => element.TryGetProperty(name, out JsonElement value) && value.ValueKind == JsonValueKind.String ? value.GetString() ?? "" : "";

      private static long Long(JsonElement element, string name)
         => element.TryGetProperty(name, out JsonElement value) && value.ValueKind == JsonValueKind.Number ? value.GetInt64() : 0;
   }
}
