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
   /// <summary>
   /// A domain's DKIM key rotation as the server reports it: Domain.DKIMRotationStatus
   /// over COM, the same document GET /api/v1/domains/{domain}/dkim/rotation answers.
   ///
   /// The server decides everything - the phase, what each slot's record check found,
   /// what is waiting and until when - and this only reads it and says it in the
   /// reader's language. A word the server has added since this was written reads as
   /// itself rather than throwing, so a newer server never breaks an older Control
   /// Panel's dialog. No WPF here, so the tests hold every word to a translated
   /// sentence and a level without a dispatcher.
   /// </summary>
   public sealed class DkimRotationStatus
   {
      public sealed class Record
      {
         public string Host = "";
         public string Type = "TXT"; // no-loc
         public string Value = "";
         public string Action = "";
      }

      public sealed class CheckResult
      {
         public string Status = "unchecked";
         public DateTimeOffset? Checked;
         public string Detail = "";
      }

      public sealed class Slot
      {
         public string Selector = "";
         public string KeyFile = "";
         public bool Generated;
         public DateTimeOffset? Since;
         public DateTimeOffset? Created;
         public DateTimeOffset? FirstSeen;
         public DateTimeOffset? SwitchDue;
         public DateTimeOffset? RetireDue;
         public DateTimeOffset? NextRotationDue;
         public DateTimeOffset? At;
         public string KeyFileKept = "";
         public List<Record> Records = new();
         public CheckResult Check;
      }

      public sealed class Wait
      {
         public string Step = "";
         public string Reason = "";
         public DateTimeOffset? Until;
         public string Detail = "";
      }

      public string Domain = "";
      public string Phase = "off";
      public bool PolicyEnabled;
      public int IntervalDays;
      public int PropagationHours;
      public int RetentionDays;
      public int KeyBits;
      public Slot Current;
      public Slot Next;
      public Slot Retiring;
      public Slot Retired;
      public List<Wait> Waiting = new();
      public string LastError = "";
      public DateTimeOffset? LastErrorTime;

      private static readonly Dictionary<string, string> Phases = new()
      {
         ["off"] = N("Off"),
         ["idle"] = N("Idle"),
         ["awaiting_dns"] = N("Waiting for the DNS record"),
         ["propagating"] = N("Propagating"),
         ["ready"] = N("Ready to switch"),
         ["waiting_for_retiring_slot"] = N("Waiting for the old selector to retire"),
      };

      private static readonly Dictionary<string, (StatusLevel Level, string Word)> Checks = new()
      {
         ["unchecked"] = (StatusLevel.Information, N("Not checked")),
         ["matches"] = (StatusLevel.Good, N("Matches")),
         ["not_found"] = (StatusLevel.Warning, N("Not found")),
         ["different"] = (StatusLevel.Critical, N("Different")),
         ["lookup_failed"] = (StatusLevel.Warning, N("Lookup failed")),
         ["key_unreadable"] = (StatusLevel.Critical, N("Key unreadable")),
      };

      private static readonly Dictionary<string, string> Actions = new()
      {
         ["publish"] = N("Publish"),
         ["keep"] = N("Keep"),
         ["remove"] = N("Remove"),
      };

      private static readonly Dictionary<string, string> Reasons = new()
      {
         ["not_scheduled"] = N("No rotation is scheduled. Rotate now to start one."),
         ["domain_inactive"] = N("The domain is inactive, so the schedule starts no rotation."),
         ["interval"] = N("The next rotation starts when the interval has passed."),
         ["unchecked"] = N("The new key's record has not been checked yet."),
         ["record_not_found"] = N("The new key's record was not found. Publish it."),
         ["record_differs"] = N("The published record does not carry the new key. Correct it."),
         ["lookup_failed"] = N("The DNS lookup failed. It is tried again on the next pass."),
         ["key_unreadable"] = N("The new key's file could not be read."),
         ["propagation"] = N("The record is propagating."),
         ["retiring_slot"] = N("Waiting for the old selector to be retired."),
         ["retention"] = N("The old selector is kept for mail still on its way."),
      };

      /// <summary>The document, or null when it is not one this can read.</summary>
      public static DkimRotationStatus Parse(string json)
      {
         if (string.IsNullOrWhiteSpace(json))
            return null;

         try
         {
            using JsonDocument document = JsonDocument.Parse(json);
            JsonElement root = document.RootElement;
            if (root.ValueKind != JsonValueKind.Object)
               return null;

            var status = new DkimRotationStatus
            {
               Domain = Text(root, "domain"),
               Phase = Text(root, "phase"),
               Current = ReadSlot(root, "current"),
               Next = ReadSlot(root, "next"),
               Retiring = ReadSlot(root, "retiring"),
               Retired = ReadSlot(root, "retired"),
            };

            if (root.TryGetProperty("policy", out JsonElement policy) && policy.ValueKind == JsonValueKind.Object)
            {
               status.PolicyEnabled = policy.TryGetProperty("enabled", out JsonElement enabled) && enabled.ValueKind == JsonValueKind.True;
               status.IntervalDays = (int)Number(policy, "interval_days");
               status.PropagationHours = (int)Number(policy, "propagation_hours");
               status.RetentionDays = (int)Number(policy, "retention_days");
               status.KeyBits = (int)Number(policy, "key_bits");
            }

            if (root.TryGetProperty("waiting", out JsonElement waiting) && waiting.ValueKind == JsonValueKind.Array)
            {
               foreach (JsonElement item in waiting.EnumerateArray())
               {
                  status.Waiting.Add(new Wait
                  {
                     Step = Text(item, "step"),
                     Reason = Text(item, "reason"),
                     Until = Time(item, "until"),
                     Detail = Text(item, "detail"),
                  });
               }
            }

            if (root.TryGetProperty("last_error", out JsonElement error) && error.ValueKind == JsonValueKind.Object)
            {
               status.LastError = Text(error, "text");
               status.LastErrorTime = Time(error, "time");
            }

            return status;
         }
         catch (JsonException)
         {
            return null;
         }
      }

      /// <summary>The phase in the reader's language; an unknown word as itself.</summary>
      public static string PhaseText(string phase)
         => phase != null && Phases.TryGetValue(phase, out string text) ? L(text) : phase ?? "";

      /// <summary>What a record check found, as a level and a word.</summary>
      public static (StatusLevel Level, string Word) CheckPresentation(string status)
         => status != null && Checks.TryGetValue(status, out var found) ? (found.Level, L(found.Word)) : (StatusLevel.Information, status ?? "");

      public static string ActionText(string action)
         => action != null && Actions.TryGetValue(action, out string text) ? L(text) : action ?? "";

      /// <summary>What a waiting reason means, as a sentence; an unknown word as itself.</summary>
      public static string ReasonText(string reason)
         => reason != null && Reasons.TryGetValue(reason, out string text) ? L(text) : reason ?? "";

      /// <summary>A time the document gives, in the reader's time zone.</summary>
      public static string When(DateTimeOffset? time)
         => time.HasValue ? time.Value.ToLocalTime().ToString("yyyy-MM-dd HH:mm", CultureInfo.InvariantCulture) : "—"; // no-loc

      /// <summary>The words this reads, for the tests.</summary>
      public static IEnumerable<string> KnownPhases => Phases.Keys;
      public static IEnumerable<string> KnownChecks => Checks.Keys;
      public static IEnumerable<string> KnownReasons => Reasons.Keys;
      public static IEnumerable<string> KnownActions => Actions.Keys;

      private static Slot ReadSlot(JsonElement root, string name)
      {
         if (!root.TryGetProperty(name, out JsonElement element) || element.ValueKind != JsonValueKind.Object)
            return null;

         var slot = new Slot
         {
            Selector = Text(element, "selector"),
            KeyFile = Text(element, "key_file"),
            Generated = element.TryGetProperty("generated", out JsonElement generated) && generated.ValueKind == JsonValueKind.True,
            Since = Time(element, "since"),
            Created = Time(element, "created"),
            FirstSeen = Time(element, "first_seen"),
            SwitchDue = Time(element, "switch_due"),
            RetireDue = Time(element, "retire_due"),
            NextRotationDue = Time(element, "next_rotation_due"),
            At = Time(element, "at"),
            KeyFileKept = Text(element, "key_file_kept"),
         };

         if (element.TryGetProperty("records", out JsonElement records) && records.ValueKind == JsonValueKind.Array)
         {
            foreach (JsonElement record in records.EnumerateArray())
            {
               slot.Records.Add(new Record
               {
                  Host = Text(record, "host"),
                  Type = Text(record, "type") is { Length: > 0 } type ? type : "TXT", // no-loc
                  Value = Text(record, "value"),
                  Action = Text(record, "action"),
               });
            }
         }

         if (element.TryGetProperty("check", out JsonElement check) && check.ValueKind == JsonValueKind.Object)
         {
            slot.Check = new CheckResult
            {
               Status = Text(check, "status") is { Length: > 0 } word ? word : "unchecked",
               Checked = Time(check, "checked"),
               Detail = Text(check, "detail"),
            };
         }

         return slot;
      }

      private static string Text(JsonElement element, string name)
         => element.TryGetProperty(name, out JsonElement value) && value.ValueKind == JsonValueKind.String ? value.GetString() ?? "" : "";

      private static double Number(JsonElement element, string name)
         => element.TryGetProperty(name, out JsonElement value) && value.ValueKind == JsonValueKind.Number ? value.GetDouble() : 0;

      /// <summary>Seconds since the epoch; 0 is "none".</summary>
      private static DateTimeOffset? Time(JsonElement element, string name)
      {
         double seconds = Number(element, name);
         return seconds > 0 ? DateTimeOffset.FromUnixTimeSeconds((long)seconds) : null;
      }
   }
}
