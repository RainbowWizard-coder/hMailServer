// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Collections.Generic;
using System.Globalization;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// What a rule's criterion asks of its fields before Views/RuleCriteriaDialog
   /// saves it, and the comparisons each field offers (review finding F227).
   ///
   /// The dialog wrote the header name and the value as they stood: a custom header
   /// with no name - which the server reads as no header at all, so the criterion
   /// matches nothing - and a value left empty, which "contains" matches in every
   /// message and "does not contain" in none. Only "equals" and "does not equal" mean
   /// something with an empty value: that the field is empty, or that it is not.
   ///
   /// Two fields are numbers the server writes as digits and compares as numbers
   /// when the match is "is less than" or "is greater than" (RuleApplier::TestMatch):
   /// the message's size in bytes, headers and attachments included
   /// (Message::GetSize, the file's size), and the delivery attempt, 1 on the first
   /// (GetNoOfRetries() + 1). They opened on "contains", a comparison of text that
   /// says nothing about a number, and had no unit; they now open on "is greater
   /// than", offer the four comparisons a number has, and take a whole number.
   /// </summary>
   public static class RuleCriterionInput
   {
      /// <summary>The predefined field the dialog's list tags a custom header with.</summary>
      public const int CustomHeader = 0;

      /// <summary>RuleCriteria::FTMessageSize: the message's size in bytes.</summary>
      public const int MessageSize = 6;

      /// <summary>RuleCriteria::FTDeliveryAttempts: 1 on the first attempt.</summary>
      public const int DeliveryAttempts = 8;

      // RuleCriteria::MatchType, as the dialog's list tags them.
      public const int MatchEquals = 1;
      public const int MatchContains = 2;
      public const int MatchLessThan = 3;
      public const int MatchGreaterThan = 4;
      public const int MatchRegex = 5;
      public const int MatchNotContains = 6;
      public const int MatchNotEquals = 7;
      public const int MatchWildcard = 8;

      /// <summary>Every comparison, in the order the list has always shown them.</summary>
      private static readonly int[] AllMatches =
      {
         MatchEquals, MatchContains, MatchLessThan, MatchGreaterThan, MatchRegex, MatchNotContains, MatchNotEquals, MatchWildcard
      };

      /// <summary>A number's comparisons, the one it opens on first.</summary>
      private static readonly int[] NumberMatches = { MatchGreaterThan, MatchLessThan, MatchEquals, MatchNotEquals };

      /// <summary>Whether the field's value is a number: the message size or the delivery attempt.</summary>
      public static bool IsNumeric(int field) => field == MessageSize || field == DeliveryAttempts;

      /// <summary>Whether the comparison is one a number has.</summary>
      public static bool IsNumberMatch(int match)
         => match == MatchGreaterThan || match == MatchLessThan || match == MatchEquals || match == MatchNotEquals;

      /// <summary>
      /// The comparisons the field offers. A number's four; for every other field all
      /// eight. <paramref name="stored"/> is the comparison a criterion was saved with,
      /// kept in the list when the field does not offer it - a size saved with
      /// "contains" before this dialog asked for a number - so that opening the
      /// criterion shows what it is and Save does not change it unasked.
      /// </summary>
      public static IReadOnlyList<int> MatchesFor(int field, int? stored = null)
      {
         if (!IsNumeric(field))
            return AllMatches;

         var matches = new List<int>(NumberMatches);
         if (stored is int kept && !matches.Contains(kept) && System.Array.IndexOf(AllMatches, kept) >= 0)
            matches.Add(kept);
         return matches;
      }

      /// <summary>The comparison a new criterion on the field opens on.</summary>
      public static int DefaultMatch(int field) => IsNumeric(field) ? MatchGreaterThan : MatchContains;

      /// <summary>The value's caption, with the unit of a number: Alt+V in each.</summary>
      public static string ValueCaption(int field) => field switch
      {
         MessageSize => L("_Value (bytes)"),
         DeliveryAttempts => L("_Value (attempts)"),
         _ => L("_Value")
      };

      /// <summary>What the value of a number is, under its box; null for every other field.</summary>
      public static string ValueHint(int field) => field switch
      {
         MessageSize => L("The size of the whole message, attachments included: 1048576 bytes is 1 MB."),
         DeliveryAttempts => L("1 on the first delivery attempt, 2 on the first retry, and so on."),
         _ => null
      };

      /// <summary>Why the header name cannot be saved, or null: a custom header needs one.</summary>
      public static string HeaderError(int field, string header)
         => field == CustomHeader && string.IsNullOrWhiteSpace(header) ? L("Enter a header name.") : null;

      /// <summary>
      /// Why the value cannot be saved, or null. A number compared as one takes a whole
      /// number of no sign; any other value may be empty only where "equals" or "does
      /// not equal" asks whether the field is empty.
      /// </summary>
      public static string ValueError(int field, int match, string value)
      {
         string text = value ?? "";

         if (IsNumeric(field) && IsNumberMatch(match))
         {
            if (long.TryParse(text.Trim(), NumberStyles.None, CultureInfo.InvariantCulture, out _))
               return null;

            return field == MessageSize
               ? L("Enter the size as a whole number of bytes, such as 1048576 for 1 MB.")
               : L("Enter the number of the attempt as a whole number, such as 3.");
         }

         if (text.Length == 0 && match != MatchEquals && match != MatchNotEquals)
            return L("Enter a value.");

         return null;
      }

      /// <summary>
      /// The value as it is stored: a number as the number, without the spaces around
      /// it and without leading zeros, since the server compares "equals" and "does not
      /// equal" as text, against the size or the attempt it writes with %d and %u
      /// (RuleApplier) - " 3" is not "3", and neither is "03", so that "Delivery attempts
      /// equals 03" matched nothing and "does not equal 03" every message; any other
      /// value as typed, where a space can be what is matched.
      /// </summary>
      public static string StoredValue(int field, int match, string value)
      {
         if (!IsNumeric(field) || !IsNumberMatch(match))
            return value ?? "";

         string trimmed = (value ?? "").Trim();
         return long.TryParse(trimmed, NumberStyles.None, CultureInfo.InvariantCulture, out long number)
            ? number.ToString(CultureInfo.InvariantCulture)
            : trimmed;
      }

      /// <summary>The description under the dialog's heading: the rule the criterion belongs to.</summary>
      public static string Description(string ruleName, bool adding)
      {
         if (string.IsNullOrWhiteSpace(ruleName))
            return null;

         return adding ? F("Add a criterion to the rule “{0}”.", ruleName) : F("A criterion of the rule “{0}”.", ruleName);
      }

      /// <summary>The same for an action.</summary>
      public static string ActionDescription(string ruleName, bool adding)
      {
         if (string.IsNullOrWhiteSpace(ruleName))
            return null;

         return adding ? F("Add an action to the rule “{0}”.", ruleName) : F("An action of the rule “{0}”.", ruleName);
      }
   }
}
