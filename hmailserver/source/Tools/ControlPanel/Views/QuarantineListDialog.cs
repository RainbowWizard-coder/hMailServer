// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Linq;
using System.Windows;
using System.Windows.Controls;
using hMailServer.ControlPanel.Services;
using hMailServer.ControlPanel.Views.Scaffold;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Views
{
   /// <summary>
   /// The question Allow sender and Block sender on the Quarantine page ask before
   /// they change anything: which list. The sender's address on the server's allow
   /// and block list, or the organisation's domain, of the sender or, when the page
   /// is grouped by HELO domain, of the HELO name, on the list of allowed and blocked
   /// domains. The two are read at different moments and do different things, and the
   /// dialog says which.
   ///
   /// What would be written is asked of the server itself (a dry run of
   /// Quarantine.Bulk) and listed before the button is pressed: one entry per
   /// distinct sender or domain, whether each is new or already there, and the
   /// messages no entry can be made for. A domain that is itself a public suffix
   /// is written only after a box says so. Block then deletes, so for it the
   /// keyboard default is Cancel, as for every delete on this page.
   /// </summary>
   public class QuarantineListDialog : FluentDialogWindow
   {
      private readonly bool block_;
      private readonly IReadOnlyList<int> ids_;
      private readonly bool heloGrouping_;
      private readonly Func<string, BulkAnswer> bulk_;

      private readonly RadioButton address_ = new() { GroupName = "list" };
      private readonly RadioButton domain_ = new() { GroupName = "list", Margin = new Thickness(0, 12, 0, 0) };
      private readonly TextBlock plan_ = new() { TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 16, 0, 0) };
      private readonly CheckBox suffixOk_ = new() { VerticalAlignment = VerticalAlignment.Center };
      private readonly Wpf.Ui.Controls.Button primary_;

      // What an allowed address does not stop: a message the server-wide spam score
      // holds during the SMTP conversation is held still - how most messages here
      // were held - which the address's note said in its last sentence alone, under
      // the option the dialog chose for them (review finding F262).
      private readonly InlineNotice stillHeld_ = new() { Level = StatusLevel.Warning, Visibility = Visibility.Collapsed, Margin = new Thickness(28, 8, 0, 0) };

      // What the server's answer says that is not the plan: the messages no entry can
      // be made for, a public suffix among the entries - with the box that lists it
      // anyway - and a preview that could not be made, with Try again. Each was a line
      // of the plan's plain text, the Primary disabled with no reason given.
      private readonly InlineNotice refused_ = new() { Level = StatusLevel.Warning, Visibility = Visibility.Collapsed, Margin = new Thickness(0, 12, 0, 0) };
      private readonly InlineNotice suffix_ = new() { Level = StatusLevel.Warning, Visibility = Visibility.Collapsed, Margin = new Thickness(0, 12, 0, 0) };
      private readonly InlineNotice failure_ = new() { Level = StatusLevel.Critical, Visibility = Visibility.Collapsed, Margin = new Thickness(0, 12, 0, 0) };

      private bool planUsable_;

      /// <summary>address or domain: the list the answer names.</summary>
      public string ListWord => domain_.IsChecked == true ? "domain" : "address";

      /// <summary>True when a public suffix is among the entries and the box that allows it is ticked.</summary>
      public bool ConfirmPublicSuffix => suffix_.Visibility == Visibility.Visible && suffixOk_.IsChecked == true;

      public QuarantineListDialog(Window owner, bool block, IReadOnlyList<int> ids, bool heloGrouping, Func<string, BulkAnswer> bulk)
         : this(owner, block, ids, heloGrouping, bulk, false, null)
      {
      }

      /// <param name="heldByScore">
      /// Whether a message among them was held by the server-wide spam score
      /// (QuarantineReview.HeldByServerScore): allowing it then takes the domain list,
      /// which stops that, where the address list does not.
      /// </param>
      /// <param name="deleteThreshold">The server's delete threshold, which the note under the address says; null when it could not be read.</param>
      public QuarantineListDialog(Window owner, bool block, IReadOnlyList<int> ids, bool heloGrouping, Func<string, BulkAnswer> bulk,
         bool heldByScore, int? deleteThreshold)
      {
         Owner = owner;
         block_ = block;
         ids_ = ids ?? Array.Empty<int>();
         heloGrouping_ = heloGrouping;
         bulk_ = bulk;

         Title = block ? L("Block and delete") : L("Allow and release");

         address_.Content = L("The sender's _address, on the server's allow and block list");
         domain_.Content = heloGrouping
            ? L("The _HELO name's domain, on the list of allowed and blocked domains")
            : L("The sender's _domain, on the list of allowed and blocked domains");

         // Allowing messages the server-wide score held: the list that stops the next
         // one, where the address was chosen whatever had held them.
         if (!block && heldByScore)
            domain_.IsChecked = true;
         else
            address_.IsChecked = true;

         var addressNote = Note_(block
            ? L("Read at delivery, for every recipient: a blocked sender's mail is held for them where the quarantine is on, and marked as spam where it is not.")
            : L("Read at delivery, for every recipient: an allowed sender's mail is delivered unmarked, and the accounts' own spam thresholds do not run. A message the server-wide spam score holds during the SMTP conversation is still held; the domain list is the one that stops that."));

         var domainNote = Note_(block
            ? L("Read at MAIL FROM: a blocked domain is refused with 550 before anything else runs, for every recipient. The entry covers the organisation's domain and everything beneath it.")
            : L("Read at MAIL FROM: an allowed domain is not greylisted, and not refused or held on the spam score, while every test still runs and the mark threshold still marks. The entry covers the organisation's domain and everything beneath it."));

         stillHeld_.Text = deleteThreshold > 0
            ? F("This will not stop the next message from this sender being held if it scores {0} or more.", deleteThreshold.Value)
            : L("This will not stop the next message from this sender being held by the server-wide spam score.");
         System.Windows.Automation.AutomationProperties.SetAutomationId(stillHeld_, "quarantine-list-still-held");

         // The public suffix's warning carries the box that lists it anyway.
         suffixOk_.Content = L("_List it anyway");
         suffixOk_.Click += (_, _) => UpdatePrimary_();
         suffix_.Content = suffixOk_;
         System.Windows.Automation.AutomationProperties.SetAutomationId(suffix_, "quarantine-list-suffix");
         System.Windows.Automation.AutomationProperties.SetAutomationId(refused_, "quarantine-list-refused");
         System.Windows.Automation.AutomationProperties.SetAutomationId(failure_, "quarantine-list-failure");

         address_.Checked += (_, _) => RefreshPlan_();
         domain_.Checked += (_, _) => RefreshPlan_();

         var body = new StackPanel();
         body.Children.Add(address_);
         body.Children.Add(addressNote);
         body.Children.Add(stillHeld_);
         body.Children.Add(domain_);
         body.Children.Add(domainNote);
         body.Children.Add(plan_);
         body.Children.Add(refused_);
         body.Children.Add(suffix_);
         body.Children.Add(failure_);

         primary_ = new Wpf.Ui.Controls.Button
         {
            Content = block ? L("_Block and delete") : L("Allo_w and release"),
            Appearance = block ? Wpf.Ui.Controls.ControlAppearance.Danger : Wpf.Ui.Controls.ControlAppearance.Primary,
            MinWidth = 88
         };
         primary_.Click += (_, _) => { DialogResult = true; Close(); };

         var cancel = new Wpf.Ui.Controls.Button { Content = L("Cancel"), MinWidth = 88 };
         cancel.Click += (_, _) => { DialogResult = false; Close(); };

         string heading = block
            ? (ids_.Count == 1 ? L("Block the sender of this message, then delete it")
               : Plural(ids_.Count, "Block the senders of {0} message, then delete it", "Block the senders of {0} messages, then delete them"))
            : (ids_.Count == 1 ? L("Allow the sender of this message, then release it")
               : Plural(ids_.Count, "Allow the senders of {0} message, then release it", "Allow the senders of {0} messages, then release them"));

         UseFrame(heading, body, primary_, cancel,
            description: L("Which list? One entry is written for each distinct sender or domain, and then each message is acted on. A message no entry can be made for is left where it is."),
            width: DesignTokens.Dialog.WideWidth);

         // A block deletes: Enter declines, as it does for every delete on the
         // Quarantine page.
         if (block)
         {
            primary_.IsDefault = false;
            cancel.IsDefault = true;
         }

         Loaded += (_, _) => RefreshPlan_();
      }

      private static TextBlock Note_(string text)
      {
         var note = new TextBlock { Text = text, TextWrapping = TextWrapping.Wrap, Margin = new Thickness(28, 2, 0, 0) };
         note.SetResourceReference(TextBlock.ForegroundProperty, "TextFillColorSecondaryBrush");
         return note;
      }

      private string OutcomeText_(string outcome)
      {
         return outcome switch
         {
            "add" => L("will be added"),
            "change" => block_ ? L("is on the list the other way, and will be changed to block") : L("is on the list the other way, and will be changed to allow"),
            "unchanged" => L("is already on the list"),
            _ => outcome
         };
      }

      /// <summary>Asks the server what this choice would write, and says so.</summary>
      private void RefreshPlan_()
      {
         planUsable_ = false;
         suffix_.Visibility = Visibility.Collapsed;
         suffixOk_.IsChecked = false;
         refused_.Visibility = Visibility.Collapsed;
         failure_.Visibility = Visibility.Collapsed;
         failure_.Content = null;
         stillHeld_.Visibility = !block_ && address_.IsChecked == true ? Visibility.Visible : Visibility.Collapsed;

         try
         {
            string list = ListWord;
            BulkAnswer answer = bulk_(QuarantineReview.BulkRequest(ids_, block_ ? "block" : "allow", list,
               list == "domain" ? (heloGrouping_ ? "helo" : "sender") : null, dryRun: true));

            if (answer == null)
            {
               Plan_(null);
               Failure_(L("This server is older than the Control Panel and cannot allow or block a sender from the quarantine. Upgrade the server."), false);
               UpdatePrimary_();
               return;
            }

            var lines = new List<string>();

            if (answer.Entries.Count > 0)
            {
               lines.Add(L("What will be written:"));
               lines.AddRange(answer.Entries.Take(12).Select(e => "  " + F("{0} - {1}", e.Value, OutcomeText_(e.Outcome))));

               if (answer.Entries.Count > 12)
                  lines.Add("  " + F("and {0} more", answer.Entries.Count - 12));
            }

            List<BulkResult> refused = answer.Results.Where(r => !r.Ok).ToList();
            if (refused.Count > 0)
            {
               refused_.Text = refused.Count == 1
                  ? F("1 message cannot be listed and will be left where it is: {0}", refused[0].Error)
                  : PluralF(refused.Count,
                     "{0} message cannot be listed and will be left where it is. The first reason: {1}",
                     "{0} messages cannot be listed and will be left where they are. The first reason: {1}",
                     refused.Count, refused[0].Error);
               refused_.Visibility = Visibility.Visible;
            }

            // Nothing to write and nothing refused: say so, where the plan was blank
            // over a Primary that could not be pressed.
            if (answer.Entries.Count == 0 && refused.Count == 0)
               lines.Add(L("No entry can be written for these messages."));

            List<string> suffixes = answer.Entries.Where(e => e.PublicSuffix).Select(e => e.Value).ToList();
            if (suffixes.Count > 0)
            {
               suffix_.Text = F("{0} is a public suffix. An entry for it covers every domain registered beneath it, which is almost never what is meant.", string.Join(", ", suffixes));
               suffix_.Visibility = Visibility.Visible;
            }

            Plan_(lines);
            planUsable_ = answer.Entries.Count > 0;
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            // The preview could not be made: Critical, with Try again, where the reason
            // stood alone in the plan's place.
            Plan_(null);
            Failure_(F("What would be written could not be read from the server: {0}", ServerSession.DescribeComError(ex)), true);
         }

         UpdatePrimary_();
      }

      /// <summary>The plan's lines, or none.</summary>
      private void Plan_(List<string> lines)
      {
         plan_.Text = lines == null ? "" : string.Join(Environment.NewLine, lines);
         plan_.Visibility = plan_.Text.Length > 0 ? Visibility.Visible : Visibility.Collapsed;
      }

      /// <summary>A preview that could not be made, at Critical, and Try again where another try can help.</summary>
      private void Failure_(string text, bool tryAgain)
      {
         failure_.Text = text;
         failure_.Content = tryAgain ? ReadFailure.TryAgainButton(RefreshPlan_) : null;
         failure_.Visibility = Visibility.Visible;
      }

      private void UpdatePrimary_()
      {
         primary_.IsEnabled = planUsable_ && (suffix_.Visibility != Visibility.Visible || suffixOk_.IsChecked == true);
      }
   }
}
