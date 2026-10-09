// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.Json;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>One pattern of the never-ban list by reverse name.</summary>
   public sealed class NeverBanNameRow
   {
      public int Id { get; init; }
      public string Pattern { get; init; } = "";
      public string Note { get; init; } = "";
      public string Created { get; init; } = "";

      /// <summary>What a screen reader says for the row on the Auto-ban page's grid.</summary>
      public override string ToString() => RowSpeech.Of(Pattern, Note, RowSpeech.Labelled(L("Added"), Created));
   }

   /// <summary>One reverse name of a checked address, as the server judged it.</summary>
   public sealed class NeverBanCheckedName
   {
      public string Name { get; init; } = "";

      /// <summary>False for an address written as a name, or a public suffix: never matched.</summary>
      public bool Usable { get; init; }

      /// <summary>The pattern covering the name, or empty.</summary>
      public string Pattern { get; init; } = "";

      /// <summary>Whether the name's forward lookup gives the address; null when it was not asked or not answered.</summary>
      public bool? ResolvesBack { get; init; }
   }

   /// <summary>What the server would decide about one address.</summary>
   public sealed class NeverBanCheck
   {
      public string Address { get; init; } = "";
      public bool Answered { get; init; }
      public bool Spared { get; init; }
      public string Name { get; init; } = "";
      public string Pattern { get; init; } = "";

      /// <summary>The server's own clause, in English, for the log.</summary>
      public string Reason { get; init; } = "";

      public IReadOnlyList<NeverBanCheckedName> Names { get; init; } = Array.Empty<NeverBanCheckedName>();
   }

   /// <summary>
   /// The never-ban list by reverse name (GitLab work item #56) as the Auto-ban
   /// page reads it: Settings.AutoBanNeverBanNames, the rows, and
   /// Settings.CheckAutoBanNeverBanName, the verdict about one address - the same
   /// documents GET /api/v1/never-ban-names and POST /api/v1/never-ban-names/check
   /// answer - and the sentence the page says about a verdict.
   ///
   /// Pure, so that the shape the page depends on is pinned by a test: a key
   /// renamed on the server must fail here, not draw an empty column. A document
   /// that cannot be read is an error string and nothing else, never an
   /// exception, because the page shows the string where the answer would be.
   /// </summary>
   public static class NeverBanNames
   {
      public static IReadOnlyList<NeverBanNameRow> ParseList(string json, out string error)
      {
         error = null;
         var rows = new List<NeverBanNameRow>();

         try
         {
            using JsonDocument document = JsonDocument.Parse(json ?? "");
            JsonElement root = document.RootElement;

            if (root.ValueKind != JsonValueKind.Object || !root.TryGetProperty("items", out JsonElement items) || items.ValueKind != JsonValueKind.Array)
            {
               error = L("The server's answer has no list of entries.");
               return rows;
            }

            foreach (JsonElement item in items.EnumerateArray())
            {
               rows.Add(new NeverBanNameRow
               {
                  Id = Int_(item, "id"),
                  Pattern = Text_(item, "pattern"),
                  Note = Text_(item, "note"),
                  Created = Text_(item, "created")
               });
            }
         }
         catch (JsonException ex)
         {
            error = F("The server's answer could not be read: {0}", ex.Message);
            rows.Clear();
         }

         return rows;
      }

      public static NeverBanCheck ParseCheck(string json, out string error)
      {
         error = null;

         try
         {
            using JsonDocument document = JsonDocument.Parse(json ?? "");
            JsonElement root = document.RootElement;

            if (root.ValueKind != JsonValueKind.Object || !root.TryGetProperty("address", out _) || !root.TryGetProperty("spared", out _))
            {
               error = L("The server's answer has no verdict.");
               return null;
            }

            var names = new List<NeverBanCheckedName>();
            if (root.TryGetProperty("reverse_names", out JsonElement list) && list.ValueKind == JsonValueKind.Array)
            {
               foreach (JsonElement item in list.EnumerateArray())
               {
                  names.Add(new NeverBanCheckedName
                  {
                     Name = Text_(item, "name"),
                     Usable = Flag_(item, "usable") == true,
                     Pattern = Text_(item, "pattern"),
                     ResolvesBack = Flag_(item, "resolves_back")
                  });
               }
            }

            return new NeverBanCheck
            {
               Address = Text_(root, "address"),
               Answered = Flag_(root, "answered") == true,
               Spared = Flag_(root, "spared") == true,
               Name = Text_(root, "name"),
               Pattern = Text_(root, "pattern"),
               Reason = Text_(root, "reason"),
               Names = names
            };
         }
         catch (JsonException ex)
         {
            error = F("The server's answer could not be read: {0}", ex.Message);
            return null;
         }
      }

      /// <summary>
      /// The verdict in the reader's words, and what to do about it: built from the
      /// structured answer rather than from the server's English clause, so that it
      /// is translated.
      /// </summary>
      public static string Describe(NeverBanCheck check)
      {
         if (check == null)
            return "";

         if (check.Spared)
            return F("{0} is never auto-banned: its reverse name {1} resolves back to it and is covered by {2}.", check.Address, check.Name, check.Pattern);

         if (!check.Answered)
            return F("The reverse lookup of {0} was not answered, so the list cannot spare it now. Try again, or add the address to 'Never auto-ban these addresses' on the Auto-ban page.", check.Address);

         if (check.Names.Count == 0)
            return F("{0} has no reverse name, so no pattern can spare it. Add the address to 'Never auto-ban these addresses' on the Auto-ban page instead.", check.Address);

         NeverBanCheckedName covered = check.Names.FirstOrDefault(n => n.Usable && n.Pattern.Length > 0);
         if (covered != null)
         {
            return covered.ResolvesBack == false
               ? F("{0} is covered by {1} but does not resolve back to {2}, so it is not spared: anybody who holds an address can write its reverse name.", covered.Name, covered.Pattern, check.Address)
               : F("{0} is covered by {1}, but its forward lookup was not answered, so {2} is not spared now.", covered.Name, covered.Pattern, check.Address);
         }

         NeverBanCheckedName usable = check.Names.FirstOrDefault(n => n.Usable);
         if (usable == null)
            return F("The reverse name {0} is an address or a public suffix, which the list never matches. Add the address to 'Never auto-ban these addresses' on the Auto-ban page instead.", check.Names[0].Name);

         return usable.ResolvesBack == true
            ? F("No pattern covers {0}. It resolves back to {1}, so a pattern covering it would spare the address.", usable.Name, check.Address)
            : F("No pattern covers {0}, and it does not resolve back to {1}, so no pattern would spare the address.", usable.Name, check.Address);
      }

      private static string Text_(JsonElement element, string name)
      {
         if (element.ValueKind != JsonValueKind.Object || !element.TryGetProperty(name, out JsonElement value))
            return "";
         return value.ValueKind switch
         {
            JsonValueKind.String => value.GetString() ?? "",
            JsonValueKind.Null => "",
            _ => value.ToString()
         };
      }

      private static int Int_(JsonElement element, string name)
      {
         if (element.ValueKind != JsonValueKind.Object || !element.TryGetProperty(name, out JsonElement value))
            return 0;
         return value.ValueKind == JsonValueKind.Number && value.TryGetInt32(out int number) ? number : 0;
      }

      private static bool? Flag_(JsonElement element, string name)
      {
         if (element.ValueKind != JsonValueKind.Object || !element.TryGetProperty(name, out JsonElement value))
            return null;
         return value.ValueKind switch
         {
            JsonValueKind.True => true,
            JsonValueKind.False => false,
            _ => null
         };
      }
   }
}
