// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Windows;
using System.Windows.Controls;
using hMailServer.ControlPanel.Services;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Views
{
   /// <summary>
   /// One account's recovery address and its one-time recovery codes, embedded in the
   /// account editor.
   ///
   /// This page is deliberately almost all reading. Setting a recovery address and
   /// making codes are the account holder's own, proved with the account's own
   /// password in the webmail, and an administrator who could do either on somebody's
   /// behalf could reset their password by setting an address they control - which is
   /// the one thing this whole feature has to make impossible.
   ///
   /// So what is here is what an administrator actually needs: whether an address is
   /// set, whether the person has confirmed it, the masked spelling of it, how many
   /// codes are unused - and two buttons, which take the address and the codes away.
   /// Those are the answers to the two things somebody rings up to say: "that address
   /// is not mine any more", and "somebody else has seen my code card". They are
   /// separate acts because they are separate reports, and because a code stands in
   /// for the account's second factor - a set somebody else has read is a standing way
   /// past it, and the account holder's own replacement from the webmail is exactly
   /// what somebody locked out of the webmail cannot reach.
   ///
   /// The address itself is answered by nothing. Account.RecoveryAddress is the masked
   /// form, which is what every other surface shows too: enough to recognise an address
   /// you chose, not enough to write to one you did not.
   /// </summary>
   public class AccountRecoveryPanel : UserControl
   {
      private readonly string domainName_;
      private readonly string address_;

      private readonly TextBlock status_ = new() { TextWrapping = TextWrapping.Wrap, FontSize = Typography.Body };
      private readonly TextBlock codes_ = new() { TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 6, 0, 0) };
      private readonly Button clear_;
      private readonly Button clearCodes_;

      public AccountRecoveryPanel(string domainName, string address)
      {
         domainName_ = domainName;
         address_ = address;

         clear_ = MakeButton(L("_Clear the recovery address"), Wpf.Ui.Controls.ControlAppearance.Danger, (_, _) => Clear());
         clearCodes_ = MakeButton(L("Clear the recovery co_des"), Wpf.Ui.Controls.ControlAppearance.Danger, (_, _) => ClearCodes());

         Build();
      }

      public void Reload()
      {
         if (string.IsNullOrEmpty(address_))
         {
            status_.Text = L("Save the account and reopen it - a recovery address belongs to an account that exists.");
            codes_.Text = "";
            clear_.IsEnabled = false;
            clearCodes_.IsEnabled = false;
            return;
         }

         try
         {
            dynamic account = OpenAccount();
            try
            {
               string masked = (string)account.RecoveryAddress;
               bool verified = (bool)account.RecoveryAddressVerified;
               int unused = (int)account.RecoveryCodesUnused;

               status_.Text = string.IsNullOrEmpty(masked)
                  ? L("No recovery address is set. Until the account holder sets one in the webmail, a forgotten password can only be reset here.")
                  : verified
                     ? F("Confirmed: {0}. A password-reset link can be sent there.", masked)
                     : F("Set but not confirmed: {0}. Until the account holder follows the link sent to it, it is used for nothing.", masked);

               codes_.Text = F("{0} of the account's one-time recovery codes are unused.", unused);

               clear_.IsEnabled = !string.IsNullOrEmpty(masked);
               clearCodes_.IsEnabled = unused > 0;
            }
            finally
            {
               ServerSession.Release(account);
            }
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            status_.Text = ServerSession.DescribeComError(ex);
         }
      }

      private dynamic OpenAccount()
      {
         dynamic domains = ServerSession.Current.Application.Domains;
         dynamic domain = domains.ItemByName[domainName_];
         dynamic accounts = domain.Accounts;
         dynamic account = accounts.ItemByAddress[address_];

         ServerSession.Release(accounts);
         ServerSession.Release(domain);
         ServerSession.Release(domains);

         return account;
      }

      private void Clear()
      {
         if (MessageBox.Show(
               L("Remove this account's recovery address? Every password-reset link already on its way there stops working, and the account holder sets a new address in the webmail. The one-time codes are left alone."),
               L("Clear the recovery address"), MessageBoxButton.YesNo, MessageBoxImage.Warning) != MessageBoxResult.Yes)
         {
            return;
         }

         try
         {
            dynamic account = OpenAccount();
            try
            {
               account.ClearRecoveryAddress();
            }
            finally
            {
               ServerSession.Release(account);
            }

            Reload();
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            status_.Text = ServerSession.DescribeComError(ex);
         }
      }

      private void ClearCodes()
      {
         if (MessageBox.Show(
               L("End this account's one-time recovery codes? A code stands in for the account's second factor, so a set somebody else has read is a standing way past it. The account holder makes a fresh set in the webmail. The recovery address is left alone."),
               L("Clear the recovery codes"), MessageBoxButton.YesNo, MessageBoxImage.Warning) != MessageBoxResult.Yes)
         {
            return;
         }

         try
         {
            dynamic account = OpenAccount();
            try
            {
               account.ClearRecoveryCodes();
            }
            finally
            {
               ServerSession.Release(account);
            }

            Reload();
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            status_.Text = ServerSession.DescribeComError(ex);
         }
      }

      private void Build()
      {
         var root = new StackPanel { Margin = new Thickness(4, 8, 4, 4) };

         var hint = new TextBlock
         {
            Text = L("Another mailbox of the account holder's, which a password-reset link can be sent to, and ten one-time codes that stand in for a code from an authenticator app. Both are the account holder's own to set, in the webmail, with their own password: an administrator who could set the address could reset the password by pointing it at themselves. What can be done here is take an address away."),
            TextWrapping = TextWrapping.Wrap,
            Margin = new Thickness(0, 0, 0, 12)
         };
         hint.SetResourceReference(ForegroundProperty, "TextFillColorSecondaryBrush");
         root.Children.Add(hint);

         root.Children.Add(status_);
         root.Children.Add(codes_);

         var buttons = new StackPanel { Orientation = Orientation.Horizontal, Margin = new Thickness(0, 12, 0, 0) };
         buttons.Children.Add(clear_);
         buttons.Children.Add(clearCodes_);
         root.Children.Add(buttons);

         var masking = new TextBlock
         {
            Text = L("The address is shown masked here and everywhere else, and no interface answers it whole: it is a mailbox belonging to the person, and nothing an administrator does needs to read it."),
            TextWrapping = TextWrapping.Wrap,
            Margin = new Thickness(0, 12, 0, 0)
         };
         masking.SetResourceReference(ForegroundProperty, "TextFillColorSecondaryBrush");
         root.Children.Add(masking);

         Content = root;
      }

      private static Button MakeButton(string text, Wpf.Ui.Controls.ControlAppearance appearance, RoutedEventHandler onClick)
      {
         var button = new Wpf.Ui.Controls.Button
         {
            Content = text,
            Appearance = appearance,
            Margin = new Thickness(0, 0, 8, 0),
            MinWidth = 120
         };
         button.Click += onClick;
         return button;
      }
   }
}
