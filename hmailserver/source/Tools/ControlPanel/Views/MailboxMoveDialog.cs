// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using hMailServer.ControlPanel.Services;
using hMailServer.ControlPanel.Views.Scaffold;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Views
{
   /// <summary>
   /// Renaming a mailbox, and moving one to another domain on this server.
   ///
   /// The account dialog's Address box has always renamed a mailbox WITHIN its
   /// domain, and that is all it can do: the account's domain is decided by the
   /// domain it is listed under, and the box is checked against it. This is the
   /// other half, and the honest one - the whole operation, the one that carries
   /// the message store, the Sieve tree, the aliases that point at the account,
   /// its distribution list memberships, the recipients of mail still in the
   /// queue and its quarantined messages, and that leaves the old address behind
   /// as an alias so that nothing stops arriving.
   ///
   /// It asks the server first. Account.InUseReason says whether anything is
   /// holding the mailbox - a POP3 session from authentication until its socket
   /// closes, an IMAP session while it has one of the account's folders
   /// selected - and while something is, the move would be refused, so the
   /// button is disabled and the reason is shown rather than letting somebody
   /// press it and read an error.
   ///
   /// Everything goes through Account.MoveTo, which is one COM call that either
   /// completes or leaves the account as it was. There is no Save: the mailbox
   /// has moved by the time it returns.
   /// </summary>
   public class MailboxMoveDialog : FluentDialogWindow
   {
      private readonly string address_;

      private readonly Wpf.Ui.Controls.TextBox newAddress_ = new();
      private readonly CheckBox keepAlias_ = new() { Content = L("_Keep the old address as an alias"), IsChecked = true };
      private readonly InlineNotice notice_ = DialogFields.Notice();
      private readonly Wpf.Ui.Controls.Button move_;

      private readonly FieldRow newAddressRow_;

      /// <summary>The address the mailbox now has, once it has been moved.</summary>
      public string MovedTo { get; private set; }

      public MailboxMoveDialog(Window owner, string address)
      {
         address_ = address ?? "";

         Owner = owner;
         Title = F("Move or rename {0}", address_);

         var body = new StackPanel();
         body.Children.Add(notice_);

         newAddress_.Text = address_;
         newAddressRow_ = DialogFields.Field(L("_New address (it may be at any domain this server holds)"), newAddress_);
         body.Children.Add(newAddressRow_);

         body.Children.Add(DialogFields.Check(keepAlias_));

         move_ = new Wpf.Ui.Controls.Button
         {
            Content = L("_Move"),
            Appearance = Wpf.Ui.Controls.ControlAppearance.Primary,
            MinWidth = 88
         };
         AutomationProperties.SetAutomationId(move_, "mailbox-move");
         move_.Click += (s, e) => Move();

         var cancel = new Wpf.Ui.Controls.Button { Content = L("Cancel"), MinWidth = 88 };
         cancel.Click += (s, e) => Close();

         UseFrame(address_, body, move_, cancel,
            L("The mailbox, its messages and everything keyed on the account go with it: its rules, filters, folders and their permissions, application passwords, second factor, contacts, calendars, preferences, password history, sender lists, group memberships and its role."),
            width: 560);

         Loaded += (s, e) => ShowWhatWouldStopIt();
      }

      private dynamic OpenAccount()
      {
         dynamic domains = ServerSession.Current.Application.Domains;
         dynamic domain = domains.ItemByName[address_.Substring(address_.IndexOf('@') + 1)];
         dynamic accounts = domain.Accounts;
         return accounts.ItemByAddress[address_];
      }

      /// <summary>
      /// Asked before the move rather than after: a mailbox somebody is reading
      /// would be refused, and a button that can only fail is worse than one
      /// that is not there.
      /// </summary>
      private void ShowWhatWouldStopIt()
      {
         try
         {
            string reason = (string)OpenAccount().InUseReason ?? "";

            if (string.IsNullOrWhiteSpace(reason))
               return;

            notice_.Show(StatusLevel.Warning, F("This mailbox cannot be moved at the moment: {0}", reason));
            move_.IsEnabled = false;
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            // A server that cannot answer the question is not a reason to refuse
            // the move: MoveTo asks it again and refuses there, with the same
            // sentence, where it can be shown properly.
         }
      }

      private void Move()
      {
         notice_.Hide();
         newAddressRow_.Error = null;

         string wanted = (newAddress_.Text ?? "").Trim();

         if (wanted.Length == 0)
         {
            newAddressRow_.Error = L("A new address is required.");
            newAddress_.Focus();
            return;
         }

         try
         {
            OpenAccount().MoveTo(wanted, keepAlias_.IsChecked == true);
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            // Everything the server refuses arrives here as a sentence, and every
            // one of those refusals happens before anything is changed.
            notice_.Show(StatusLevel.Critical, F("The mailbox could not be moved. {0}", ServerSession.DescribeComError(fatalCheck)));
            return;
         }

         MovedTo = wanted;
         Dialogs.Info(F("{0} has been moved to {1}.", address_, wanted));
         DialogResult = true;
         Close();
      }
   }
}
