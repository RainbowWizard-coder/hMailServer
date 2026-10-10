// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Linq;
using System.Windows;
using System.Windows.Controls;
using hMailServer.ControlPanel.Services;
using hMailServer.ControlPanel.Views.Scaffold;
using MessageBox = hMailServer.ControlPanel.Views.Dialogs;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Views
{
   /// <summary>Add/edit a single rule criterion (predefined field or custom header, match type, value), on the standard frame.</summary>
   public class RuleCriteriaDialog : FluentDialogWindow
   {
      private readonly int ruleId_;
      private readonly int criteriaId_; // 0 = new
      private readonly Func<dynamic> rulesProvider_;

      private readonly ComboBox field_ = new();
      private readonly TextBox header_ = new();
      private readonly FieldRow headerRow_;
      private readonly ComboBox match_ = new();
      private readonly TextBox value_ = new();
      private readonly FieldRow valueRow_;
      private readonly InlineNotice notice_ = DialogFields.Notice();
      private readonly DialogFrame frame_;

      /// <summary>The comparison the criterion was saved with, kept in the list a number's field offers (RuleCriterionInput.MatchesFor).</summary>
      private int? storedMatch_;

      public RuleCriteriaDialog(Window owner, int ruleId, int criteriaId, Func<dynamic> rulesProvider = null)
      {
         ruleId_ = ruleId;
         criteriaId_ = criteriaId;
         rulesProvider_ = rulesProvider ?? (() => ServerSession.Current.Application.Rules);
         Owner = owner;
         Title = criteriaId == 0 ? L("Add criterion") : L("Edit criterion");

         var body = new StackPanel();
         body.Children.Add(notice_);

         body.Children.Add(Label(L("_Field"), field_));
         field_.Items.Add(DialogFields.Combo(L("From"), 1));
         field_.Items.Add(DialogFields.Combo(L("To"), 2));
         field_.Items.Add(DialogFields.Combo(L("CC"), 3));
         field_.Items.Add(DialogFields.Combo(L("Subject"), 4));
         field_.Items.Add(DialogFields.Combo(L("Body"), 5));
         field_.Items.Add(DialogFields.Combo(L("Message size"), 6));
         field_.Items.Add(DialogFields.Combo(L("Recipient list"), 7));
         field_.Items.Add(DialogFields.Combo(L("Delivery attempts"), 8));
         field_.Items.Add(DialogFields.Combo(L("Attachment name"), 9));
         // Outbound content policy: each a list with ";" between its entries,
         // so "contains" reads as "any of them is".
         field_.Items.Add(DialogFields.Combo(L("Sensitive content found (card_number, iban, account_number)"), 10));
         field_.Items.Add(DialogFields.Combo(L("Attachment type by its bytes (executable, pdf, zip, office, text…)"), 11));
         field_.Items.Add(DialogFields.Combo(L("External recipients (addresses this server does not host)"), 12));
         field_.Items.Add(DialogFields.Combo(L("Readable text (body and text attachments)"), 13));
         field_.Items.Add(DialogFields.Combo(L("Custom header…"), 0));
         field_.SelectionChanged += (s, e) => FieldChanged_();

         headerRow_ = DialogFields.Field(L("_Header name"), header_, F("Such as {0}", "X-Spam-Status")); // no-loc: a header name, the same in every language
         body.Children.Add(headerRow_);

         body.Children.Add(Label(L("_Match type"), match_));

         valueRow_ = Label(L("_Value"), value_);
         body.Children.Add(valueRow_);

         // An error goes when what it is about is edited.
         header_.TextChanged += (s, e) => headerRow_.Error = null;
         value_.TextChanged += (s, e) => valueRow_.Error = null;
         match_.SelectionChanged += (s, e) => valueRow_.Error = null;

         // Enter saves, Escape cancels.
         var save = new Wpf.Ui.Controls.Button { Content = L("_Save"), Appearance = Wpf.Ui.Controls.ControlAppearance.Primary, MinWidth = 88 };
         save.Click += (s, e) => Save();
         var cancel = new Wpf.Ui.Controls.Button { Content = L("Cancel"), MinWidth = 88 };
         cancel.Click += (s, e) => Close();

         // Upper-case deliberately, sentence-case sweep notwithstanding: "IF" is
         // the rule grammar's keyword, not prose. RulesView's editor panes carry
         // the same IF/THEN pair, and this dialog edits one clause of it. The
         // description under it names the rule (review finding F227), once Load has
         // read it.
         frame_ = UseFrame(L("IF"), body, save, cancel, width: DesignTokens.Dialog.DefaultWidth);

         // Room for the message a press puts under the header name or the value, one
         // at a time: the longest, a size that is not a whole number of bytes, wraps
         // to two lines at this width in French (review finding F213).
         KeepRoomForFieldErrors(2);
         Loaded += (s, e) => Load();
      }

      /// <summary>
      /// The comparisons the chosen field offers, the value's caption with its unit and
      /// the line that says what a number counts (RuleCriterionInput): a field that is
      /// a number offers a number's four comparisons, and keeps the one it showed when
      /// that is among them, or takes "is greater than"; a field of text offers all
      /// eight and keeps the one shown.
      /// </summary>
      private void FieldChanged_()
      {
         int field = DialogFields.ComboValue(field_);
         headerRow_.Visibility = field == RuleCriterionInput.CustomHeader ? Visibility.Visible : Visibility.Collapsed;
         headerRow_.Error = null;

         int shown = match_.SelectedItem is ComboBoxItem ? DialogFields.ComboValue(match_) : 0;
         var offered = RuleCriterionInput.MatchesFor(field, storedMatch_);

         match_.Items.Clear();
         foreach (int match in offered)
            match_.Items.Add(DialogFields.Combo(MatchWord_(match), match));

         int keep = shown != 0 && offered.Contains(shown) ? shown : RuleCriterionInput.DefaultMatch(field);
         DialogFields.SelectCombo(match_, keep, orFirst: true);

         valueRow_.Label = RuleCriterionInput.ValueCaption(field);
         valueRow_.Hint = RuleCriterionInput.ValueHint(field);
         valueRow_.Error = null;
      }

      /// <summary>A comparison's words, as the list has always said them.</summary>
      private static string MatchWord_(int match) => match switch
      {
         RuleCriterionInput.MatchEquals => L("equals"),
         RuleCriterionInput.MatchContains => L("contains"),
         RuleCriterionInput.MatchLessThan => L("is less than"),
         RuleCriterionInput.MatchGreaterThan => L("is greater than"),
         RuleCriterionInput.MatchRegex => L("matches regex"),
         RuleCriterionInput.MatchNotContains => L("does not contain"),
         RuleCriterionInput.MatchNotEquals => L("does not equal"),
         _ => L("matches wildcard")
      };

      /// <summary>The rule's name, for the description; null when it cannot be read, which only leaves the dialog undescribed.</summary>
      private string RuleName_()
      {
         dynamic rules = null;
         dynamic rule = null;
         try
         {
            rules = rulesProvider_();
            rule = rules.ItemByDBID[ruleId_];
            return rule == null ? null : (string)rule.Name;
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            return null;
         }
         finally
         {
            ServerSession.Release((object)rule);
            ServerSession.Release((object)rules);
         }
      }

      private void Load()
      {
         frame_.Description = RuleCriterionInput.Description(RuleName_(), criteriaId_ == 0);

         if (criteriaId_ == 0)
         {
            DialogFields.SelectCombo(field_, 1);
            FieldChanged_();
            return;
         }

         dynamic rules = rulesProvider_();
         try
         {
            dynamic rule = rules.ItemByDBID[ruleId_];
            if (rule == null) { Close(); return; }
            dynamic criterias = rule.Criterias;
            try
            {
               dynamic c = criterias.ItemByDBID[criteriaId_];
               // Before the field is chosen, so that the list a number's field offers
               // keeps the comparison the criterion was saved with.
               storedMatch_ = (int)c.MatchType;
               if ((bool)c.UsePredefined)
                  DialogFields.SelectCombo(field_, (int)c.PredefinedField);
               else
               {
                  DialogFields.SelectCombo(field_, 0);
                  header_.Text = (string)c.HeaderField ?? "";
               }
               FieldChanged_();
               DialogFields.SelectCombo(match_, (int)c.MatchType);
               value_.Text = (string)c.MatchValue ?? "";
               ServerSession.Release(c);
            }
            finally
            {
               ServerSession.Release(criterias);
            }
            ServerSession.Release(rule);
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            MessageBox.Show(F("Could not load the criterion: {0}", ServerSession.DescribeComError(ex)), L("Control Panel"));
            Close();
            return;
         }
         finally
         {
            ServerSession.Release(rules);
         }

         // What was read is not an edit to answer with an error.
         DialogFields.ClearErrors(headerRow_, valueRow_);
      }

      private void Save()
      {
         notice_.Hide();
         DialogFields.ClearErrors(headerRow_, valueRow_);
         int field = DialogFields.ComboValue(field_);
         int match = DialogFields.ComboValue(match_);

         // On the field at fault, before anything is written: a custom header with no
         // name matches nothing, and an empty value matches every message or none
         // (review finding F227).
         if (RuleCriterionInput.HeaderError(field, header_.Text) is string headerError)
         {
            DialogFields.ShowError(headerRow_, headerError);
            return;
         }

         if (RuleCriterionInput.ValueError(field, match, value_.Text) is string valueError)
         {
            DialogFields.ShowError(valueRow_, valueError);
            return;
         }

         dynamic rules = rulesProvider_();
         try
         {
            dynamic rule = rules.ItemByDBID[ruleId_];
            if (rule == null) { Close(); return; }
            dynamic criterias = rule.Criterias;
            try
            {
               dynamic c = criteriaId_ == 0 ? criterias.Add() : criterias.ItemByDBID[criteriaId_];
               c.RuleID = ruleId_;
               if (field == RuleCriterionInput.CustomHeader)
               {
                  c.UsePredefined = false;
                  c.HeaderField = header_.Text.Trim();
               }
               else
               {
                  c.UsePredefined = true;
                  c.PredefinedField = field;
               }
               c.MatchType = match;
               c.MatchValue = RuleCriterionInput.StoredValue(field, match, value_.Text);
               c.Save();
               ServerSession.Release(c);
            }
            finally
            {
               ServerSession.Release(criterias);
            }
            rule.Save();
            ServerSession.Release(rule);
            Close();
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            notice_.Show(StatusLevel.Critical, F("Could not save the criterion: {0}", ServerSession.DescribeComError(ex)));
         }
         finally
         {
            ServerSession.Release(rules);
         }
      }

      /// <summary>
      /// A caption and its editor as one row, which names the editor to UI
      /// Automation - without it this dialog announced itself as "combo box,
      /// edit, combo box, edit".
      /// </summary>
      private static FieldRow Label(string text, FrameworkElement editor) => DialogFields.Field(text, editor);
   }
}
