// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Text.Json;

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

      /// <summary>A setting's change carries the value it had, so it can be put back.</summary>
      public bool CanPutBack => ObjectType == "setting" && (Action == "updated" || Action == "deleted");
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

            return new AuditTrailReport { Entries = rows, Total = (int) Long(root, "total"), Offset = (int) Long(root, "offset") };
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

      private static string Text(JsonElement element, string name)
         => element.TryGetProperty(name, out JsonElement value) && value.ValueKind == JsonValueKind.String ? value.GetString() ?? "" : "";

      private static long Long(JsonElement element, string name)
         => element.TryGetProperty(name, out JsonElement value) && value.ValueKind == JsonValueKind.Number ? value.GetInt64() : 0;
   }
}
