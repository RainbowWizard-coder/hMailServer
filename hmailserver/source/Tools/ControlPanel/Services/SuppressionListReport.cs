// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Text.Json;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>One address on a domain's suppression list.</summary>
   public sealed class SuppressionEntryRow
   {
      public string Address { get; init; } = "";

      /// <summary>bounce, report or manual - the server's words, which the page translates.</summary>
      public string Source { get; init; } = "";

      /// <summary>hard, mailbox_full, policy, other, or none for an entry made by hand.</summary>
      public string Category { get; init; } = "";

      public string Status { get; init; } = "";

      /// <summary>The remote's own words for a bounce or a report; the administrator's note for a manual entry.</summary>
      public string Diagnostic { get; init; } = "";

      public string RemoteMta { get; init; } = "";
      public string Created { get; init; } = "";

      /// <summary>Empty when the entry has no expiry of its own.</summary>
      public string Expires { get; init; } = "";

      /// <summary>No longer applied, and not yet swept.</summary>
      public bool Expired { get; init; }
   }

   /// <summary>One address's bounce counts under one sending domain.</summary>
   public sealed class BounceCountRow
   {
      public string Address { get; init; } = "";
      public int Hard { get; init; }
      public int Soft { get; init; }
      public int MailboxFull { get; init; }
      public int Policy { get; init; }
      public int Other { get; init; }

      /// <summary>Hard bounces with no delivery between them: what the threshold is measured against.</summary>
      public int ConsecutiveHard { get; init; }

      public string LastStatus { get; init; } = "";
      public string LastCategory { get; init; } = "";
      public string LastBounce { get; init; } = "";
      public string LastDelivered { get; init; } = "";
      public bool Suppressed { get; init; }
   }

   /// <summary>
   /// The two documents the server answers for a domain's suppression list -
   /// Utilities.GetSuppressionList and Utilities.GetBounceCounts over COM, which are
   /// also what GET /api/v1/domains/{domain}/suppressions and /bounces answer - read
   /// into rows a grid can bind to.
   ///
   /// Pure, so that the shape the page depends on is pinned by a test rather than by
   /// looking at a screen: a key renamed on the server side must fail here, not
   /// quietly draw an empty column. Malformed input is an error string and no rows,
   /// never an exception, because the page shows the string where the table would be.
   /// </summary>
   public sealed class SuppressionListReport
   {
      public string Domain { get; private set; } = "";
      public bool ProcessingEnabled { get; private set; }

      /// <summary>refuse, drop or observe; empty on the counts document.</summary>
      public string Action { get; private set; } = "";

      /// <summary>How many match, which can be more than the rows returned.</summary>
      public int Total { get; private set; }

      public IReadOnlyList<SuppressionEntryRow> Entries => entries_;
      public IReadOnlyList<BounceCountRow> Counts => counts_;

      /// <summary>Null when the document was read; otherwise what was wrong with it.</summary>
      public string Error { get; private set; }

      private readonly List<SuppressionEntryRow> entries_ = new();
      private readonly List<BounceCountRow> counts_ = new();

      public static SuppressionListReport ParseSuppressions(string json)
      {
         var report = new SuppressionListReport();

         report.Read_(json, root =>
         {
            report.ProcessingEnabled = Bool_(root, "processing_enabled");
            report.Action = Text_(root, "action");

            foreach (JsonElement item in Items_(root))
            {
               report.entries_.Add(new SuppressionEntryRow
               {
                  Address = Text_(item, "address"),
                  Source = Text_(item, "source"),
                  Category = Text_(item, "category"),
                  Status = Text_(item, "status"),
                  Diagnostic = Text_(item, "diagnostic"),
                  RemoteMta = Text_(item, "remote_mta"),
                  Created = Text_(item, "created"),
                  Expires = Text_(item, "expires"),
                  Expired = Bool_(item, "expired")
               });
            }
         });

         return report;
      }

      public static SuppressionListReport ParseCounts(string json)
      {
         var report = new SuppressionListReport();

         report.Read_(json, root =>
         {
            foreach (JsonElement item in Items_(root))
            {
               report.counts_.Add(new BounceCountRow
               {
                  Address = Text_(item, "address"),
                  Hard = Number_(item, "hard"),
                  Soft = Number_(item, "soft"),
                  MailboxFull = Number_(item, "mailbox_full"),
                  Policy = Number_(item, "policy"),
                  Other = Number_(item, "other"),
                  ConsecutiveHard = Number_(item, "consecutive_hard"),
                  LastStatus = Text_(item, "last_status"),
                  LastCategory = Text_(item, "last_category"),
                  LastBounce = Text_(item, "last_bounce"),
                  LastDelivered = Text_(item, "last_delivered"),
                  Suppressed = Bool_(item, "suppressed")
               });
            }
         });

         return report;
      }

      private void Read_(string json, System.Action<JsonElement> read)
      {
         if (string.IsNullOrWhiteSpace(json))
         {
            Error = "The server answered with nothing."; // no-loc
            return;
         }

         try
         {
            using JsonDocument document = JsonDocument.Parse(json);
            JsonElement root = document.RootElement;

            if (root.ValueKind != JsonValueKind.Object)
            {
               Error = "The server's answer is not a JSON object."; // no-loc
               return;
            }

            Domain = Text_(root, "domain");
            Total = Number_(root, "total");
            read(root);
         }
         catch (JsonException ex)
         {
            entries_.Clear();
            counts_.Clear();
            Error = ex.Message;
         }
         catch (InvalidOperationException ex)
         {
            // A member of the wrong type: a number where a string belongs, say.
            entries_.Clear();
            counts_.Clear();
            Error = ex.Message;
         }
      }

      private static IEnumerable<JsonElement> Items_(JsonElement root)
      {
         if (!root.TryGetProperty("items", out JsonElement items) || items.ValueKind != JsonValueKind.Array)
            yield break;

         foreach (JsonElement item in items.EnumerateArray())
         {
            if (item.ValueKind == JsonValueKind.Object)
               yield return item;
         }
      }

      private static string Text_(JsonElement element, string name)
         => element.TryGetProperty(name, out JsonElement value) && value.ValueKind == JsonValueKind.String
            ? value.GetString() ?? ""
            : "";

      private static int Number_(JsonElement element, string name)
         => element.TryGetProperty(name, out JsonElement value) && value.ValueKind == JsonValueKind.Number && value.TryGetInt32(out int number)
            ? number
            : 0;

      private static bool Bool_(JsonElement element, string name)
         => element.TryGetProperty(name, out JsonElement value) && value.ValueKind == JsonValueKind.True;
   }
}
