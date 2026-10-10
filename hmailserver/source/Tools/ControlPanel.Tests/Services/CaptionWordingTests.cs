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
   /// never", "empty = the server message"), each meaning five words at most. In a dialog, a
   /// last parenthesis with neither is a unit or a range, "optional", "leave empty to keep
   /// current", or three words at most: the domain dialog's "_Domain name (changing it renames
   /// the domain and moves every account, alias and list with it)" and the account's "_Restricted
   /// delivery (the named domains are on the domain's dialog)" had neither and were explanations.
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

      // The brackets every dialog's password box and optional field share, which say what an
      // empty box does or that it may be left so.
      private static readonly string[] SharedBrackets = { "leave empty to keep current", "optional" };

      /// <summary>
      /// Why a dialog's caption breaks the rule, or null when it keeps it: Problem's reasons, and a
      /// last bracket with no ";" or "=" that is neither a unit nor a range nor one of the shared
      /// forms and runs past three words - an explanation, "_Domain name (changing it renames the
      /// domain and moves every account, alias and list with it)", which Problem cannot see and a
      /// screen reader reads as the field's name.
      /// </summary>
      internal static string DialogProblem(string caption)
      {
         string problem = Problem(caption);
         if (problem != null)
            return problem;
         Match last = Regex.Match(caption.Replace("_", ""), @"\(([^()]*)\)\s*$");
         if (!last.Success)
            return null;
         string inner = last.Groups[1].Value;
         if (inner.Contains(';') || inner.Contains('=') || SharedBrackets.Contains(inner) || UnitPart.IsMatch(inner))
            return null;
         return inner.Split(' ', StringSplitOptions.RemoveEmptyEntries).Length > 3 ? "an explanation, which is the field's hint: " + inner : null;
      }

      [Theory]
      [InlineData("New _password (leave empty to keep current)")]
      [InlineData("_Active Directory domain (optional)")]
      [InlineData("Windows _domain (empty = a local Windows account)")]
      [InlineData("_Propagation period (hours, 1 to 720)")]
      [InlineData("Expires on (YYYY-MM-DD HH:MM:SS)")]
      [InlineData("Mirror every folder (IMAP only)")]
      [InlineData("_Domain name")]
      public void ADialogCaption_ThatSaysAUnitOrASharedForm_KeepsTheRule(string caption)
      {
         Assert.Null(DialogProblem(caption));
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
      [InlineData("_Domain name (changing it renames the domain and moves every account, alias and list with it)")]
      [InlineData("_Postmaster address (mail to unknown recipients is redirected here)")]
      [InlineData("_Restricted delivery (the named domains are on the domain's dialog)")]
      [InlineData("Windows _domain (leave empty for a local Windows account)")]
      [InlineData("_Priority (higher wins when ranges overlap)")]
      [InlineData("Expires on (yyyy-mm-dd hh:mm:ss, when it stops mattering)")]
      public void ACaption_ThatCarriesAnExplanation_BreaksIt(string caption)
      {
         Assert.NotNull(DialogProblem(caption));
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
               string problem = DialogProblem(caption);
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
         Assert.Contains("Label(L(\"_Domain name\"), name_).WithHint(L(\"Changing it renames the domain and moves every account, alias and list with it.\"));", domain, StringComparison.Ordinal);
         Assert.Contains("Label(L(\"_Postmaster address\"), postmaster_).WithHint(L(\"Mail to unknown recipients is redirected here.\"))", domain, StringComparison.Ordinal);
         Assert.Contains("Label(L(\"_Restricted delivery\"), restrictedDelivery_).WithHint(L(\"The named domains are on the domain's dialog.\"))", account, StringComparison.Ordinal);
         Assert.Contains("Label(L(\"Windows _domain (empty = a local Windows account)\"), adDomain_);", account, StringComparison.Ordinal);
         Assert.Contains(".WithHint(L(\"For example allow:GB,IE or block:CN,RU.\"))", domain, StringComparison.Ordinal);
         Assert.Contains(".WithHint(L(\"For example partner.example, or .example for the domain and everything beneath it; separate them with commas or spaces.\"))", domain, StringComparison.Ordinal);
      }

      /// <summary>
      /// A hint that moved out of a caption says what it is, as a sentence does: the domain dialog's
      /// country blocking showed "allow:GB,IE or block:CN,RU." alone under its box and the restricted
      /// delivery "partner.example, or .example for the domain ...", which a screen reader read as the
      /// editor's help text with nothing to say they were examples.
      /// </summary>
      [Fact]
      public void TheDialogsHints_BeginAsSentencesDo()
      {
         var hint = new Regex(@"(?:\.WithHint\(|\bHint = )L\(\s*""((?:[^""\\]|\\.)*)""");
         var offenders = new List<string>();
         int read = 0;
         foreach ((string relative, string source) in ConfirmationPatternTests.SourceFiles())
         {
            string name = Path.GetFileName(relative);
            if (!name.EndsWith("Dialog.cs", StringComparison.Ordinal) && name != "CollectionSpecs.cs")
               continue;
            foreach (Match match in hint.Matches(source))
            {
               read++;
               string text = match.Groups[1].Value;
               if (!(char.IsUpper(text[0]) || char.IsDigit(text[0]) || text[0] is '\'' or '"' or '\\'))
                  offenders.Add(relative + ": " + text);
            }
         }

         Assert.True(read > 30, "Only " + read + " hints were read from the dialogs.");
         Assert.True(offenders.Count == 0, "A hint begins as a sentence, \"For example ...\":\n" + string.Join("\n", offenders));
      }
   }
}
