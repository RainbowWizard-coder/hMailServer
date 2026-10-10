// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using System.Windows.Input;
using hMailServer.ControlPanel.Services;
using hMailServer.ControlPanel.Views.Scaffold;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Views
{
   /// <summary>
   /// Modal editor for the recipients of a distribution list, on the standard
   /// frame: the list, the two buttons that act on it under it, the add row
   /// under them, what happened in a notice above, and Close alone in the
   /// footer - the buttons that act on the list stay by the list, and the one
   /// that ends the dialog stays apart from them, because putting a "leave"
   /// next to a "delete" is how the wrong one gets pressed.
   /// </summary>
   public class RecipientsDialog : FluentDialogWindow
   {
      private readonly string domainName_;
      private readonly string listAddress_;
      // On a card with the compact inset, as the public folders list is: the
      // rows take the application's list row style (the selection fill and the
      // brand pill, in place of WPF-UI's solid accent band), and the card is the
      // list's frame.
      private readonly ListBox listBox_ = new() { Height = 240, BorderThickness = new Thickness(0), Background = System.Windows.Media.Brushes.Transparent };
      private readonly TextBox addBox_ = new();
      private readonly InlineNotice notice_ = DialogFields.Notice();

      // What an empty list means, in the list's place rather than a blank box
      // (review finding F207). The add row under the list is its action.
      private readonly EmptyState empty_ = new() { Icon = Wpf.Ui.Controls.SymbolRegular.Person24, Padding = new Thickness(12), Visibility = Visibility.Collapsed };

      // How the selected member of a managed list is sent its mail
      // (DistributionListRecipient.Delivery, docs/MailingLists.md): written at once,
      // as the adds and removes on this dialog are. Disabled until a member is
      // selected, and on a server that has no such property.
      private readonly ComboBox delivery_ = new() { IsEnabled = false, HorizontalAlignment = HorizontalAlignment.Left, MinWidth = 240 };
      private bool showingDelivery_;

      public RecipientsDialog(Window owner, string domainName, string listAddress)
      {
         domainName_ = domainName;
         listAddress_ = listAddress;

         Owner = owner;
         // Member throughout, as the page's Members button that opens this dialog
         // says, where the heading, the box, the question and the outcomes said
         // recipient (review finding F082).
         Title = F("Members - {0}", listAddress);

         var body = new StackPanel();
         body.Children.Add(notice_);

         AutomationProperties.SetName(listBox_, F("Members of {0}", listAddress));
         var listHost = new Grid();
         listHost.Children.Add(listBox_);
         listHost.Children.Add(empty_);
         body.Children.Add(new Card { Padding = new Thickness(DesignTokens.Space.Sm), Content = listHost });

         // The two buttons that act on the list, under it, where they act: in
         // the add row beside the box and Add, the three buttons left the box
         // about 30 pixels in German ("Hinzufügen", "Aus AD hinzufügen…",
         // "Ausgewählte entfernen"), 100 in Japanese and 155 in English, which
         // is too narrow to read an address in. A WrapPanel, so that in a
         // language where the two do not fit on one line they take two.
         var listActions = new WrapPanel { HorizontalAlignment = HorizontalAlignment.Right, Margin = new Thickness(0, DesignTokens.Space.Sm, 0, 0) };

         var adButton = new Wpf.Ui.Controls.Button { Content = L("Add from A_D…"), Margin = new Thickness(DesignTokens.Space.Sm, 0, 0, 0) };
         adButton.Click += (s, e) => ImportFromActiveDirectory();
         listActions.Children.Add(adButton);

         // The list pages' one Delete style (ButtonStyles.DangerText): red words
         // while there is a member to remove, the disabled grey while none is
         // chosen - not the filled red of a confirmation, live over an empty list.
         var removeButton = new Wpf.Ui.Controls.Button { Content = L("Re_move"), Margin = new Thickness(DesignTokens.Space.Sm, 0, 0, 0) };
         ButtonStyles.DangerText(removeButton);
         SelectionGate.Bind(listBox_, removeButton);
         removeButton.Click += (s, e) => RemoveSelected();
         listActions.Children.Add(removeButton);

         body.Children.Add(listActions);

         delivery_.Items.Add(DialogFields.Combo(L("Each message"), 0));
         delivery_.Items.Add(DialogFields.Combo(L("The daily digest"), 1));
         delivery_.Items.Add(DialogFields.Combo(L("Nothing, while staying a member"), 2));
         var deliveryRow = DialogFields.Field(L("_How the selected member is sent a managed list's mail"), delivery_);
         deliveryRow.Margin = new Thickness(0, DesignTokens.Space.Md, 0, 0);
         body.Children.Add(deliveryRow);
         body.Children.Add(DialogFields.Note(L("Only a managed list reads it, and the member can change it on their own page. The digest goes out once a day on a list whose digest is on; on one whose digest is off, a digest member is sent each message.")));

         listBox_.SelectionChanged += (s, e) => ShowDelivery_();
         delivery_.SelectionChanged += (s, e) =>
         {
            if (!showingDelivery_)
               SaveDelivery_();
         };

         // The add row: the box, with the dialog's width but for Add beside it.
         var bottom = new Grid { Margin = new Thickness(0, DesignTokens.Space.Md, 0, 0) };
         bottom.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
         bottom.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });

         // The box is captioned, as every other box on these dialogs is: it was a
         // bare box beside three buttons, which said what it was for to a screen
         // reader and to nobody else. The button sits on its baseline.
         AutomationProperties.SetName(addBox_, L("Address to add as a member"));

         // Enter adds. Handled, so the keystroke cannot also reach a default button
         // if one is ever added here.
         addBox_.KeyDown += (s, e) =>
         {
            if (e.Key != Key.Enter)
               return;

            AddRecipient();
            e.Handled = true;
         };

         // The caption above the whole row, so that it reads on one line rather
         // than folded into the width of the box.
         bottom.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
         bottom.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
         TextBlock caption = DialogFields.Caption(L("Address to add as a member"));
         Grid.SetColumnSpan(caption, 2);
         bottom.Children.Add(caption);
         Grid.SetRow(addBox_, 1);
         bottom.Children.Add(addBox_);

         var addButton = new Wpf.Ui.Controls.Button { Content = L("_Add"), Margin = new Thickness(DesignTokens.Space.Sm, 0, 0, 0), VerticalAlignment = VerticalAlignment.Bottom };
         addButton.Click += (s, e) => AddRecipient();
         Grid.SetColumn(addButton, 1);
         Grid.SetRow(addButton, 1);
         bottom.Children.Add(addButton);

         body.Children.Add(bottom);

         // Escape closes; there is no default button, because Enter in the add
         // box means "add" and Enter with a row selected must not close the list.
         var closeButton = new Wpf.Ui.Controls.Button { Content = L("Close"), MinWidth = 88 };
         closeButton.Click += (s, e) => Close();

         UseFrame(F("Members of {0}", listAddress), body, null, closeButton, width: 520);
         Loaded += (s, e) => Reload();
      }

      private dynamic OpenList(dynamic domains)
      {
         dynamic domain = domains.ItemByName[domainName_];
         dynamic lists = domain.DistributionLists;
         int count = (int)lists.Count;
         for (int i = 0; i < count; i++)
         {
            dynamic list = lists.Item[i];
            if ((string)list.Address == listAddress_)
            {
               ServerSession.Release(lists);
               ServerSession.Release(domain);
               return list;
            }
            ServerSession.Release(list);
         }
         ServerSession.Release(lists);
         ServerSession.Release(domain);
         return null;
      }

      private void Reload()
      {
         var rows = new List<string>();
         bool read = false;
         dynamic domains = ServerSession.Current.Application.Domains;
         try
         {
            dynamic list = OpenList(domains);
            if (list != null)
            {
               dynamic recipients = list.Recipients;
               int count = (int)recipients.Count;
               for (int i = 0; i < count; i++)
               {
                  dynamic recipient = recipients.Item[i];
                  rows.Add((string)recipient.RecipientAddress);
                  ServerSession.Release(recipient);
               }
               ServerSession.Release(recipients);
               ServerSession.Release(list);
            }

            read = true;
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            // An empty list is a claim this dialog cannot make about a list it
            // could not read: the empty state stays hidden and the notice says
            // what happened.
            notice_.Show(StatusLevel.Critical, L("Could not read the list: ") + ServerSession.DescribeComError(ex));
         }
         finally
         {
            ServerSession.Release(domains);
         }

         listBox_.ItemsSource = rows;
         if (read)
            StatusText.Show(empty_, null, rows.Count, null, L("No members yet, so mail sent to this list reaches nobody. Add an address below."));
         else
            empty_.Visibility = Visibility.Collapsed;
      }

      private void AddRecipient()
      {
         string address = addBox_.Text.Trim();
         if (address.Length == 0)
            return;

         notice_.Hide();
         dynamic domains = ServerSession.Current.Application.Domains;
         try
         {
            dynamic list = OpenList(domains);
            if (list != null)
            {
               dynamic recipients = list.Recipients;
               dynamic recipient = recipients.Add();
               recipient.RecipientAddress = address;
               recipient.Save();
               ServerSession.Release(recipient);
               ServerSession.Release(recipients);
               ServerSession.Release(list);
            }
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            notice_.Show(StatusLevel.Critical, F("Could not add the member: {0}", ServerSession.DescribeComError(ex)));
         }
         finally
         {
            ServerSession.Release(domains);
         }

         addBox_.Text = "";
         Reload();
      }

      private void ImportFromActiveDirectory()
      {
         var picker = new ActiveDirectoryPickerDialog(this, multiSelect: true);
         if (picker.ShowDialog() != true || picker.SelectedUsers.Count == 0)
            return;

         notice_.Hide();
         var emails = new List<string>();
         int skipped = 0;
         foreach (AdUser u in picker.SelectedUsers)
         {
            if (!string.IsNullOrWhiteSpace(u.Email))
               emails.Add(u.Email.Trim());
            else
               skipped++;
         }

         if (emails.Count == 0)
         {
            notice_.Show(StatusLevel.Warning, L("None of the selected accounts have an e-mail address."));
            return;
         }

         int added = 0;
         dynamic domains = ServerSession.Current.Application.Domains;
         try
         {
            dynamic list = OpenList(domains);
            if (list != null)
            {
               dynamic recipients = list.Recipients;
               foreach (string address in emails)
               {
                  dynamic recipient = recipients.Add();
                  recipient.RecipientAddress = address;
                  recipient.Save();
                  ServerSession.Release(recipient);
                  added++;
               }
               ServerSession.Release(recipients);
               ServerSession.Release(list);
            }
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            notice_.Show(StatusLevel.Critical, F("Could not add all the members: {0}", ServerSession.DescribeComError(ex)));
         }
         finally
         {
            ServerSession.Release(domains);
         }

         Reload();
         if (skipped > 0)
            notice_.Show(StatusLevel.Information, PluralF(skipped,
               "{0} {1} account had no e-mail address and was skipped.",
               "{0} {1} accounts had no e-mail address and were skipped.",
               Plural(added, "{0} member added.", "{0} members added."), skipped));
      }

      /// <summary>The selected member's delivery, read from the server.</summary>
      private void ShowDelivery_()
      {
         string address = listBox_.SelectedItem as string;
         showingDelivery_ = true;
         try
         {
            delivery_.IsEnabled = false;
            delivery_.SelectedIndex = -1;
            if (address == null)
               return;

            int? delivery = WithRecipient_(address, recipient => (int)recipient.Delivery);
            if (delivery.HasValue)
            {
               DialogFields.SelectCombo(delivery_, delivery.Value);
               delivery_.IsEnabled = true;
            }
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            // A server without DistributionListRecipient.Delivery: the choice stays
            // disabled rather than offering one that cannot be read back.
         }
         finally
         {
            showingDelivery_ = false;
         }
      }

      /// <summary>Writes the selected member's delivery, at once.</summary>
      private void SaveDelivery_()
      {
         string address = listBox_.SelectedItem as string;
         int value = DialogFields.ComboValue(delivery_, -1);
         if (address == null || value < 0)
            return;

         notice_.Hide();
         try
         {
            WithRecipient_(address, recipient =>
            {
               recipient.Delivery = value;
               recipient.Save();
               return value;
            });
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            notice_.Show(StatusLevel.Critical, F("Could not change how the member is sent mail: {0}", ServerSession.DescribeComError(ex)));
            ShowDelivery_();
         }
      }

      /// <summary>Runs <paramref name="use"/> on the recipient row with this address; null when there is none.</summary>
      private int? WithRecipient_(string address, Func<dynamic, int> use)
      {
         dynamic domains = ServerSession.Current.Application.Domains;
         try
         {
            dynamic list = OpenList(domains);
            if (list == null)
               return null;

            dynamic recipients = list.Recipients;
            try
            {
               int count = (int)recipients.Count;
               for (int i = 0; i < count; i++)
               {
                  dynamic recipient = recipients.Item[i];
                  try
                  {
                     if ((string)recipient.RecipientAddress == address)
                        return use(recipient);
                  }
                  finally
                  {
                     ServerSession.Release(recipient);
                  }
               }
               return null;
            }
            finally
            {
               ServerSession.Release(recipients);
               ServerSession.Release(list);
            }
         }
         finally
         {
            ServerSession.Release(domains);
         }
      }

      private void RemoveSelected()
      {
         string address = listBox_.SelectedItem as string;
         if (address == null)
            return;

         if (!Dialogs.Confirm(F("Remove the member {0} from this list?", address), L("Control Panel"), L("Re_move"), destructive: true))
            return;

         notice_.Hide();
         dynamic domains = ServerSession.Current.Application.Domains;
         try
         {
            dynamic list = OpenList(domains);
            if (list != null)
            {
               dynamic recipients = list.Recipients;
               int count = (int)recipients.Count;
               for (int i = 0; i < count; i++)
               {
                  dynamic recipient = recipients.Item[i];
                  if ((string)recipient.RecipientAddress == address)
                  {
                     recipient.Delete();
                     ServerSession.Release(recipient);
                     break;
                  }
                  ServerSession.Release(recipient);
               }
               ServerSession.Release(recipients);
               ServerSession.Release(list);
            }
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            notice_.Show(StatusLevel.Critical, F("Could not remove the member: {0}", ServerSession.DescribeComError(ex)));
         }
         finally
         {
            ServerSession.Release(domains);
         }

         Reload();
      }
   }
}
