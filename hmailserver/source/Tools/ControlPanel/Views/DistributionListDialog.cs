// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Windows;
using System.Windows.Controls;
using hMailServer.ControlPanel.Services;
using hMailServer.ControlPanel.Views.Scaffold;
using MessageBox = hMailServer.ControlPanel.Views.Dialogs;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Views
{
   /// <summary>
   /// Property editor for one distribution list (the membership list is edited
   /// separately via <see cref="RecipientsDialog"/>). On the standard frame: the
   /// list's address is the heading, every field a <see cref="FieldRow"/> with
   /// its note under it, a save that fails says so in a notice above the fields
   /// and leaves the dialog open with what was typed.
   /// </summary>
   public class DistributionListDialog : FluentDialogWindow
   {
      private readonly string domainName_;
      private readonly string address_;

      private readonly CheckBox active_ = new() { Content = L("List is _active") };
      private readonly TextBox addressBox_ = new();
      private readonly ComboBox mode_ = new();
      private readonly CheckBox requireAuth_ = new() { Content = L("Require SMTP au_thentication to send to the list") };
      private readonly TextBox requireSender_ = new();
      private readonly TextBox moderator_ = new();
      private readonly TextBox bounce_ = new();
      private readonly TextBox membership_ = new();

      // The list manager (docs/MailingLists.md): the same group the Control
      // Deck's list editor shows, over the same COM properties.
      private readonly CheckBox managed_ = new() { Content = L("Ma_naged by the list manager") };
      private readonly ComboBox subscribe_ = new();
      private readonly ComboBox archive_ = new();
      private readonly TextBox archiveDays_ = new();
      private readonly CheckBox digest_ = new() { Content = L("Send a dail_y digest to the members who choose it") };
      private readonly TextBox digestHour_ = new();
      private readonly CheckBox replyToList_ = new() { Content = L("Replies go to th_e list") };
      private readonly TextBox prefix_ = new() { MaxLength = 64 };
      private readonly TextBox footer_ = new() { MaxLength = 2000, AcceptsReturn = true, TextWrapping = TextWrapping.Wrap, MinHeight = 60 };
      private readonly CheckBox verp_ = new() { Content = L("Encode each member in the bounce address (_VERP)") };
      private readonly TextBox owner_ = new();
      private readonly InlineNotice notice_ = DialogFields.Notice();

      public DistributionListDialog(Window owner, string domainName, string address)
      {
         domainName_ = domainName;
         address_ = address;
         Owner = owner;
         Title = F("Distribution list - {0}", address);

         var body = new StackPanel();
         body.Children.Add(notice_);

         body.Children.Add(DialogFields.Field(null, active_));
         body.Children.Add(Label(L("_List address"), addressBox_));

         // Four modes, not five. There used to be a fifth - "Anyone with a server
         // account can send", mode 4 - and it was the most dangerous entry in this
         // dialog, because it did the opposite of what it said.
         //
         // The server does not implement it: DistributionList::ListMode stops at
         // LMDomainMembers = 3 and RecipientParser::UserCanSendToList_ has no
         // branch for a fifth mode. Mode 4 existed only as an enumerator in the
         // type library. put_Mode's switch had no case for it and seeded its local
         // with LMPublic, so choosing it stored "anyone may send" - and get_Mode's
         // default reported it back as "Public", so the only symptom was a
         // selection that looked as though it had not stuck.
         //
         // An administrator picking the more restrictive-sounding of the two
         // "anyone..." entries, to keep outsiders off a list, was silently given
         // the single most permissive setting the server has. put_Mode now refuses
         // the value outright; the option is gone from here so nobody can reach it.
         mode_.Items.Add(DialogFields.Combo(L("Public — anyone can send"), 0));
         mode_.Items.Add(DialogFields.Combo(L("Membership — only list members can send"), 1));
         mode_.Items.Add(DialogFields.Combo(L("Announcements only"), 2));
         mode_.Items.Add(DialogFields.Combo(L("Anyone in the domain can send"), 3));
         body.Children.Add(Label(L("_Who may send to this list"), mode_)
            .WithHint(L("\"Anyone in the domain\" means the sender's address is at a domain this server hosts, which an outsider can claim unless the list also requires authentication. Tick that below if the list must be restricted to people who have logged in.")));

         body.Children.Add(DialogFields.Field(null, requireAuth_));
         body.Children.Add(Label(L("_Require sender address (empty = any)"), requireSender_));

         body.Children.Add(Label(L("_Moderator (empty = no moderation)"), moderator_)
            .WithHint(L("With a moderator set, a sender the rules above refuse is forwarded to the moderator instead of being rejected. The moderator approves by resending the message to the list from an authenticated session.")));

         body.Children.Add(Label(L("_Bounce address (empty = bounces go to the poster)"), bounce_)
            .WithHint(L("Used as the envelope sender of every copy the list sends, so delivery failures - a dead subscriber, a full mailbox - reach the list owner instead of whoever happened to post last.")));

         // The query, beside the members rather than instead of them: a list
         // whose query is set still delivers to the addresses under Recipients,
         // and an address that is both is delivered to once.
         body.Children.Add(Label(L("Membership _query (empty = the recipients alone)"), membership_)
            .WithHint(L("Finds the rest of the members among the server's accounts each time a message is sent to the list, so nobody has to keep the list up to date by hand. Leave it empty for a list whose members are its recipients alone; domain for every active account of this domain; domain:other.example for another domain this server hosts; pattern:sales-*@example.com for the addresses matching a wildcard, with * for any run of characters and ? for one; everyone for every active account on the server. Accounts that are switched off are never included.")));

         // The list manager. Off, the rest of this group does nothing and the
         // list is the distribution list it always was.
         body.Children.Add(DialogFields.Separator());
         body.Children.Add(DialogFields.Field(null, managed_)
            .WithHint(L("With the manager on, the list answers its own addresses - its -subscribe, -unsubscribe, -request, -owner and -bounces addresses - and sends each member a copy of their own, with their own way to leave. Each member's delivery and bounces are shown in the Control Deck.")));

         subscribe_.Items.Add(DialogFields.Combo(L("Nobody - the members are added here"), 0));
         subscribe_.Items.Add(DialogFields.Combo(L("Anyone who confirms by mail"), 1));
         subscribe_.Items.Add(DialogFields.Combo(L("Anyone the owner approves after they confirm"), 2));
         body.Children.Add(Label(L("Who may _join"), subscribe_));

         archive_.Items.Add(DialogFields.Combo(L("No archive"), 0));
         archive_.Items.Add(DialogFields.Combo(L("For the members"), 1));
         archive_.Items.Add(DialogFields.Combo(L("Public"), 2));
         body.Children.Add(Label(L("Ar_chive"), archive_));
         body.Children.Add(Label(L("Keep archived messages for (_days, 0 = for ever)"), archiveDays_));

         body.Children.Add(DialogFields.Field(null, digest_));
         body.Children.Add(Label(L("Digest _hour (0-23)"), digestHour_));
         body.Children.Add(DialogFields.Field(null, replyToList_));
         body.Children.Add(Label(L("Subject _prefix"), prefix_));
         body.Children.Add(Label(L("_Footer"), footer_));
         body.Children.Add(DialogFields.Field(null, verp_));
         body.Children.Add(Label(L("_Owner (empty = the moderator)"), owner_));

         // Enter saves, Escape cancels.
         var save = new Wpf.Ui.Controls.Button { Content = L("_Save"), Appearance = Wpf.Ui.Controls.ControlAppearance.Primary, MinWidth = 88 };
         save.Click += (s, e) => Save();
         var cancel = new Wpf.Ui.Controls.Button { Content = L("Cancel"), MinWidth = 88 };
         cancel.Click += (s, e) => Close();

         UseFrame(address, body, save, cancel, width: 520);
         Loaded += (s, e) => Load();
      }

      private dynamic OpenList(dynamic domains)
      {
         dynamic domain = domains.ItemByName[domainName_];
         dynamic lists = domain.DistributionLists;
         dynamic list = lists.ItemByAddress[address_];
         ServerSession.Release(lists);
         ServerSession.Release(domain);
         return list;
      }

      private void Load()
      {
         dynamic domains = ServerSession.Current.Application.Domains;
         try
         {
            dynamic l = OpenList(domains);
            active_.IsChecked = (bool)l.Active;
            addressBox_.Text = (string)l.Address ?? "";
            DialogFields.SelectCombo(mode_, (int)l.Mode);
            requireAuth_.IsChecked = (bool)l.RequireSMTPAuth;
            requireSender_.Text = (string)l.RequireSenderAddress ?? "";
            moderator_.Text = (string)l.ModeratorAddress ?? "";
            bounce_.Text = (string)l.BounceAddress ?? "";
            membership_.Text = (string)l.Membership ?? "";
            managed_.IsChecked = (bool)l.Managed;
            DialogFields.SelectCombo(subscribe_, (int)l.SubscribePolicy, true);
            DialogFields.SelectCombo(archive_, (int)l.Archive, true);
            archiveDays_.Text = ((int)l.ArchiveDays).ToString(System.Globalization.CultureInfo.InvariantCulture);
            digest_.IsChecked = (bool)l.Digest;
            digestHour_.Text = ((int)l.DigestHour).ToString(System.Globalization.CultureInfo.InvariantCulture);
            replyToList_.IsChecked = (bool)l.ReplyToList;
            prefix_.Text = (string)l.SubjectPrefix ?? "";
            footer_.Text = (string)l.Footer ?? "";
            verp_.IsChecked = (bool)l.Verp;
            owner_.Text = (string)l.Owner ?? "";
            ServerSession.Release(l);
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            MessageBox.Show(F("Could not load the list: {0}", ServerSession.DescribeComError(ex)), L("Control Panel"));
            Close();
         }
         finally
         {
            ServerSession.Release(domains);
         }
      }

      private void Save()
      {
         notice_.Hide();

         dynamic domains = ServerSession.Current.Application.Domains;
         try
         {
            dynamic l = OpenList(domains);
            l.Active = active_.IsChecked is true;
            if (addressBox_.Text.Trim().Length > 0)
               l.Address = addressBox_.Text.Trim();
            l.Mode = DialogFields.ComboValue(mode_);
            l.RequireSMTPAuth = requireAuth_.IsChecked is true;
            l.RequireSenderAddress = requireSender_.Text.Trim();
            l.ModeratorAddress = moderator_.Text.Trim();
            l.BounceAddress = bounce_.Text.Trim();
            l.Membership = membership_.Text.Trim();
            l.Managed = managed_.IsChecked is true;
            l.SubscribePolicy = DialogFields.ComboValue(subscribe_);
            l.Archive = DialogFields.ComboValue(archive_);
            l.ArchiveDays = WholeNumber(archiveDays_.Text, 0);
            l.Digest = digest_.IsChecked is true;
            l.DigestHour = WholeNumber(digestHour_.Text, 7);
            l.ReplyToList = replyToList_.IsChecked is true;
            l.SubjectPrefix = prefix_.Text.Trim();
            l.Footer = footer_.Text;
            l.Verp = verp_.IsChecked is true;
            l.Owner = owner_.Text.Trim();
            l.Save();
            ServerSession.Release(l);
            Close();
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            notice_.Show(StatusLevel.Critical, F("Could not save the list: {0}", ServerSession.DescribeComError(ex)));
         }
         finally
         {
            ServerSession.Release(domains);
         }
      }

      /// <summary>
      /// A caption and its editor as one row, which names the editor to UI
      /// Automation. The two checkboxes need no caption: a content control names
      /// itself.
      /// </summary>
      private static FieldRow Label(string text, FrameworkElement editor) => DialogFields.Field(text, editor);

      /// <summary>
      /// A whole number typed into a box, or the fallback for an empty box. A
      /// number out of range is passed on as it is, so the server refuses it with
      /// its own sentence rather than this dialog guessing a nearer value.
      /// </summary>
      private static int WholeNumber(string text, int fallback)
      {
         var trimmed = (text ?? "").Trim();
         if (trimmed.Length == 0)
            return fallback;

         return int.TryParse(trimmed, System.Globalization.NumberStyles.Integer, System.Globalization.CultureInfo.InvariantCulture, out var value)
            ? value
            : -1;
      }
   }
}
