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

      /// <summary>
      /// What a change was made to, in the reader's words: every object the server
      /// records - the audited tables' (AuditTrail.cpp, AuditedTables) and the events'
      /// and mail reads' own nouns - named as the pages that hold them name it, the
      /// old white lists by owner decision 4 ("Spam exemptions entry", "Greylisting
      /// exemption"); a word a newer server writes is humanised. Only 18 were named,
      /// and the rest read in English in every language, "Dns blacklist" and "White
      /// list address" among them (review finding F254).
      /// </summary>
      public static string ObjectWord(string objectType)
         => ObjectWords.TryGetValue((objectType ?? "").Trim().ToLowerInvariant(), out string word) ? L(word) : Humanise(objectType);

      /// <summary>Whether the page has a word of its own for an object type the server writes.</summary>
      public static bool NamesObject(string objectType) => ObjectWords.ContainsKey((objectType ?? "").Trim().ToLowerInvariant());

      /// <summary>
      /// What was done, in the reader's words: every action the server records - a
      /// row's created, updated and deleted, and each event's and mail read's own
      /// verb (every RecordEvent call) - where 8 were named and the rest humanised in
      /// English, "Directory opt out changed" with its hyphen lost; an action a newer
      /// server writes is humanised.
      /// </summary>
      public static string ActionWord(string action)
         => ActionWords.TryGetValue((action ?? "").Trim().ToLowerInvariant(), out string word) ? L(word) : Humanise(action);

      /// <summary>Whether the page has a word of its own for an action the server writes.</summary>
      public static bool NamesAction(string action) => ActionWords.ContainsKey((action ?? "").Trim().ToLowerInvariant());

      // The server's tokens, as it writes them and lower-cased ("OpenPGP key"), and the page's words.
      private static readonly Dictionary<string, string> ObjectWords = new(StringComparer.Ordinal)
      {
         ["setting"] = N("Setting"),
         ["domain"] = N("Domain"),
         ["domain alias"] = N("Domain alias"),
         ["account"] = N("Account"),
         ["booking policy"] = N("Booking policy"),
         ["alias"] = N("Alias address"),
         ["distribution list"] = N("Distribution list"),
         ["distribution list recipient"] = N("Distribution list member"),
         ["rule"] = N("Mail rule"),
         ["rule criterion"] = N("Rule criterion"),
         ["rule action"] = N("Rule action"),
         ["route"] = N("Route"),
         ["route address"] = N("Route address"),
         ["ip range"] = N("IP range"),
         ["tcp/ip port"] = N("TCP/IP port"),
         ["ssl certificate"] = N("SSL certificate"),
         ["incoming relay"] = N("Incoming relay"),
         ["dns blacklist"] = N("DNS block list"),
         ["surbl server"] = N("SURBL server"),
         ["white list address"] = N("Spam exemptions entry"),
         ["greylisting white address"] = N("Greylisting exemption"),
         ["greylisting bypass entry"] = N("Greylisting bypass entry"),
         ["blocked attachment"] = N("Blocked attachment"),
         ["blocked sender"] = N("Blocked senders entry"),
         ["sender list entry"] = N("Allow and block list entry"),
         ["spam trap"] = N("Spam trap"),
         ["spam trap hits"] = N("Spam trap hits"),
         ["domain policy list entry"] = N("Allowed or blocked domain"),
         ["never-ban name"] = N("Never-ban name"),
         ["journal rule"] = N("Journaling rule"),
         ["server message"] = N("Server message text"),
         ["group"] = N("Group"),
         ["group member"] = N("Group member"),
         ["fetch account"] = N("External POP3 or IMAP account"),
         ["application password"] = N("App password"),
         ["passkey"] = N("Passkey"),
         ["alert rule"] = N("Alert rule"),
         ["folder permission"] = N("Folder permission"),
         ["remote domain policy"] = N("Policy for a remote domain"),
         ["warm-up schedule"] = N("Warm-up schedule"),
         ["archive"] = N("Archive"),
         ["archive entry"] = N("Archived message"),
         ["discovery case"] = N("Discovery case"),
         ["discovery search"] = N("Discovery search"),
         ["discovery hold"] = N("Discovery hold"),
         ["discovery export"] = N("Discovery export"),
         ["role"] = N("Role"),
         ["role assignment"] = N("Role assignment"),
         ["role_assignment"] = N("Role assignment"),
         ["suppression list entry"] = N("Suppression list entry"),
         ["webhook subscription"] = N("Webhook subscription"),
         ["inbound webhook"] = N("Inbound webhook"),
         ["inbound webhook delivery"] = N("Inbound webhook delivery"),
         ["openpgp key"] = N("OpenPGP key"),
         ["s/mime authority"] = N("S/MIME certificate authority"),
         ["s/mime certificate"] = N("S/MIME certificate"),
         ["setting level"] = N("Domain or account setting"),
         ["migration"] = N("Migration"),
         ["migration job"] = N("Migration job"),
         ["backup"] = N("Backup"),
         ["queue"] = N("Delivery queue"),
         ["update"] = N("Software update"),
         ["server"] = N("Server"),
         ["administrator"] = N("Administrator"),
         ["api key"] = N("API key"),
         ["device"] = N("Signed-in device"),
         ["profile"] = N("Setting profile"),
         ["logon failures"] = N("Logon failures"),
         ["quarantined message"] = N("Quarantined message"),
         ["sandbox hold"] = N("Message held by the sandbox"),
         ["held message"] = N("Message held for review"),
      };

      private static readonly Dictionary<string, string> ActionWords = new(StringComparer.Ordinal)
      {
         ["created"] = N("Created"),
         ["updated"] = N("Updated"),
         ["deleted"] = N("Deleted"),
         ["unlocked"] = N("Unlocked"),
         ["cleared"] = N("Cleared"),
         ["revoked"] = N("Revoked"),
         ["applied"] = N("Applied"),
         ["reverted"] = N("Reverted"),
         ["declined"] = N("Declined"),
         ["discarded"] = N("Discarded"),
         ["released"] = N("Released"),
         ["removed"] = N("Removed"),
         ["retried"] = N("Retried"),
         ["downloaded"] = N("Downloaded"),
         ["cancelled"] = N("Cancelled"),
         ["moved"] = N("Moved"),
         ["move failed"] = N("Move failed"),
         ["enrolled"] = N("Enrolled"),
         ["disabled"] = N("Turned off"),
         ["entered"] = N("Started"),
         ["left"] = N("Ended"),
         ["read"] = N("Contents read"),
         ["searched"] = N("Searched"),
         ["mailbox-read"] = N("Mailbox read"),
         ["mailbox-changed"] = N("Mailbox changed"),
         ["password_reset"] = N("Password reset"),
         ["password reset"] = N("Password reset"),
         ["restore started"] = N("Restore started"),
         ["mailbox restore started"] = N("Mailbox restore started"),
         ["back-off cleared"] = N("Back-off cleared"),
         ["directory-opt-out-changed"] = N("Directory opt-out changed"),
         ["recovery-address-set"] = N("Recovery address set"),
         ["recovery-address-cleared"] = N("Recovery address cleared"),
         ["recovery-codes-generated"] = N("Recovery codes generated"),
         ["recovery-codes-cleared"] = N("Recovery codes cleared"),
         ["push-subscriptions-removed"] = N("Push subscriptions removed"),
         ["address-book-share-removed"] = N("Address book share removed"),
         ["review-changed"] = N("Review settings changed"),
         ["compromise-lock-cleared"] = N("Compromise lock cleared"),
         ["support-session-opened"] = N("Support session opened"),
         ["mailbox-recovery-key-made"] = N("Mailbox recovery key made"),
         ["mailbox-recovery-key-replaced"] = N("Mailbox recovery key replaced"),
         ["mailbox-recovery-key-removed"] = N("Mailbox recovery key removed"),
         ["mailbox-encryption-on"] = N("Mailbox encryption turned on"),
         ["mailbox-encryption-off"] = N("Mailbox encryption turned off"),
         ["mailbox-key-forfeited"] = N("Mailbox key forfeited"),
         ["mailbox-key-recovered"] = N("Mailbox key recovered"),
         ["install requested"] = N("Installation requested"),
         ["install started"] = N("Installation started"),
         ["install cancelled"] = N("Installation cancelled"),
         ["installed"] = N("Installed"),
         ["installed incomplete"] = N("Installed incompletely"),
         ["rolled back"] = N("Rolled back"),
         ["install failed"] = N("Installation failed"),
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

      // ---- the grid's widths (review finding F254) -----------------------------------

      /// <summary>
      /// A time as wide as any its format shows: every digit of <paramref name="formatted"/>
      /// the digit <paramref name="measure"/> finds widest. The window's font draws its
      /// digits at widths of their own, so that the column measured from
      /// "2026-10-10 00:00:00" cut "2026-10-09 23:56:48" to "2026-10-09 23:56:…".
      /// </summary>
      public static string WidestDigits(string formatted, Func<string, double> measure)
      {
         char widest = '0';
         double most = double.MinValue;
         for (char digit = '0'; digit <= '9'; digit++)
         {
            // Ten of each, so that a width rounded to the pixel still tells them apart.
            double width = measure(new string(digit, 10));
            if (width > most)
            {
               most = width;
               widest = digit;
            }
         }

         char[] sample = (formatted ?? "").ToCharArray();
         for (int i = 0; i < sample.Length; i++)
         {
            if (sample[i] >= '0' && sample[i] <= '9')
               sample[i] = widest;
         }

         return new string(sample);
      }

      /// <summary>
      /// The floor of a column of words: the widest of its words on the page shown, no
      /// narrower than its header and no wider than <paramref name="cap"/>, past which a
      /// word ends in an ellipsis and its tool tip says it whole. Floored at 90 pixels, the
      /// audit trail's five columns of words cut "Administrator", "Control Panel" and the
      /// name of the setting that changed at 1280 by 800, in every language.
      /// </summary>
      public static double WordsFloor(double header, IEnumerable<double> words, double cap)
      {
         double widest = 0;
         foreach (double width in words ?? Array.Empty<double>())
            widest = Math.Max(widest, width);
         return Math.Max(header, Math.Min(cap, Math.Ceiling(widest)));
      }

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
