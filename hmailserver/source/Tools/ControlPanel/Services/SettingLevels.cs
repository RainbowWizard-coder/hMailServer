// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Collections.Generic;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
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
      public SettingLevel(string name, SettingLevelType type, bool domainField, bool accountField, bool serverAdministratorsOnly)
      {
         Name = name;
         Type = type;
         DomainField = domainField;
         AccountField = accountField;
         ServerAdministratorsOnly = serverAdministratorsOnly;
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

      /// <summary>A limit or a ceiling: the server administrator's to set below the server.</summary>
      public bool ServerAdministratorsOnly { get; }
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
         new SettingLevel("max_message_size_kb", SettingLevelType.Integer, true, false, true),         // no-loc
         new SettingLevel("quarantine_digest", SettingLevelType.Boolean, true, false, false),          // no-loc
         new SettingLevel("quarantine_digest_hour", SettingLevelType.Integer, true, false, false),     // no-loc
         new SettingLevel("external_tag_subject", SettingLevelType.Boolean, true, false, false),       // no-loc
         new SettingLevel("external_tag_header", SettingLevelType.Boolean, true, false, false),        // no-loc
         new SettingLevel("impersonation_action", SettingLevelType.Integer, true, false, false),       // no-loc
         new SettingLevel("session_idle_seconds", SettingLevelType.Integer, true, false, false),       // no-loc
         new SettingLevel("session_lifetime_seconds", SettingLevelType.Integer, true, false, false),   // no-loc
         new SettingLevel("session_one_per_device", SettingLevelType.Boolean, true, false, false),     // no-loc
         new SettingLevel("session_bind_to_address", SettingLevelType.Boolean, true, false, false),    // no-loc
         new SettingLevel("quota_warning_percent", SettingLevelType.Integer, true, false, false),      // no-loc
         new SettingLevel("quota_grace_percent", SettingLevelType.Integer, true, false, false),        // no-loc
         new SettingLevel("quota_send_stop_percent", SettingLevelType.Integer, true, false, false),    // no-loc
         new SettingLevel("plus_addressing_enabled", SettingLevelType.Boolean, true, false, true),     // no-loc
         new SettingLevel("country_policy", SettingLevelType.Text, true, false, false),                // no-loc
         new SettingLevel("message_retention_days", SettingLevelType.Integer, true, true, true),       // no-loc
         new SettingLevel("recall_days", SettingLevelType.Integer, false, false, false),               // no-loc
         new SettingLevel("recover_deleted_days", SettingLevelType.Integer, false, false, true),       // no-loc
         new SettingLevel("sending_max_messages", SettingLevelType.Integer, false, false, true),       // no-loc
         new SettingLevel("sending_max_recipients", SettingLevelType.Integer, false, false, true),     // no-loc
         new SettingLevel("openpgp_portal_enabled", SettingLevelType.Boolean, false, false, false),    // no-loc
         new SettingLevel("zero_access_allowed", SettingLevelType.Boolean, false, false, false),       // no-loc
         new SettingLevel("reader_fetch_accounts_max", SettingLevelType.Integer, false, false, true),  // no-loc
         new SettingLevel("reader_sender_addresses_max", SettingLevelType.Integer, false, false, true),// no-loc
         new SettingLevel("reader_masked_addresses_max", SettingLevelType.Integer, false, false, true),// no-loc
         new SettingLevel("reader_offline_days_max", SettingLevelType.Integer, false, false, true),    // no-loc
         new SettingLevel("directory_user_editable", SettingLevelType.Boolean, false, false, false),   // no-loc
      };

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
