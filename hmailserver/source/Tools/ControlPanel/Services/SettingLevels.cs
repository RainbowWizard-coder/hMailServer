// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Collections.Generic;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// The three kinds of setting with levels (docs/SettingLevels.md). A limit protects
   /// the server or the other tenants: a level below may tighten the level above and
   /// never loosen it, and the server refuses a looser value with a sentence the
   /// Levels tab shows. A gate is a capability the server grants: the server
   /// administrator's at every level. A preference is the tenant's own: the deepest
   /// level that holds a value wins.
   /// </summary>
   public enum SettingLevelClass
   {
      Limit,
      Gate,
      Preference
   }

   /// <summary>How a setting with levels is written and shown.</summary>
   public enum SettingLevelType
   {
      Integer,
      Boolean,
      Text
   }

   /// <summary>
   /// One setting a domain or an account may hold its own value of (one value,
   /// three levels - docs/SettingLevels.md). The name is the server's: what
   /// Domain.SettingOverride, Account.SettingOverride, EffectiveSetting and
   /// EffectiveSettingSource take over COM, and REST's field.
   /// </summary>
   public sealed class SettingLevel
   {
      public SettingLevel(string name, SettingLevelType type, bool domainField, bool accountField, SettingLevelClass kind)
      {
         Name = name;
         Type = type;
         DomainField = domainField;
         AccountField = accountField;
         Class = kind;
      }

      public string Name { get; }

      public SettingLevelType Type { get; }

      /// <summary>
      /// The domain's own value is held in a field of its own that another tab of the
      /// domain dialog edits (MaxMessageSize, QuarantineDigestHour, ...), not in the
      /// overlay: the Levels tab shows what is in effect and leaves the editing there.
      /// </summary>
      public bool DomainField { get; }

      /// <summary>The same for the account dialog: message_retention_days.</summary>
      public bool AccountField { get; }

      /// <summary>What kind of setting it is: a limit, a gate or a preference.</summary>
      public SettingLevelClass Class { get; }

      /// <summary>A gate: the server administrator's to set below the server.</summary>
      public bool ServerAdministratorsOnly => Class == SettingLevelClass.Gate;
   }

   /// <summary>
   /// The settings with levels, as the server's table has them
   /// (Common/Application/EffectiveSetting.cpp). build/check-setting-levels.py holds
   /// this list to that table name for name, so the Control Panel neither offers a
   /// setting the server does not resolve nor misses one it does. Plain data and
   /// pure functions, so ControlPanel.Tests can hold the words to the levels.
   /// </summary>
   public static class SettingLevels
   {
      public static readonly IReadOnlyList<SettingLevel> All = new[]
      {
         new SettingLevel("max_message_size_kb", SettingLevelType.Integer, true, false, SettingLevelClass.Limit),       // no-loc
         new SettingLevel("quarantine_digest", SettingLevelType.Boolean, true, false, SettingLevelClass.Preference),    // no-loc
         new SettingLevel("quarantine_digest_hour", SettingLevelType.Integer, true, false, SettingLevelClass.Preference), // no-loc
         new SettingLevel("external_tag_subject", SettingLevelType.Boolean, true, false, SettingLevelClass.Preference), // no-loc
         new SettingLevel("external_tag_header", SettingLevelType.Boolean, true, false, SettingLevelClass.Preference),  // no-loc
         new SettingLevel("impersonation_action", SettingLevelType.Integer, true, false, SettingLevelClass.Preference), // no-loc
         new SettingLevel("session_idle_seconds", SettingLevelType.Integer, true, false, SettingLevelClass.Limit),      // no-loc
         new SettingLevel("session_lifetime_seconds", SettingLevelType.Integer, true, false, SettingLevelClass.Limit),  // no-loc
         new SettingLevel("session_one_per_device", SettingLevelType.Boolean, true, false, SettingLevelClass.Limit),    // no-loc
         new SettingLevel("session_bind_to_address", SettingLevelType.Boolean, true, false, SettingLevelClass.Limit),   // no-loc
         new SettingLevel("quota_warning_percent", SettingLevelType.Integer, true, false, SettingLevelClass.Preference), // no-loc
         new SettingLevel("quota_grace_percent", SettingLevelType.Integer, true, false, SettingLevelClass.Limit),       // no-loc
         new SettingLevel("quota_send_stop_percent", SettingLevelType.Integer, true, false, SettingLevelClass.Limit),   // no-loc
         new SettingLevel("plus_addressing_enabled", SettingLevelType.Boolean, true, false, SettingLevelClass.Gate),    // no-loc
         new SettingLevel("country_policy", SettingLevelType.Text, true, false, SettingLevelClass.Preference),          // no-loc
         new SettingLevel("message_retention_days", SettingLevelType.Integer, true, true, SettingLevelClass.Preference), // no-loc
         new SettingLevel("recall_days", SettingLevelType.Integer, false, false, SettingLevelClass.Limit),              // no-loc
         new SettingLevel("recover_deleted_days", SettingLevelType.Integer, false, false, SettingLevelClass.Limit),     // no-loc
         new SettingLevel("sending_max_messages", SettingLevelType.Integer, false, false, SettingLevelClass.Limit),     // no-loc
         new SettingLevel("sending_max_recipients", SettingLevelType.Integer, false, false, SettingLevelClass.Limit),   // no-loc
         new SettingLevel("openpgp_portal_enabled", SettingLevelType.Boolean, false, false, SettingLevelClass.Gate),    // no-loc
         new SettingLevel("zero_access_allowed", SettingLevelType.Boolean, false, false, SettingLevelClass.Gate),       // no-loc
         new SettingLevel("reader_fetch_accounts_max", SettingLevelType.Integer, false, false, SettingLevelClass.Limit), // no-loc
         new SettingLevel("reader_sender_addresses_max", SettingLevelType.Integer, false, false, SettingLevelClass.Limit), // no-loc
         new SettingLevel("reader_masked_addresses_max", SettingLevelType.Integer, false, false, SettingLevelClass.Limit), // no-loc
         new SettingLevel("reader_offline_days_max", SettingLevelType.Integer, false, false, SettingLevelClass.Limit),  // no-loc
         new SettingLevel("directory_user_editable", SettingLevelType.Boolean, false, false, SettingLevelClass.Gate),   // no-loc
      };

      /// <summary>
      /// Each setting's name as a person reads it, from the caption the setting has on
      /// the page or tab that edits it for the whole server or the domain, less the
      /// bracket that says what its values mean there: the Levels tab showed the
      /// server's own keys as its headings - "max_message_size_kb", "session_idle_seconds"
      /// - in every language, against the owner's decision 11 of 9 October 2026 (a key
      /// name only as a small caption). The key stays in reach as that small caption
      /// beside the name, for the REST API that sets a level by it.
      /// </summary>
      private static readonly Dictionary<string, string> Captions = new(StringComparer.Ordinal)
      {
         ["max_message_size_kb"] = N("Maximum message size (KB)"),
         ["quarantine_digest"] = N("Quarantine digest"),
         ["quarantine_digest_hour"] = N("Digest hour"),
         ["external_tag_subject"] = N("Tag the subject of mail from outside"),
         ["external_tag_header"] = N("Add a header the reader shows as a banner"),
         ["impersonation_action"] = N("Impersonation protection"),
         ["session_idle_seconds"] = N("Seconds a session may go unused"),
         ["session_lifetime_seconds"] = N("Seconds a session may last at all"),
         ["session_one_per_device"] = N("Signing in again from the same device ends the earlier session"),
         ["session_bind_to_address"] = N("Refuse a session from any address but the one it began on"),
         ["quota_warning_percent"] = N("Warn the mailbox owner at this percentage of their quota"),
         ["quota_grace_percent"] = N("Still accept incoming mail past a mailbox's limit by (% of the limit)"),
         ["quota_send_stop_percent"] = N("Stop an account sending at (% of its limit)"),
         ["plus_addressing_enabled"] = N("Enable plus addressing"),
         ["country_policy"] = N("Country policy at sign-in"),
         ["message_retention_days"] = N("Delete messages older than (days)"),
         ["recall_days"] = N("Days after sending that mail to this server's accounts may be recalled"),
         ["recover_deleted_days"] = N("Days deleted messages are kept for the person to put back"),
         ["sending_max_messages"] = N("Maximum messages per account per period"),
         ["sending_max_recipients"] = N("Maximum recipients per account per period"),
         ["openpgp_portal_enabled"] = N("Offer OpenPGP in the webmail"),
         ["zero_access_allowed"] = N("Let people make their mailbox zero-access"),
         ["reader_fetch_accounts_max"] = N("Other POP3 or IMAP accounts a person may collect from"),
         ["reader_sender_addresses_max"] = N("Addresses at other providers a person may send as"),
         ["reader_masked_addresses_max"] = N("Masked addresses a person may make"),
         ["reader_offline_days_max"] = N("Days of mail a person may keep in the browser to read offline"),
         ["directory_user_editable"] = N("Let people edit their own directory entry"),
      };

      /// <summary>A setting's name as a person reads it, in the reader's language; the key itself for one this list does not know.</summary>
      public static string Caption(string name)
         => Captions.TryGetValue(name ?? "", out string caption) ? L(caption) : name;

      /// <summary>Whether a setting has a name of its own on the Levels tab (SettingLevelsTests holds every one).</summary>
      public static bool HasCaption(string name) => Captions.ContainsKey(name ?? "");

      /// <summary>A value in its text form (1 or 0, digits, text), as a person reads it.</summary>
      public static string Shown(string value, SettingLevelType type)
      {
         if (type == SettingLevelType.Boolean)
            return value == "1" ? L("On") : L("Off");

         if (string.IsNullOrEmpty(value))
            return L("(empty)");

         return value;
      }

      /// <summary>
      /// The line beside a setting: what is in effect and where it came from, as the
      /// server answers it - server, domain or account. Null for a word this program
      /// does not know, so that nothing is claimed.
      /// </summary>
      public static string InEffect(string value, string source, SettingLevelType type)
      {
         switch ((source ?? "").Trim().ToLowerInvariant())
         {
            case "server":   // no-loc
               return F("In effect: {0}, from the server", Shown(value, type));
            case "domain":   // no-loc
               return F("In effect: {0}, from the domain", Shown(value, type));
            case "account":  // no-loc
               return F("In effect: {0}, from the account", Shown(value, type));
            default:
               return null;
         }
      }

      /// <summary>
      /// A person's entry for a boolean setting, as the server takes it; the other
      /// types go as they were typed, and the server refuses what it does not take.
      /// </summary>
      public static string ForServer(string typed, SettingLevelType type)
      {
         string text = (typed ?? "").Trim();
         if (type != SettingLevelType.Boolean)
            return text;

         switch (text.ToLowerInvariant())
         {
            case "1":
            case "true":   // no-loc
            case "on":     // no-loc
            case "yes":    // no-loc
               return "1";
            case "0":
            case "false":  // no-loc
            case "off":    // no-loc
            case "no":     // no-loc
               return "0";
            default:
               return text;
         }
      }
   }
}
