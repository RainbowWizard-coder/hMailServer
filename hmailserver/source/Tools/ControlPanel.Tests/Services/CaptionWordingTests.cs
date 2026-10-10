// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// A field's caption names the field and, in its last parenthesis, the unit, the range
   /// and what an edge value means - "Delete messages older than (days; 0 = no policy)" -
   /// and nothing else: what the field is for, how to fill it in and what follows from
   /// it are the field's hint, which FieldRow draws under the editor and gives the editor
   /// as its help text (review finding F105). The caption is the editor's accessible
   /// name, so a screen reader read "Address (changing it renames the mailbox; it must
   /// stay in this domain)" as the field's name, and the domain dialog's limits, country
   /// blocking and restricted delivery carried whole sentences in brackets.
   ///
   /// The rule, for a parenthesis with a ";" or an "=" in it: each part between "; " is a
   /// unit or a range ("MB", "% of the limit", "minutes, up to 525,600", "1 to 20", "up
   /// to 100 characters") or what values mean ("0 = no limit", "-1 = the server's, 0 =
   /// never", "empty = the server message"), each meaning five words at most.
   /// </summary>
   public class CaptionWordingTests
   {
      private const string Unit =
         @"(?:KB|MB|GB|KiB|PEM|bytes|%|% of (?:the|its|a) [\w' ]+|% of it|days?|hours?|minutes?|seconds?|milliseconds?|ms|" +
         @"characters?|messages?|recipients?|entries|addresses|optional|comma[- ]separated|24-hour HH:MM|host:port)";
      private const string Number = @"-?[\d,.]+";
      private static readonly string Range = $@"(?:up to {Number}|at most {Number}|at least {Number}|{Number} to {Number})";
      internal static readonly Regex UnitPart = new Regex($@"^(?:{Unit}(?:, {Range})?|{Range}(?: {Unit})?)$");
      private const string Value = @"(?:-?[\d,.]+(?: or -?[\d,.]+)?|empty|\*|-)";
      // A meaning may hold a number whose thousands are grouped: "0 = the built-in 210,000".
      private const string Meaning = @"(?:[^=;,()]|,(?=\d{3}))+";
      internal static readonly Regex ValuePart = new Regex($@"^{Value} = {Meaning}(?:, {Value} = {Meaning})*$");
      private static readonly Regex Meanings = new Regex($@"{Value} = ({Meaning})");

      // The caption positions: a FieldRow's Label, the dialogs' Field, Label and ReadOnlyField
      // helpers, the hMailServer.ini pages' Labelled*_ and a list editor's FieldSpec.
      internal static readonly Regex CaptionInCode = new Regex(
         @"\b(?:Label\s*=\s*|(?:DialogFields\.)?(?:Field|ReadOnlyField|FormField_|Captioned|Label|LabelledBox_|LabelledCombo_)\(\s*|LabelledCheck_\(\s*\w+\s*,\s*)L\(\s*""((?:[^""\\]|\\.)*)""");

      /// <summary>Why a caption breaks the rule, or null when it keeps it.</summary>
      internal static string Problem(string caption)
      {
         Match last = Regex.Match(caption.Replace("_", ""), @"\(([^()]*)\)\s*$");
         if (!last.Success)
            return null;
         string inner = last.Groups[1].Value;
         if (!inner.Contains(';') && !inner.Contains('='))
            return null;
         foreach (string part in inner.Split("; "))
         {
            if (ValuePart.IsMatch(part))
            {
               if (Meanings.Matches(part).Any(m => m.Groups[1].Value.Split(' ', StringSplitOptions.RemoveEmptyEntries).Length > 5))
                  return "a meaning of more than five words: " + part;
               continue;
            }

            if (!UnitPart.IsMatch(part))
               return "not a unit, a range or what a value means: " + part;
         }

         return null;
      }

      [Theory]
      [InlineData("Maximum _domain size (MB; 0 = no limit)")]
      [InlineData("_Warn the owner at (% of the limit; -1 = the server's, 0 = never)")]
      [InlineData("Delete messages in this domain's mailboxes _older than (days; 0 = no policy)")]
      [InlineData("Warning te_xt (empty = the server message)")]
      [InlineData("Tag te_xt (up to 100 characters; empty = [EXTERNAL])")]
      [InlineData("Send it during this _hour of the local day (0 to 23)")]
      [InlineData("_Recipient wildcard (* = everyone)")]
      [InlineData("_Delete threshold override (-1 or 0 = off)")]
      [InlineData("Expected token issuer (iss)")]
      [InlineData("_Domain (e.g. gmail.com)")]
      public void ACaption_ThatSaysTheUnitAndWhatAValueMeans_KeepsTheRule(string caption)
      {
         Assert.Null(Problem(caption));
      }

      [Theory]
      [InlineData("_Address (changing it renames the mailbox; it must stay in this domain)")]
      [InlineData("Delete messages in this domain's mailboxes _older than (days; 0 = no policy, an account's own value overrides it)")]
      [InlineData("Warning te_xt (empty = the server message; %MACRO_MAILBOX%, %MACRO_QUOTA%, %MACRO_USED%, %MACRO_PERCENT%)")]
      [InlineData("_Active Directory domain (for AD-synchronised domains; optional)")]
      [InlineData("Tag te_xt (empty = [EXTERNAL], at most 100 characters)")]
      public void ACaption_ThatCarriesAnExplanation_BreaksIt(string caption)
      {
         Assert.NotNull(Problem(caption));
      }

      [Fact]
      public void TheDialogs_CaptionTheirFields_AndSayTheRestInTheHint()
      {
         var offenders = new List<string>();
         int read = 0;
         foreach ((string relative, string source) in ConfirmationPatternTests.SourceFiles())
         {
            string name = Path.GetFileName(relative);
            if (!name.EndsWith("Dialog.cs", StringComparison.Ordinal) && name != "CollectionSpecs.cs")
               continue;
            foreach (Match match in CaptionInCode.Matches(source))
            {
               read++;
               string caption = match.Groups[1].Value.Replace("\\\"", "\"").Replace("\\\\", "\\");
               string problem = Problem(caption);
               if (problem != null)
                  offenders.Add(relative + ": " + caption + " - " + problem);
            }
         }

         Assert.True(read > 150, "Only " + read + " captions were read from the dialogs; the pattern no longer finds them.");
         Assert.True(offenders.Count == 0, "These captions explain their field; say it in the hint:\n" + string.Join("\n", offenders));
      }

      [Fact]
      public void TheDomainAndAccountDialogs_SayWhatTheirCaptionsSaidInTheHint()
      {
         string domain = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "DomainDialog.cs"));
         string account = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "AccountDialog.cs"));

         Assert.Contains("Label(L(\"_Address\"), addressBox_).WithHint(L(\"Changing it renames the mailbox; it must stay in this domain.\"));", account, StringComparison.Ordinal);
         Assert.Contains("Label(L(\"Delete messages in this domain's mailboxes _older than (days; 0 = no policy)\"), retentionDays_).WithHint(L(\"An account's own value overrides it.\"));", domain, StringComparison.Ordinal);
         Assert.Contains(".WithHint(L(\"It may use %MACRO_MAILBOX%, %MACRO_QUOTA%, %MACRO_USED% and %MACRO_PERCENT%.\"))", domain, StringComparison.Ordinal);
      }
   }
}
