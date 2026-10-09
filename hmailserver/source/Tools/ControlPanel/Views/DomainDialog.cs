// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Linq;
using System.Windows.Automation;
using System.Windows;
using System.Windows.Controls;
using hMailServer.ControlPanel.Services;
using hMailServer.ControlPanel.Views.Scaffold;
using MessageBox = hMailServer.ControlPanel.Views.Dialogs;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Views
{
   /// <summary>
   /// Modal, tabbed editor for one domain: general, limits, signature and DKIM.
   /// Aliases and distribution lists are managed from the Domains page itself.
   ///
   /// On the standard frame: the domain name is the heading, the seven tabs all
   /// take the height the dialog allows so the window never resizes on a click of
   /// the strip, every field is a <see cref="FieldRow"/>, each of the eight
   /// numbers that can be wrong is said on its own field rather than in one line
   /// at the foot of the dialog, and the key rotation's outcome and each record
   /// check is an <see cref="InlineNotice"/> that carries its level in colour, shape
   /// and word instead of in a held brush. The message boxes that remain are the real
   /// questions - renaming a domain, switching, retiring or cancelling a rotation - and
   /// the file dialogs they lead to.
   /// </summary>
   public class DomainDialog : FluentDialogWindow
   {
      /// <summary>The width this dialog asks the frame for.</summary>
      private const double DialogWidth = 640;

      private readonly string domainName_;

      private readonly InlineNotice notice_ = DialogFields.Notice();

      // General
      private readonly CheckBox active_ = new() { Content = L("Domain _enabled") };
      private readonly TextBox name_ = NewInput();
      private FieldRow nameRow_;
      private readonly TextBox postmaster_ = NewInput();
      private readonly TextBox adDomain_ = NewInput();

      // Limits
      private readonly TextBox maxSize_ = NewInput();
      private FieldRow maxSizeRow_;
      private readonly TextBox maxMessageSize_ = NewInput();
      private FieldRow maxMessageSizeRow_;
      private readonly TextBox maxAccountSize_ = NewInput();
      private FieldRow maxAccountSizeRow_;
      private readonly TextBox retentionDays_ = NewInput();

      // The quota policy: -1 and an empty text mean the server's own apply.
      private readonly TextBox quotaWarnAt_ = NewInput();
      private readonly TextBox quotaSubject_ = NewInput();
      private readonly TextBox quotaText_ = NewMemo();
      private readonly TextBox quotaGrace_ = NewInput();
      private readonly TextBox quotaStopAt_ = NewInput();
      private FieldRow quotaWarnAtRow_, quotaGraceRow_, quotaStopAtRow_;
      private FieldRow retentionRow_;
      private readonly CheckBox maxAccountsOn_ = new() { Content = L("_Limit number of accounts") };
      private readonly TextBox maxAccounts_ = NewInput();
      private FieldRow maxAccountsRow_;
      private readonly CheckBox maxAliasesOn_ = new() { Content = L("Limit number of al_iases") };
      private readonly TextBox maxAliases_ = NewInput();
      private FieldRow maxAliasesRow_;
      private readonly CheckBox maxDistsOn_ = new() { Content = L("Limit _number of distribution lists") };
      private readonly TextBox maxDists_ = NewInput();
      private FieldRow maxDistsRow_;
      private readonly CheckBox plusAddressingOn_ = new() { Content = L("_Enable plus addressing") };
      private readonly TextBox plusChar_ = NewInput();
      private readonly CheckBox greylisting_ = new() { Content = L("Enable _greylisting for this domain") };

      // Signature
      private readonly CheckBox signatureOn_ = new() { Content = L("_Add signature to outgoing messages") };
      private readonly ComboBox signatureMethod_ = new();
      private readonly CheckBox signReplies_ = new() { Content = L("Add signature to _replies") };
      private readonly CheckBox signLocal_ = new() { Content = L("Add signature to _local e-mail") };
      private readonly TextBox signaturePlain_ = NewMemo();
      private readonly TextBox signatureHtml_ = NewMemo();

      // Outbound relay for this domain
      // Domain-wide out-of-office. The server sends this only for accounts that
      // have no vacation message of their own; the override below additionally
      // replaces an account's personal text for senders outside this server.
      private readonly CheckBox oooOn_ = new() { Content = L("Send a _domain-wide out-of-office reply") };
      private readonly TextBox oooSubject_ = NewInput();
      private readonly TextBox oooMessage_ = NewMemo();
      private readonly TextBox oooInternalSubject_ = NewInput();
      private readonly TextBox oooInternalMessage_ = NewMemo();
      private readonly CheckBox oooExternalOverride_ = new()
      {
         Content = L("_Outside senders always get the domain's text, even when the account has its own")
      };
      private readonly TextBox oooDays_ = NewInput();

      // What this domain does to a message: the external-sender tag, the
      // first-contact note and the disclaimer. See Common/Util/MessageOrigin.h
      // for the rule that decides who is outside.
      private readonly CheckBox tagSubject_ = new() { Content = L("_Tag the subject of mail from outside") };
      private readonly TextBox tagText_ = NewInput();
      private FieldRow tagTextRow_;
      private readonly CheckBox tagHeader_ = new() { Content = L("Add a _header the reader shows as a banner") };
      private readonly CheckBox firstContact_ = new() { Content = L("Tell the reader about a _first contact from outside") };
      private readonly CheckBox disclaimerOn_ = new()
      {
         Content = L("_Append a disclaimer to mail leaving the organisation")
      };
      private readonly TextBox disclaimerPlain_ = NewMemo();
      private readonly TextBox disclaimerHtml_ = NewMemo();

      private readonly TextBox relayHost_ = NewInput();
      private readonly TextBox relayPort_ = NewInput();
      private FieldRow relayPortRow_;
      private readonly CheckBox relayAuthOn_ = new() { Content = L("The relay requires _authentication") };
      private readonly TextBox relayUser_ = NewInput();
      private readonly hMailServer.ControlPanel.Views.PasswordField relayPassword_ = new();
      private readonly ComboBox relaySecurity_ = new();

      // DKIM
      private readonly CheckBox dkimOn_ = new() { Content = L("_Enable DKIM signing") };
      private readonly CheckBox dkimAliases_ = new() { Content = L("Sign aliases _too") };
      private readonly TextBox dkimSelector_ = NewInput();
      private readonly TextBox dkimKeyFile_ = NewInput();
      private readonly ComboBox dkimHeaderCanon_ = new();
      private readonly ComboBox dkimBodyCanon_ = new();
      private readonly ComboBox dkimAlgorithm_ = new();
      private readonly TextBox dkimDns_ = ReadOnlyBox(70);
      private StackPanel dkimDnsPanel_;
      private string dkimDnsValue_ = "";

      // DKIM key rotation. The policy is saved with the domain, like every other
      // field of this dialog; the rotation itself is the server's - it generates the
      // keys, checks the records from its own resolver and moves signing - so this
      // dialog shows what the server reports (DkimRotationStatus) and asks it to act,
      // redrawing from the answer. Nothing here reads a key file or looks up DNS on
      // this machine, which is what lets it administer a server that is not this one.
      private readonly CheckBox rotOn_ = new() { Content = L("Rotate the _key on a schedule") };
      private readonly TextBox rotInterval_ = NewInput();
      private FieldRow rotIntervalRow_;
      private readonly TextBox rotPropagation_ = NewInput();
      private FieldRow rotPropagationRow_;
      private readonly TextBox rotRetention_ = NewInput();
      private FieldRow rotRetentionRow_;
      private readonly ComboBox rotKeyBits_ = new();
      private readonly InlineNotice rotStatus_ = DialogFields.Notice();
      private readonly StackPanel rotState_ = new();
      private Wpf.Ui.Controls.Button rotStart_;
      private Wpf.Ui.Controls.Button rotCheck_;
      private Wpf.Ui.Controls.Button rotSwitch_;
      private Wpf.Ui.Controls.Button rotRetire_;
      private Wpf.Ui.Controls.Button rotCancel_;
      private DkimRotationStatus rotation_;

      // False against a server older than the rotation: the policy is then neither
      // shown as editable nor written, and the rest of the dialog works as before.
      private bool rotationSupported_;

      // The selector and key file as the dialog opened, so that Save writes them only
      // when the administrator changed them. A rotation moves them while the dialog is
      // open, and writing the values it opened with would put the old key back.
      private string loadedSelector_ = "";
      private string loadedKeyFile_ = "";

      // Names (domain aliases) — embedded list editor
      private CollectionEditorView aliasEditor_;

      // The domain's allow and block list - embedded list editor
      private CollectionEditorView senderListEditor_;

      // Quarantine digest
      private readonly CheckBox digestOn_ = new() { Content = L("Send each account a daily _digest of what the quarantine holds for it") };
      private readonly TextBox digestHour_ = NewInput();
      private FieldRow digestHourRow_;

      // The reports the domain receives, read: the address its DMARC aggregate
      // and TLS-RPT reports are posted to and read from (Domain.ReportMailbox).
      private readonly TextBox reportMailbox_ = NewInput();

      // Restricted delivery: the domains this domain's accounts may exchange
      // mail with (Domain.RestrictedDomains, docs/RestrictedDelivery.md).
      private readonly TextBox restrictedDomains_ = NewMemo();

      // Impersonation protection: what to do with a borrowed name or a
      // look-alike domain on mail from outside, and the two lists it judges
      // against (Domain.ImpersonationAction, ProtectedPersons,
      // ProtectedPartners; docs/ImpersonationProtection.md).
      private readonly ComboBox impersonationAction_ = new();
      private readonly TextBox protectedPersons_ = NewMemo();
      private readonly TextBox protectedPartners_ = NewMemo();

      // Country blocking at sign-in (Domain.CountryPolicy, docs/CountryBlocking.md):
      // the domain's own policy, beside the server's for connecting.
      private readonly TextBox countryPolicy_ = NewInput();

      // Publishing OpenPGP keys in the Web Key Directory (Domain.OpenPGPPublishKeys,
      // Domain.OpenPGPPublishNewAccounts; docs/OpenPGP.md).
      private readonly CheckBox publishKeys_ = new() { Content = L("_Publish the OpenPGP public keys of this domain's accounts in the Web Key Directory") };
      private readonly CheckBox publishNewAccounts_ = new() { Content = L("_New accounts start with their keys published") };

      // Outbound webhooks (Domain.WebhooksEnabled, Domain.WebhookSubjects;
      // docs/Webhooks.md).
      private readonly CheckBox webhooksOn_ = new() { Content = L("_Raise webhook events about this domain's mail and accounts") };
      private readonly CheckBox webhookSubjects_ = new() { Content = L("Let those events carry a message's s_ubject") };

      public DomainDialog(Window owner, string domainName)
      {
         domainName_ = domainName;

         Owner = owner;
         Title = F("Domain - {0}", domainName);

         var tabs = new TabControl { Background = System.Windows.Media.Brushes.Transparent, BorderThickness = new Thickness(0) };
         tabs.Items.Add(new TabItem { Header = L("General"), Content = BuildGeneral() });
         tabs.Items.Add(new TabItem { Header = L("Names"), Content = BuildNames() });
         tabs.Items.Add(new TabItem { Header = L("Limits"), Content = BuildLimits() });
         tabs.Items.Add(new TabItem { Header = L("Signature"), Content = BuildSignature() });
         tabs.Items.Add(new TabItem { Header = L("Relay"), Content = BuildRelay() });
         tabs.Items.Add(new TabItem { Header = L("Out of office"), Content = BuildOutOfOffice() });
         tabs.Items.Add(new TabItem { Header = L("Tagging and disclaimer"), Content = BuildTransforms() });
         tabs.Items.Add(new TabItem { Header = L("Impersonation protection"), Content = BuildImpersonation() });
         tabs.Items.Add(new TabItem { Header = L("DKIM"), Content = BuildDkim() });
         tabs.Items.Add(new TabItem { Header = L("Allow and block list"), Content = BuildSenderList() });
         tabs.Items.Add(new TabItem { Header = L("Quarantine digest"), Content = BuildQuarantineDigest() });
         tabs.Items.Add(new TabItem { Header = L("Reports received"), Content = BuildReportsReceived() });
         tabs.Items.Add(new TabItem { Header = L("Restricted delivery"), Content = BuildRestrictedDelivery() });
         tabs.Items.Add(new TabItem { Header = L("Country blocking"), Content = BuildCountryPolicy() });
         tabs.Items.Add(new TabItem { Header = L("OpenPGP"), Content = BuildOpenPgp() });
         tabs.Items.Add(new TabItem { Header = L("Webhooks"), Content = BuildWebhooks() });
         tabs.Items.Add(new TabItem { Header = L("Levels"), Content = BuildLevels() });
         DialogFields.FillTabs(tabs);

         var body = new StackPanel();
         body.Children.Add(notice_);
         body.Children.Add(tabs);

         var save = new Wpf.Ui.Controls.Button { Content = L("_Save"), Appearance = Wpf.Ui.Controls.ControlAppearance.Primary, MinWidth = 88 };
         save.Click += (s, e) => Save();
         var cancel = new Wpf.Ui.Controls.Button { Content = L("Cancel"), MinWidth = 88 };
         cancel.Click += (s, e) => Close();

         UseFrame(domainName, body, save, cancel, width: DialogWidth);
         Loaded += (s, e) => { Load(); aliasEditor_?.OnEnter(); senderListEditor_?.OnEnter(); };
      }

      private ScrollViewer BuildGeneral()
      {
         var panel = DialogFields.TabPanel();
         panel.Children.Add(DialogFields.Check(active_));
         nameRow_ = Label(L("_Domain name (changing it renames the domain and moves every account, alias and list with it)"), name_);
         panel.Children.Add(nameRow_);
         panel.Children.Add(Label(L("_Postmaster address (mail to unknown recipients is redirected here)"), postmaster_));

         var adRow = new Grid();
         adRow.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
         adRow.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
         Grid.SetColumn(adDomain_, 0);
         adRow.Children.Add(adDomain_);
         var adBrowse = new Wpf.Ui.Controls.Button { Content = L("_Browse…"), Margin = new Thickness(DesignTokens.Space.Sm, 0, 0, 0) };
         adBrowse.Click += BrowseAdDomain;
         Grid.SetColumn(adBrowse, 1);
         adRow.Children.Add(adBrowse);
         panel.Children.Add(DialogFields.Field(L("Active Directory domain (for AD-synchronised domains; optional)"), adRow));
         return DialogFields.Scroll(panel);
      }

      private void GenerateDkim()
      {
         string selector = dkimSelector_.Text.Trim();
         if (selector.Length == 0)
         {
            selector = "dkim";
            dkimSelector_.Text = selector;
         }

         var save = new Microsoft.Win32.SaveFileDialog
         {
            Title = L("Save the DKIM private key"),
            Filter = "PEM key files (*.pem)|*.pem|All files (*.*)|*.*",
            FileName = selector + "._domainkey." + domainName_ + ".pem"
         };
         try
         {
            string existing = dkimKeyFile_.Text.Trim();
            if (existing.Length > 0)
            {
               string dir = System.IO.Path.GetDirectoryName(existing);
               if (!string.IsNullOrEmpty(dir) && System.IO.Directory.Exists(dir))
                  save.InitialDirectory = dir;
            }
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck)) { /* Deliberately ignored: best effort only, and the outcome of the surrounding operation does not depend on this succeeding. */ }

         if (save.ShowDialog() != true)
            return;

         try
         {
            DkimKeyGenerator.Result result = DkimKeyGenerator.Generate(selector, domainName_);
            System.IO.File.WriteAllText(save.FileName, result.PrivateKeyPem);
            dkimKeyFile_.Text = save.FileName;
            dkimOn_.IsChecked = true;
            dkimDnsValue_ = result.DnsTxtValue;
            dkimDns_.Text = F("Host/Name:  {0}", result.DnsHost) + Environment.NewLine +
                            L("Type:       TXT") + Environment.NewLine +
                            F("Value:      {0}", result.DnsTxtValue);
            dkimDnsPanel_.Visibility = Visibility.Visible;
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            notice_.Show(StatusLevel.Critical, F("Could not generate the DKIM key: {0}", ex.Message));
         }
      }

      private void BrowseAdDomain(object sender, RoutedEventArgs e)
      {
         notice_.Hide();

         if (!ActiveDirectoryService.IsAvailable(out string reason))
         {
            notice_.Show(StatusLevel.Warning, F("Active Directory is not available on this machine: {0}", reason));
            return;
         }

         System.Collections.Generic.List<string> domains = ActiveDirectoryService.ListDomains();
         if (domains == null || domains.Count == 0)
         {
            notice_.Show(StatusLevel.Warning, L("No Active Directory domains were found."));
            return;
         }

         var menu = new ContextMenu();
         foreach (string dom in domains)
         {
            var item = new MenuItem { Header = dom };
            item.Click += (s2, e2) => adDomain_.Text = dom;
            menu.Items.Add(item);
         }
         menu.PlacementTarget = (UIElement)sender;
         menu.IsOpen = true;
      }

      private FrameworkElement BuildNames()
      {
         aliasEditor_ = CollectionSpecs.DomainAliases(domainName_);
         aliasEditor_.Margin = new Thickness(0, DesignTokens.Space.Md, 0, 0);
         return aliasEditor_;
      }

      private FrameworkElement BuildSenderList()
      {
         senderListEditor_ = CollectionSpecs.DomainSenderList(domainName_);
         senderListEditor_.Margin = new Thickness(0, DesignTokens.Space.Md, 0, 0);
         return senderListEditor_;
      }

      private ScrollViewer BuildQuarantineDigest()
      {
         var panel = DialogFields.TabPanel();
         panel.Children.Add(DialogFields.Note(L("One message to each account of this domain that has something newly held for it, listing what was held with a link that delivers it. A link works once and stops working after the hours set on the Quarantine page, which is also where the address the links point at is set - without that address nothing is sent. An empty digest is never sent, and each message says how to stop it.")));
         panel.Children.Add(DialogFields.Check(digestOn_));
         digestHourRow_ = Label(L("Send it during this _hour of the local day (0 to 23; the sends are spread across the hour)"), digestHour_);
         panel.Children.Add(digestHourRow_);
         return DialogFields.Scroll(panel);
      }

      private ScrollViewer BuildReportsReceived()
      {
         var panel = DialogFields.TabPanel();
         panel.Children.Add(DialogFields.Note(L("The DMARC aggregate reports and the TLS-RPT reports that the receivers of this domain's mail send back are read from one address of the domain's, and the Control Deck charts who sends as the domain and with what result. Publish the address as rua=mailto: in the domain's _dmarc and _smtp._tls TXT records. Make the account or alias at it yourself; the server does not, and with no address named nothing is read.")));
         panel.Children.Add(Label(L("Report _mailbox (one address in a domain this server hosts; empty reads nothing)"), reportMailbox_));
         return DialogFields.Scroll(panel);
      }

      /// <summary>
      ///    One value, three levels (docs/SettingLevels.md): the settings this
      ///    domain may hold its own value of, with what is in effect for its
      ///    accounts and where that came from. Written by the panel itself, at once.
      /// </summary>
      private ScrollViewer BuildLevels()
      {
         var levels = new SettingLevelsPanel(domainName_, null, false);
         levels.Reload();
         return DialogFields.Scroll(levels);
      }

      private ScrollViewer BuildRestrictedDelivery()
      {
         var panel = DialogFields.TabPanel();
         panel.Children.Add(DialogFields.Note(L("Accounts of this domain may exchange mail with the domains named here and with nobody else: an outside sender not in a named domain is refused at RCPT TO, and an account here may send only to the named domains, its own domain and the postmaster. Other domains this server hosts count as outside unless named. Bounces always get through. Each account follows this unless its own dialog says restricted or exempt; empty means the domain is not restricted. An alias or a distribution list of this domain that reaches outside is yours, and a restricted account may write to it.")));
         panel.Children.Add(Label(L("Named _domains (partner.example, or .example for the domain and everything beneath it; separated by commas or spaces; empty restricts nothing)"), restrictedDomains_));
         return DialogFields.Scroll(panel);
      }

      private ScrollViewer BuildCountryPolicy()
      {
         var panel = DialogFields.TabPanel();
         panel.Children.Add(DialogFields.Note(L("Who may sign in to an account of this domain, by the country a MaxMind database places the address in: on every protocol, after the server's own policies for connecting, which are on the Auto-ban page with the database file. Nothing is judged until that file is set. A refused sign-in gets the ordinary wrong-password answer and is not counted against the account or the address.")));
         panel.Children.Add(Label(L("_Country policy at sign-in (empty = off; allow:GB,IE or block:CN,RU)"), countryPolicy_));
         return DialogFields.Scroll(panel);
      }

      private ScrollViewer BuildOpenPgp()
      {
         var panel = DialogFields.TabPanel();
         panel.Children.Add(DialogFields.Note(L("Publishing puts an account's OpenPGP public keys where other people's mail programs look for them: the Web Key Directory, served by the web services listener. A key is published only while the server's switch on the Webmail page, this domain's and the account's own are all on.")));
         panel.Children.Add(DialogFields.Check(publishKeys_));
         panel.Children.Add(DialogFields.Check(publishNewAccounts_));
         panel.Children.Add(DialogFields.Note(L("Applies to the accounts made from now on; it changes no account that already exists.")));
         return DialogFields.Scroll(panel);
      }

      private ScrollViewer BuildWebhooks()
      {
         var panel = DialogFields.TabPanel();
         panel.Children.Add(DialogFields.Note(L("Outbound webhooks post events to the receivers subscribed in the Control Deck or over the REST API. A subscription that names no domain covers every domain, and switching this off takes the domain out of all of them. The server's own switch is on the Logging page.")));
         panel.Children.Add(DialogFields.Check(webhooksOn_));
         panel.Children.Add(DialogFields.Check(webhookSubjects_));
         panel.Children.Add(DialogFields.Note(L("Off by default: a subject line is what a message is about. It travels only where this and the subscription both allow it, and only on message.received and message.quarantined.")));
         return DialogFields.Scroll(panel);
      }

      private ScrollViewer BuildImpersonation()
      {
         var panel = DialogFields.TabPanel();
         panel.Children.Add(DialogFields.Note(L("Two checks on mail from outside, judged as the tagging tab judges outside: a sender whose display name is the name of an active account here or of a protected person, while the address is not theirs; and a sender domain one edit or one look-alike character from a domain this server hosts or a protected partner. Flag adds a header the webmail shows as a warning; quarantine holds the copy as a spam verdict would, and an administrator's release delivers it without checking again. A sender the reader marked safe is exempt, and so is a sender domain that is itself hosted here or a partner.")));
         impersonationAction_.Items.Add(DialogFields.Combo(L("Off"), 0));
         impersonationAction_.Items.Add(DialogFields.Combo(L("Flag: add a header the reader sees as a warning"), 1));
         impersonationAction_.Items.Add(DialogFields.Combo(L("Quarantine: hold the copy until it is released"), 2));
         panel.Children.Add(Label(L("_Action"), impersonationAction_));
         panel.Children.Add(Label(L("Protected _persons (people outside this domain's accounts whose name must not be borrowed; one per line, each Name <address>, Name, or address)"), protectedPersons_));
         panel.Children.Add(Label(L("Protected pa_rtners (domains whose look-alikes are judged as this server's own are; separated by commas or spaces; ASCII, xn-- for an internationalised name)"), protectedPartners_));
         return DialogFields.Scroll(panel);
      }

      private ScrollViewer BuildLimits()
      {
         var panel = DialogFields.TabPanel();
         maxSizeRow_ = Label(L("Maximum _domain size (MB, 0 = unlimited)"), maxSize_);
         panel.Children.Add(maxSizeRow_);
         maxMessageSizeRow_ = Label(L("Maximum _message size (KB, 0 = unlimited)"), maxMessageSize_);
         panel.Children.Add(maxMessageSizeRow_);
         maxAccountSizeRow_ = Label(L("Maximum size for _accounts created in this domain (MB, 0 = unlimited)"), maxAccountSize_);
         panel.Children.Add(maxAccountSizeRow_);
         retentionRow_ = Label(L("Delete messages in this domain's mailboxes _older than (days; 0 = no policy, an account's own value overrides it)"), retentionDays_);
         panel.Children.Add(retentionRow_);
         panel.Children.Add(DialogFields.Separator());

         panel.Children.Add(DialogFields.Note(L("The quota policy for this domain's mailboxes: where the owner is warned and in what words, how far past the limit incoming mail is still accepted, and where sending stops. -1 and an empty text mean the server's own values apply.")));
         quotaWarnAtRow_ = Label(L("_Warn the owner at (% of the limit; -1 = the server's, 0 = never)"), quotaWarnAt_);
         panel.Children.Add(quotaWarnAtRow_);
         panel.Children.Add(Label(L("Warning s_ubject (empty = the server message)"), quotaSubject_));
         panel.Children.Add(Label(L("Warning te_xt (empty = the server message; %MACRO_MAILBOX%, %MACRO_QUOTA%, %MACRO_USED%, %MACRO_PERCENT%)"), quotaText_));
         quotaGraceRow_ = Label(L("Still accept incoming mail past the limit _by (% of it; -1 = the server's, 0 = not at all)"), quotaGrace_);
         panel.Children.Add(quotaGraceRow_);
         quotaStopAtRow_ = Label(L("Stop sending at (% of the limit; -1 = the server's, 0 = ne_ver)"), quotaStopAt_);
         panel.Children.Add(quotaStopAtRow_);
         panel.Children.Add(DialogFields.Separator());

         panel.Children.Add(DialogFields.Check(maxAccountsOn_));
         maxAccountsRow_ = NumberUnder(maxAccountsOn_, maxAccounts_);
         panel.Children.Add(maxAccountsRow_);
         panel.Children.Add(DialogFields.Check(maxAliasesOn_));
         maxAliasesRow_ = NumberUnder(maxAliasesOn_, maxAliases_);
         panel.Children.Add(maxAliasesRow_);
         panel.Children.Add(DialogFields.Check(maxDistsOn_));
         maxDistsRow_ = NumberUnder(maxDistsOn_, maxDists_);
         panel.Children.Add(maxDistsRow_);

         panel.Children.Add(DialogFields.Separator());
         panel.Children.Add(DialogFields.Check(plusAddressingOn_));
         panel.Children.Add(Label(L("_Plus addressing character"), plusChar_));
         panel.Children.Add(DialogFields.Check(greylisting_));
         return DialogFields.Scroll(panel);
      }

      private ScrollViewer BuildSignature()
      {
         signatureMethod_.Items.Add(DialogFields.Combo(L("Use only if account has no signature"), 1));
         signatureMethod_.Items.Add(DialogFields.Combo(L("Overwrite account signature"), 2));
         signatureMethod_.Items.Add(DialogFields.Combo(L("Append to account signature"), 3));

         var panel = DialogFields.TabPanel();
         panel.Children.Add(DialogFields.Check(signatureOn_));
         panel.Children.Add(Label(L("Signature _method"), signatureMethod_));
         panel.Children.Add(DialogFields.Check(signReplies_));
         panel.Children.Add(DialogFields.Check(signLocal_));
         panel.Children.Add(Label(L("_Plain-text signature"), signaturePlain_));
         panel.Children.Add(Label(L("_HTML signature"), signatureHtml_));
         return DialogFields.Scroll(panel);
      }

      private ScrollViewer BuildRelay()
      {
         relaySecurity_.Items.Add(DialogFields.Combo(L("None"), 0));
         relaySecurity_.Items.Add(DialogFields.Combo(L("SSL/TLS"), 1));
         relaySecurity_.Items.Add(DialogFields.Combo(L("STARTTLS (optional)"), 2));
         relaySecurity_.Items.Add(DialogFields.Combo(L("STARTTLS (required)"), 3));

         var panel = DialogFields.TabPanel();
         panel.Children.Add(DialogFields.Note(L("Where mail FROM this domain leaves through. This is a different question from a route, which decides where mail addressed TO a domain is sent - it is for a server hosting several independent domains where each has its own delivery provider.")));
         panel.Children.Add(DialogFields.Note(L("Leave the host empty and the domain has no opinion: the server-wide SMTP relayer applies, exactly as it did before this setting existed. A route still wins over both, because a route is a statement about the destination.")));
         panel.Children.Add(DialogFields.Separator());
         panel.Children.Add(Label(L("_Relay host (empty = use the server-wide relayer)"), relayHost_));
         relayPortRow_ = Label(L("_Port (0 = 25)"), relayPort_);
         panel.Children.Add(relayPortRow_);
         panel.Children.Add(Label(L("_Connection security"), relaySecurity_));
         panel.Children.Add(DialogFields.Separator());
         panel.Children.Add(DialogFields.Check(relayAuthOn_));
         panel.Children.Add(Label(L("_User name"), relayUser_));
         panel.Children.Add(Label(L("Pass_word (leave blank to keep the stored one)"), relayPassword_));
         return DialogFields.Scroll(panel);
      }

      private ScrollViewer BuildOutOfOffice()
      {
         var panel = DialogFields.TabPanel();
         panel.Children.Add(DialogFields.Note(L("A reply for the whole domain - a closed office, a decommissioned department. It answers only for accounts that have NO vacation message of their own: an account's own message always wins, and at most one reply answers any message.")));
         panel.Children.Add(DialogFields.Note(L("The usual auto-reply protections apply and cannot be switched off here: no reply to bounces, mailing lists or other auto-replies, one reply per sender, and the reply itself is marked Auto-Submitted so two servers cannot loop.")));
         panel.Children.Add(DialogFields.Separator());
         panel.Children.Add(DialogFields.Check(oooOn_));
         panel.Children.Add(Label(L("Su_bject"), oooSubject_));
         panel.Children.Add(Label(L("_Message"), oooMessage_));
         panel.Children.Add(DialogFields.Separator());
         panel.Children.Add(DialogFields.Note(L("Optional different text for local senders - colleagues can be told more than strangers. Empty means everyone gets the text above. Note this identifies the sender's ADDRESS, which is forgeable: treat it as a courtesy, never as a place for anything confidential.")));
         panel.Children.Add(Label(L("Subject for _local senders (empty = same as above)"), oooInternalSubject_));
         panel.Children.Add(Label(L("Message for local s_enders (empty = same as above)"), oooInternalMessage_));
         panel.Children.Add(Label(L("Answer the same sender a_gain after this many days (0 = once per absence)"), oooDays_));
         panel.Children.Add(DialogFields.Separator());
         panel.Children.Add(DialogFields.Check(oooExternalOverride_));
         panel.Children.Add(DialogFields.Note(L("With this on, an account's own vacation message still answers colleagues, but outside senders get the domain's generic text instead - so personal detail in a vacation message stays inside the organisation.")));
         return DialogFields.Scroll(panel);
      }

      private ScrollViewer BuildTransforms()
      {
         var panel = DialogFields.TabPanel();
         panel.Children.Add(DialogFields.Note(L("Mail from outside the organisation can be marked so that a reader sees it at a glance. A sender is outside when the session did not sign in, the message did not arrive through a trusted incoming relay, and neither the envelope sender nor the From address belongs to a domain this server hosts.")));
         panel.Children.Add(DialogFields.Separator());
         panel.Children.Add(DialogFields.Check(tagSubject_));
         panel.Children.Add(DialogFields.Note(L("The subject tag breaks any DKIM signature the sender made over their subject, exactly as the anti-spam subject prefix does. The header does not, so prefer the header where the reader can show it.")));
         tagTextRow_ = Label(L("Tag te_xt (empty = [EXTERNAL], at most 100 characters)"), tagText_);
         panel.Children.Add(tagTextRow_);
         panel.Children.Add(DialogFields.Check(tagHeader_));
         panel.Children.Add(DialogFields.Separator());
         panel.Children.Add(DialogFields.Check(firstContact_));
         panel.Children.Add(DialogFields.Note(L("The server remembers who has written to each account and who each account has written to, and says so in the reader the first time an outside sender appears. It costs one database lookup for each message delivered to this domain, and nothing at all while this is off.")));
         panel.Children.Add(DialogFields.Separator());
         panel.Children.Add(DialogFields.Check(disclaimerOn_));
         panel.Children.Add(DialogFields.Note(L("Added once, to mail with at least one recipient outside this server. Never to a signed or encrypted message, an automatic reply, a bounce or a list posting, and never a second time to a reply that already quotes it.")));
         panel.Children.Add(Label(L("_Plain-text disclaimer"), disclaimerPlain_));
         panel.Children.Add(Label(L("HTM_L disclaimer (empty = the plain text, with its line breaks)"), disclaimerHtml_));
         return DialogFields.Scroll(panel);
      }

      private ScrollViewer BuildDkim()
      {
         dkimHeaderCanon_.Items.Add(DialogFields.Combo(L("Simple"), 1));
         dkimHeaderCanon_.Items.Add(DialogFields.Combo(L("Relaxed"), 2));
         dkimBodyCanon_.Items.Add(DialogFields.Combo(L("Simple"), 1));
         dkimBodyCanon_.Items.Add(DialogFields.Combo(L("Relaxed"), 2));
         dkimAlgorithm_.Items.Add(DialogFields.Combo(L("SHA1"), 1));
         dkimAlgorithm_.Items.Add(DialogFields.Combo(L("SHA256"), 2));

         var panel = DialogFields.TabPanel();
         panel.Children.Add(DialogFields.Check(dkimOn_));
         panel.Children.Add(DialogFields.Check(dkimAliases_));
         panel.Children.Add(Label(L("Se_lector"), dkimSelector_));

         var dkimKeyRow = new Grid();
         dkimKeyRow.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
         dkimKeyRow.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
         Grid.SetColumn(dkimKeyFile_, 0);
         dkimKeyRow.Children.Add(dkimKeyFile_);
         var dkimBrowse = new Wpf.Ui.Controls.Button { Content = L("B_rowse…"), Margin = new Thickness(DesignTokens.Space.Sm, 0, 0, 0) };
         AutomationProperties.SetAutomationId(dkimBrowse, "DkimKeyBrowse");
         dkimBrowse.Click += (s, e) =>
         {
            string file = PathPicker.PickFile(dkimKeyFile_.Text, "PEM/key files (*.pem;*.key)|*.pem;*.key|All files (*.*)|*.*");
            if (file != null)
               dkimKeyFile_.Text = file;
         };
         Grid.SetColumn(dkimBrowse, 1);
         dkimKeyRow.Children.Add(dkimBrowse);
         panel.Children.Add(DialogFields.Field(L("Private key file"), dkimKeyRow));

         var dkimGen = new Wpf.Ui.Controls.Button
         {
            Content = L("_Generate key pair…"),
            Appearance = Wpf.Ui.Controls.ControlAppearance.Secondary,
            Margin = new Thickness(0, 0, 0, DesignTokens.Space.Md)
         };
         AutomationProperties.SetAutomationId(dkimGen, "DkimGenerate");
         dkimGen.Click += (s, e) => GenerateDkim();
         panel.Children.Add(dkimGen);

         dkimDnsPanel_ = new StackPanel { Visibility = Visibility.Collapsed };
         dkimDnsPanel_.Children.Add(DialogFields.Field(L("Publish this DNS TXT record at your DNS provider, then enable DKIM:"), dkimDns_));
         var dkimCopy = new Wpf.Ui.Controls.Button { Content = L("_Copy DNS value"), Margin = new Thickness(0, 0, 0, DesignTokens.Space.Md) };
         AutomationProperties.SetAutomationId(dkimCopy, "DkimCopyDns");
         dkimCopy.Click += (s, e) => CopyToClipboard(dkimDnsValue_);
         dkimDnsPanel_.Children.Add(dkimCopy);
         panel.Children.Add(dkimDnsPanel_);

         panel.Children.Add(Label(L("_Header canonicalization"), dkimHeaderCanon_));
         panel.Children.Add(Label(L("_Body canonicalization"), dkimBodyCanon_));
         panel.Children.Add(Label(L("Signing _algorithm"), dkimAlgorithm_));

         panel.Children.Add(DialogFields.Separator());
         panel.Children.Add(BuildRotation());

         return DialogFields.Scroll(panel);
      }

      /// <summary>
      /// The key rotation: the schedule, then the rotation as the server reports it and
      /// the five things it can be asked to do. It is a <see cref="SettingsSection"/>,
      /// which makes its title a level-2 heading under the dialog's own - a screen
      /// reader can jump to it.
      /// </summary>
      private SettingsSection BuildRotation()
      {
         rotKeyBits_.Items.Add(DialogFields.Combo("2048", 2048)); // no-loc
         rotKeyBits_.Items.Add(DialogFields.Combo("3072", 3072)); // no-loc
         rotKeyBits_.Items.Add(DialogFields.Combo("4096", 4096)); // no-loc

         var section = new StackPanel();
         section.Children.Add(DialogFields.Check(rotOn_));
         rotIntervalRow_ = Label(L("Days bet_ween rotations (1 to 3650)"), rotInterval_);
         section.Children.Add(rotIntervalRow_);
         rotPropagationRow_ = Label(L("_Propagation period in hours (1 to 720)"), rotPropagation_);
         section.Children.Add(rotPropagationRow_);
         rotRetentionRow_ = Label(L("Retention period in _days (1 to 365)"), rotRetention_);
         section.Children.Add(rotRetentionRow_);
         section.Children.Add(Label(L("Key si_ze"), rotKeyBits_));
         section.Children.Add(DialogFields.Note(L("The schedule is saved with the domain. The buttons below act at once. While the rotation manages the domain, the selector and key file above are the rotation's to change.")));

         section.Children.Add(DialogFields.Separator());
         section.Children.Add(rotStatus_);
         section.Children.Add(rotState_);

         var buttons = new WrapPanel { Margin = new Thickness(0, DesignTokens.Space.Md, 0, 0) };

         rotStart_ = new Wpf.Ui.Controls.Button { Content = L("Rotate _now…"), Margin = new Thickness(0, 0, DesignTokens.Space.Sm, DesignTokens.Space.Sm) };
         AutomationProperties.SetAutomationId(rotStart_, "DkimRotationStart");
         rotStart_.Click += (s, e) => StartRotation();
         buttons.Children.Add(rotStart_);

         rotCheck_ = new Wpf.Ui.Controls.Button { Content = L("Check DNS n_ow"), Margin = new Thickness(0, 0, DesignTokens.Space.Sm, DesignTokens.Space.Sm) };
         AutomationProperties.SetAutomationId(rotCheck_, "DkimRotationCheck");
         rotCheck_.Click += (s, e) => RotationCall(d => (string)d.DKIMRotationCheck(), L("The records were checked."));
         buttons.Children.Add(rotCheck_);

         rotSwitch_ = new Wpf.Ui.Controls.Button { Content = L("Sw_itch now…"), Appearance = Wpf.Ui.Controls.ControlAppearance.Primary, Margin = new Thickness(0, 0, DesignTokens.Space.Sm, DesignTokens.Space.Sm) };
         AutomationProperties.SetAutomationId(rotSwitch_, "DkimRotationSwitch");
         rotSwitch_.Click += (s, e) => SwitchRotation();
         buttons.Children.Add(rotSwitch_);

         rotRetire_ = new Wpf.Ui.Controls.Button { Content = L("Retire the old ke_y now…"), Margin = new Thickness(0, 0, DesignTokens.Space.Sm, DesignTokens.Space.Sm) };
         AutomationProperties.SetAutomationId(rotRetire_, "DkimRotationRetire");
         rotRetire_.Click += (s, e) => RetireRotation();
         buttons.Children.Add(rotRetire_);

         rotCancel_ = new Wpf.Ui.Controls.Button { Content = L("Cancel the ne_xt key…"), Margin = new Thickness(0, 0, DesignTokens.Space.Sm, DesignTokens.Space.Sm) };
         AutomationProperties.SetAutomationId(rotCancel_, "DkimRotationCancel");
         rotCancel_.Click += (s, e) => CancelRotation();
         buttons.Children.Add(rotCancel_);

         section.Children.Add(buttons);

         return new SettingsSection
         {
            Heading = L("Key rotation"),
            Description = L("The server generates the next key, shows the TXT record to publish, checks it from its own resolver, and moves signing to it only once the record has resolved for the whole propagation period. The old selector is kept for the retention period and then retired."),
            Content = section
         };
      }

      // ---- DKIM key rotation ----

      /// <summary>Draws the rotation from the last status the server gave.</summary>
      private void DrawRotation()
      {
         rotState_.Children.Clear();

         DkimRotationStatus status = rotation_;
         bool known = status != null;

         rotStart_.IsEnabled = known && status.Next == null;
         rotCheck_.IsEnabled = known;
         rotSwitch_.IsEnabled = known && status.Next != null;
         rotCancel_.IsEnabled = known && status.Next != null;
         rotRetire_.IsEnabled = known && status.Retiring != null;

         if (!known)
         {
            rotState_.Children.Add(DialogFields.Note(rotationSupported_
               ? L("The server did not report the key rotation.")
               : L("This server does not rotate DKIM keys; it is older than this Control Panel.")));
            return;
         }

         rotState_.Children.Add(DialogFields.Body(F("Phase: {0}", DkimRotationStatus.PhaseText(status.Phase))));

         if (!string.IsNullOrEmpty(status.LastError))
         {
            var failed = DialogFields.Notice();
            failed.Show(StatusLevel.Warning, F("The last attempt failed at {0}: {1}", DkimRotationStatus.When(status.LastErrorTime), status.LastError));
            rotState_.Children.Add(failed);
         }

         if (status.Current == null)
            rotState_.Children.Add(DialogFields.Note(L("The domain has no signing key yet.")));

         DrawSlot(L("Current key"), status.Current, status.Current == null ? null : new[]
         {
            F("Key file: {0}", status.Current.KeyFile),
            status.Current.Since.HasValue ? F("Since {0}", DkimRotationStatus.When(status.Current.Since)) : null,
            status.Current.NextRotationDue.HasValue ? F("Next rotation due {0}", DkimRotationStatus.When(status.Current.NextRotationDue)) : null
         });

         DrawSlot(L("Next key"), status.Next, status.Next == null ? null : new[]
         {
            F("Key file: {0}", status.Next.KeyFile),
            status.Next.Created.HasValue ? F("Created {0}", DkimRotationStatus.When(status.Next.Created)) : null,
            status.Next.FirstSeen.HasValue ? F("First seen {0}", DkimRotationStatus.When(status.Next.FirstSeen)) : null,
            status.Next.SwitchDue.HasValue ? F("Switch due {0}", DkimRotationStatus.When(status.Next.SwitchDue)) : null
         });

         DrawSlot(L("Retiring key"), status.Retiring, status.Retiring == null ? null : new[]
         {
            status.Retiring.Since.HasValue ? F("Since {0}", DkimRotationStatus.When(status.Retiring.Since)) : null,
            status.Retiring.RetireDue.HasValue ? F("Retire due {0}", DkimRotationStatus.When(status.Retiring.RetireDue)) : null
         });

         DrawSlot(L("Retired key"), status.Retired, status.Retired == null ? null : new[]
         {
            status.Retired.At.HasValue ? F("Retired {0}", DkimRotationStatus.When(status.Retired.At)) : null,
            status.Retired.KeyFileKept.Length > 0 ? F("The key file was kept: {0}", status.Retired.KeyFileKept) : null
         });

         if (status.Waiting.Count > 0)
            rotState_.Children.Add(DialogFields.Separator());

         foreach (DkimRotationStatus.Wait wait in status.Waiting)
         {
            string sentence = DkimRotationStatus.ReasonText(wait.Reason);
            rotState_.Children.Add(DialogFields.Note(wait.Until.HasValue
               ? F("{0} Until {1}.", sentence, DkimRotationStatus.When(wait.Until))
               : sentence));
         }
      }

      private void DrawSlot(string title, DkimRotationStatus.Slot slot, string[] facts)
      {
         if (slot == null)
            return;

         rotState_.Children.Add(DialogFields.Separator());
         rotState_.Children.Add(DialogFields.Body(F("{0}: {1}", title, slot.Selector)));

         foreach (string fact in (facts ?? Array.Empty<string>()).Where(f => !string.IsNullOrEmpty(f)))
            rotState_.Children.Add(DialogFields.Note(fact));

         if (slot.Check != null)
         {
            var (level, word) = DkimRotationStatus.CheckPresentation(slot.Check.Status);
            string sentence = slot.Check.Checked.HasValue ? F("{0}, checked {1}.", word, DkimRotationStatus.When(slot.Check.Checked)) : word;
            if (slot.Check.Detail.Length > 0)
               sentence += " " + slot.Check.Detail;

            var notice = DialogFields.Notice();
            notice.Show(level, sentence);
            rotState_.Children.Add(notice);
         }

         foreach (DkimRotationStatus.Record record in slot.Records)
         {
            rotState_.Children.Add(DialogFields.Caption(F("{0}: TXT record at {1}", DkimRotationStatus.ActionText(record.Action), record.Host)));

            if (record.Value.Length == 0)
               continue;

            TextBox value = ReadOnlyBox(0);
            value.Text = record.Value;
            AutomationProperties.SetName(value, F("TXT record value for {0}", record.Host));
            rotState_.Children.Add(value);

            var copy = new Wpf.Ui.Controls.Button { Content = L("Copy _value"), Margin = new Thickness(0, DesignTokens.Space.Xs, 0, DesignTokens.Space.Sm) };
            string text = record.Value;
            copy.Click += (s, e) => CopyToClipboard(text);
            rotState_.Children.Add(copy);
         }
      }

      /// <summary>Reads the rotation from a domain object the caller holds.</summary>
      private void ReadRotation(dynamic domain)
      {
         try
         {
            rotation_ = DkimRotationStatus.Parse((string)domain.DKIMRotationStatus);
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            rotation_ = null;
            if (rotationSupported_)
               rotStatus_.Show(StatusLevel.Critical, F("Could not read the key rotation: {0}", ServerSession.DescribeComError(ex)));
         }
      }

      /// <summary>
      /// One operation on a domain object read now, its answer drawn, and the selector
      /// and key file boxes above brought up to what the server holds afterwards - a
      /// switch moves them, and a later Save must not write the old ones back.
      /// </summary>
      private void RotationCall(Func<dynamic, string> call, string done)
      {
         rotStatus_.Hide();
         dynamic domains = ServerSession.Current.Application.Domains;
         try
         {
            dynamic d = domains.ItemByName[domainName_];
            try
            {
               try
               {
                  rotation_ = DkimRotationStatus.Parse(call(d));
                  rotStatus_.Show(StatusLevel.Good, done);
               }
               catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
               {
                  // The server's own sentence: it knows why, and what to do.
                  rotStatus_.Show(StatusLevel.Warning, F("The rotation refused: {0}", ServerSession.DescribeComError(ex)));
                  ReadRotation(d);
               }

               loadedSelector_ = ((string)d.DKIMSelector ?? "").Trim();
               loadedKeyFile_ = ((string)d.DKIMPrivateKeyFile ?? "").Trim();
               dkimSelector_.Text = loadedSelector_;
               dkimKeyFile_.Text = loadedKeyFile_;
            }
            finally
            {
               ServerSession.Release(d);
            }
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            rotStatus_.Show(StatusLevel.Critical, F("Could not read the key rotation: {0}", ServerSession.DescribeComError(ex)));
         }
         finally
         {
            ServerSession.Release(domains);
         }

         DrawRotation();
      }

      private void StartRotation()
      {
         string selector = DialogFields.PromptText(this, L("Rotate now"),
            L("Selector for the new key (leave it empty for one named after today's date)"));
         if (selector == null)
            return;

         selector = selector.Trim();
         RotationCall(d => (string)d.DKIMRotationStart(selector), L("A new key was generated and staged. Publish its record."));
      }

      private void SwitchRotation()
      {
         if (MessageBox.Show(F("Move signing for {0} to the next key now?", domainName_),
                             L("Control Panel"), MessageBoxButton.YesNo, MessageBoxImage.Question) != MessageBoxResult.Yes)
            return;

         // Before the propagation period is over the server refuses a plain switch,
         // so the second question is asked here rather than after a refusal.
         bool force = false;
         DateTimeOffset? due = rotation_?.Next?.SwitchDue;
         if (!due.HasValue || due.Value > DateTimeOffset.UtcNow)
         {
            if (MessageBox.Show(L("The new record has not resolved for the whole propagation period yet. Move signing anyway? Receivers whose resolvers have not caught up fail the new signatures until they do. The record itself must still resolve: the server checks it first either way."),
                                L("Control Panel"), MessageBoxButton.YesNo, MessageBoxImage.Warning) != MessageBoxResult.Yes)
               return;
            force = true;
         }

         RotationCall(d => (string)d.DKIMRotationSwitch(force), L("Signing moved to the next key."));
      }

      private void RetireRotation()
      {
         if (MessageBox.Show(F("Retire the old selector of {0} now? Its TXT record may then be removed, and a key file the rotation generated is deleted.", domainName_),
                             L("Control Panel"), MessageBoxButton.YesNo, MessageBoxImage.Warning) != MessageBoxResult.Yes)
            return;

         RotationCall(d => (string)d.DKIMRotationRetire(), L("The old selector was retired."));
      }

      private void CancelRotation()
      {
         if (MessageBox.Show(F("Cancel the next key of {0}? The staged selector is removed, a key file the rotation generated for it is deleted, and signing does not change.", domainName_),
                             L("Control Panel"), MessageBoxButton.YesNo, MessageBoxImage.Warning) != MessageBoxResult.Yes)
            return;

         RotationCall(d => (string)d.DKIMRotationCancel(), L("The next key was cancelled."));
      }

      private static void CopyToClipboard(string text)
      {
         ClipboardWriter.TrySetText(text);   // best effort, as it always was: the value stays on screen to copy
      }

      private void Load()
      {
         dynamic domains = ServerSession.Current.Application.Domains;
         try
         {
            dynamic d = domains.ItemByName[domainName_];
            active_.IsChecked = (bool)d.Active;
            name_.Text = (string)d.Name ?? domainName_;
            postmaster_.Text = (string)d.Postmaster ?? "";
            adDomain_.Text = (string)d.ADDomainName ?? "";

            maxSize_.Text = ((int)d.MaxSize).ToString();
            maxMessageSize_.Text = ((int)d.MaxMessageSize).ToString();
            maxAccountSize_.Text = ((int)d.MaxAccountSize).ToString();
            retentionDays_.Text = ((int)d.MessageRetentionDays).ToString();
            quotaWarnAt_.Text = ((int)d.QuotaWarningPercent).ToString();
            quotaSubject_.Text = (string)d.QuotaWarningSubject ?? "";
            quotaText_.Text = (string)d.QuotaWarningText ?? "";
            quotaGrace_.Text = ((int)d.QuotaGracePercent).ToString();
            quotaStopAt_.Text = ((int)d.QuotaSendStopPercent).ToString();
            maxAccountsOn_.IsChecked = (bool)d.MaxNumberOfAccountsEnabled;
            maxAccounts_.Text = ((int)d.MaxNumberOfAccounts).ToString();
            maxAliasesOn_.IsChecked = (bool)d.MaxNumberOfAliasesEnabled;
            maxAliases_.Text = ((int)d.MaxNumberOfAliases).ToString();
            maxDistsOn_.IsChecked = (bool)d.MaxNumberOfDistributionListsEnabled;
            maxDists_.Text = ((int)d.MaxNumberOfDistributionLists).ToString();
            plusAddressingOn_.IsChecked = (bool)d.PlusAddressingEnabled;
            plusChar_.Text = (string)d.PlusAddressingCharacter ?? "";
            greylisting_.IsChecked = (bool)d.AntiSpamEnableGreylisting;

            signatureOn_.IsChecked = (bool)d.SignatureEnabled;
            DialogFields.SelectCombo(signatureMethod_, (int)d.SignatureMethod);
            signReplies_.IsChecked = (bool)d.AddSignaturesToReplies;
            signLocal_.IsChecked = (bool)d.AddSignaturesToLocalMail;
            signaturePlain_.Text = (string)d.SignaturePlainText ?? "";
            signatureHtml_.Text = (string)d.SignatureHTML ?? "";

            relayHost_.Text = (string)d.RelayHost ?? "";
            relayPort_.Text = ((long)d.RelayPort).ToString();
            relayAuthOn_.IsChecked = (bool)d.RelayRequiresAuthentication;
            relayUser_.Text = (string)d.RelayUsername ?? "";
            DialogFields.SelectCombo(relaySecurity_, (int)d.RelayConnectionSecurity);

            // The password box is deliberately left empty rather than filled with the
            // stored secret: a dialog that shows a password back is a dialog that hands
            // it to whoever is standing behind you, and it buys nothing - Save keeps the
            // stored value when the box is blank.
            relayPassword_.Password = "";

            oooOn_.IsChecked = (bool)d.VacationMessageIsOn;
            oooSubject_.Text = (string)d.VacationSubject ?? "";
            oooMessage_.Text = (string)d.VacationMessage ?? "";
            oooInternalSubject_.Text = (string)d.VacationInternalSubject ?? "";
            oooInternalMessage_.Text = (string)d.VacationInternalMessage ?? "";
            oooExternalOverride_.IsChecked = (bool)d.VacationExternalOverride;
            oooDays_.Text = ((int)d.VacationDays).ToString();

            tagSubject_.IsChecked = (bool)d.ExternalTagSubject;
            tagHeader_.IsChecked = (bool)d.ExternalTagHeader;
            tagText_.Text = (string)d.ExternalTagText ?? "";
            firstContact_.IsChecked = (bool)d.FirstContactTip;
            disclaimerOn_.IsChecked = (bool)d.DisclaimerEnabled;
            disclaimerPlain_.Text = (string)d.DisclaimerPlainText ?? "";
            disclaimerHtml_.Text = (string)d.DisclaimerHTML ?? "";

            dkimOn_.IsChecked = (bool)d.DKIMSignEnabled;
            dkimAliases_.IsChecked = (bool)d.DKIMSignAliasesEnabled;
            dkimSelector_.Text = (string)d.DKIMSelector ?? "";
            dkimKeyFile_.Text = (string)d.DKIMPrivateKeyFile ?? "";
            DialogFields.SelectCombo(dkimHeaderCanon_, (int)d.DKIMHeaderCanonicalizationMethod);
            DialogFields.SelectCombo(dkimBodyCanon_, (int)d.DKIMBodyCanonicalizationMethod);
            DialogFields.SelectCombo(dkimAlgorithm_, (int)d.DKIMSigningAlgorithm);

            loadedSelector_ = dkimSelector_.Text.Trim();
            loadedKeyFile_ = dkimKeyFile_.Text.Trim();

            // The rotation's policy and state. Read defensively: against an older
            // server these properties do not exist, and the rest of the dialog must
            // keep working without them.
            try
            {
               rotOn_.IsChecked = (bool)d.DKIMRotationEnabled;
               rotInterval_.Text = ((int)d.DKIMRotationIntervalDays).ToString();
               rotPropagation_.Text = ((int)d.DKIMRotationPropagationHours).ToString();
               rotRetention_.Text = ((int)d.DKIMRotationRetentionDays).ToString();
               DialogFields.SelectCombo(rotKeyBits_, (int)d.DKIMRotationKeyBits, orFirst: true);
               rotationSupported_ = true;
            }
            catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
            {
               rotationSupported_ = false;
            }

            rotOn_.IsEnabled = rotationSupported_;
            rotInterval_.IsEnabled = rotationSupported_;
            rotPropagation_.IsEnabled = rotationSupported_;
            rotRetention_.IsEnabled = rotationSupported_;
            rotKeyBits_.IsEnabled = rotationSupported_;

            if (rotationSupported_)
               ReadRotation(d);
            DrawRotation();

            // The digest, defensively for the same reason: a server older than the
            // feature has no such properties, and the tab then shows its defaults
            // rather than taking the dialog down.
            try
            {
               digestOn_.IsChecked = (bool)d.QuarantineDigest;
               digestHour_.Text = ((int)d.QuarantineDigestHour).ToString();
            }
            catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
            {
               digestOn_.IsEnabled = false;
               digestHour_.IsEnabled = false;
            }

            // The report mailbox, the same way: a server from before schema 6070
            // has no such property, and the box is then disabled rather than
            // saved back as empty.
            try
            {
               reportMailbox_.Text = (string)d.ReportMailbox ?? "";
            }
            catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
            {
               reportMailbox_.IsEnabled = false;
            }

            // The restricted domains, the same way: Domain.RestrictedDomains
            // arrived with schema 6084, and a server without it leaves the box
            // disabled rather than saved back as empty.
            try
            {
               restrictedDomains_.Text = (string)d.RestrictedDomains ?? "";
            }
            catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
            {
               restrictedDomains_.IsEnabled = false;
            }

            // Impersonation protection, the same way: the three properties
            // arrived with schema 6087, and a server without them leaves the
            // controls disabled rather than saved back as off and empty.
            try
            {
               DialogFields.SelectCombo(impersonationAction_, (int)d.ImpersonationAction);
               protectedPersons_.Text = (string)d.ProtectedPersons ?? "";
               protectedPartners_.Text = (string)d.ProtectedPartners ?? "";
            }
            catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
            {
               impersonationAction_.IsEnabled = false;
               protectedPersons_.IsEnabled = false;
               protectedPartners_.IsEnabled = false;
            }

            // The country policy, OpenPGP publishing and the webhook switches, the
            // same way: each arrived with its own schema, and a server without one
            // leaves its controls disabled rather than saved back as empty or off.
            try
            {
               countryPolicy_.Text = (string)d.CountryPolicy ?? "";
            }
            catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
            {
               countryPolicy_.IsEnabled = false;
            }

            try
            {
               publishKeys_.IsChecked = (bool)d.OpenPGPPublishKeys;
               publishNewAccounts_.IsChecked = (bool)d.OpenPGPPublishNewAccounts;
            }
            catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
            {
               publishKeys_.IsEnabled = false;
               publishNewAccounts_.IsEnabled = false;
            }

            try
            {
               webhooksOn_.IsChecked = (bool)d.WebhooksEnabled;
               webhookSubjects_.IsChecked = (bool)d.WebhookSubjects;
            }
            catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
            {
               webhooksOn_.IsEnabled = false;
               webhookSubjects_.IsEnabled = false;
            }

            ServerSession.Release(d);
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            MessageBox.Show(F("Could not load the domain: {0}", ex.Message), L("Control Panel"));
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
         DialogFields.ClearErrors(nameRow_, maxSizeRow_, maxMessageSizeRow_, maxAccountSizeRow_, retentionRow_,
                                  maxAccountsRow_, maxAliasesRow_, maxDistsRow_, relayPortRow_, tagTextRow_, digestHourRow_,
                                  rotIntervalRow_, rotPropagationRow_, rotRetentionRow_);

         // Eight numbers, each said on its own field. They used to share one line
         // at the foot of the dialog, which named the field in words and left the
         // reader to find it again among seven tabs; ShowError brings the tab
         // forward and puts the keyboard in the box itself.
         if (!NumericField.TryValidate(maxSize_.Text, L("Maximum domain size (MB)"), 0, int.MaxValue, out int msV, out bool hasMs, out string error))
         { DialogFields.ShowError(maxSizeRow_, error); return; }

         if (!NumericField.TryValidate(maxMessageSize_.Text, L("Maximum message size (KB)"), 0, int.MaxValue, out int mmsV, out bool hasMms, out error))
         { DialogFields.ShowError(maxMessageSizeRow_, error); return; }

         if (!NumericField.TryValidate(maxAccountSize_.Text, L("Maximum account size (MB)"), 0, int.MaxValue, out int masV, out bool hasMas, out error))
         { DialogFields.ShowError(maxAccountSizeRow_, error); return; }

         if (!NumericField.TryValidate(retentionDays_.Text, L("Delete messages older than (days)"), 0, int.MaxValue, out int retentionV, out bool hasRetention, out error))
         { DialogFields.ShowError(retentionRow_, error); return; }

         if (!NumericField.TryValidate(quotaWarnAt_.Text, L("Warn the owner at (%)"), -1, 100, out int quotaWarnV, out bool hasQuotaWarn, out error))
         { DialogFields.ShowError(quotaWarnAtRow_, error); return; }

         if (!NumericField.TryValidate(quotaGrace_.Text, L("Still accept incoming mail past the limit by (%)"), -1, 1000, out int quotaGraceV, out bool hasQuotaGrace, out error))
         { DialogFields.ShowError(quotaGraceRow_, error); return; }

         if (!NumericField.TryValidate(quotaStopAt_.Text, L("Stop sending at (%)"), -1, 1000, out int quotaStopV, out bool hasQuotaStop, out error))
         { DialogFields.ShowError(quotaStopAtRow_, error); return; }

         if (!NumericField.TryValidate(maxAccounts_.Text, L("Maximum number of accounts"), 0, int.MaxValue, out int mnaV, out bool hasMna, out error))
         { DialogFields.ShowError(maxAccountsRow_, error); return; }

         if (!NumericField.TryValidate(maxAliases_.Text, L("Maximum number of aliases"), 0, int.MaxValue, out int mnalV, out bool hasMnal, out error))
         { DialogFields.ShowError(maxAliasesRow_, error); return; }

         if (!NumericField.TryValidate(maxDists_.Text, L("Maximum number of distribution lists"), 0, int.MaxValue, out int mndV, out bool hasMnd, out error))
         { DialogFields.ShowError(maxDistsRow_, error); return; }

         if (!NumericField.TryValidate(relayPort_.Text, L("Relay port"), 0, 65535, out int relayPortValue, out bool _, out error))
         { DialogFields.ShowError(relayPortRow_, error); return; }

         if (!NumericField.TryValidate(digestHour_.Text, L("Digest hour"), 0, 23, out int digestHourValue, out bool hasDigestHour, out error))
         { DialogFields.ShowError(digestHourRow_, error); return; }

         int rotationInterval = 0, rotationPropagation = 0, rotationRetention = 0;
         bool hasRotationInterval = false, hasRotationPropagation = false, hasRotationRetention = false;
         if (rotationSupported_)
         {
            if (!NumericField.TryValidate(rotInterval_.Text, L("Days between rotations"), 1, 3650, out rotationInterval, out hasRotationInterval, out error))
            { DialogFields.ShowError(rotIntervalRow_, error); return; }

            if (!NumericField.TryValidate(rotPropagation_.Text, L("Propagation period in hours"), 1, 720, out rotationPropagation, out hasRotationPropagation, out error))
            { DialogFields.ShowError(rotPropagationRow_, error); return; }

            if (!NumericField.TryValidate(rotRetention_.Text, L("Retention period in days"), 1, 365, out rotationRetention, out hasRotationRetention, out error))
            { DialogFields.ShowError(rotRetentionRow_, error); return; }
         }

         // Said here as well as by the server, because a tag the server truncated
         // would be a tag every reader learns to ignore.
         if (tagText_.Text.Trim().Length > 100)
         {
            DialogFields.ShowError(tagTextRow_, L("The tag text must be 100 characters or fewer."));
            return;
         }

         string newName = name_.Text.Trim();
         if (newName.Length == 0 || !newName.Contains('.'))
         {
            DialogFields.ShowError(nameRow_, L("Enter a valid domain name."));
            return;
         }

         // Renaming asks first, because it is not a label edit: the server renames
         // every account, alias and distribution list, rewrites references held by
         // OTHER domains (forwards, alias targets, list members), and moves the
         // message directories. The old admin did all of this silently on Save,
         // which is exactly how domains got renamed by accident.
         bool renaming = !string.Equals(newName, domainName_, StringComparison.OrdinalIgnoreCase);
         if (renaming &&
             MessageBox.Show(
                F("Rename {0} to {1}?\n\nEvery account, alias and distribution list moves to the new name, and forwards or memberships in other domains that point at {0} addresses are updated to match. Mail sent to the old name will no longer be accepted.", domainName_, newName),
                L("Control Panel"), MessageBoxButton.YesNo, MessageBoxImage.Question) != MessageBoxResult.Yes)
            return;

         dynamic domains = ServerSession.Current.Application.Domains;
         try
         {
            dynamic d = domains.ItemByName[domainName_];
            if (renaming)
               d.Name = newName;
            d.Active = active_.IsChecked is true;
            d.Postmaster = postmaster_.Text.Trim();
            d.ADDomainName = adDomain_.Text.Trim();

            if (hasMs) d.MaxSize = msV;
            if (hasMms) d.MaxMessageSize = mmsV;
            if (hasMas) d.MaxAccountSize = masV;
            if (hasRetention) d.MessageRetentionDays = retentionV;
            if (hasQuotaWarn) d.QuotaWarningPercent = quotaWarnV;
            d.QuotaWarningSubject = quotaSubject_.Text.Trim();
            d.QuotaWarningText = quotaText_.Text;
            if (hasQuotaGrace) d.QuotaGracePercent = quotaGraceV;
            if (hasQuotaStop) d.QuotaSendStopPercent = quotaStopV;
            d.MaxNumberOfAccountsEnabled = maxAccountsOn_.IsChecked is true;
            if (hasMna) d.MaxNumberOfAccounts = mnaV;
            d.MaxNumberOfAliasesEnabled = maxAliasesOn_.IsChecked is true;
            if (hasMnal) d.MaxNumberOfAliases = mnalV;
            d.MaxNumberOfDistributionListsEnabled = maxDistsOn_.IsChecked is true;
            if (hasMnd) d.MaxNumberOfDistributionLists = mndV;
            d.PlusAddressingEnabled = plusAddressingOn_.IsChecked is true;
            if (plusChar_.Text.Length > 0) d.PlusAddressingCharacter = plusChar_.Text;
            d.AntiSpamEnableGreylisting = greylisting_.IsChecked is true;

            d.SignatureEnabled = signatureOn_.IsChecked is true;
            int sm = DialogFields.ComboValue(signatureMethod_);
            if (sm > 0) d.SignatureMethod = sm;
            d.AddSignaturesToReplies = signReplies_.IsChecked is true;
            d.AddSignaturesToLocalMail = signLocal_.IsChecked is true;
            d.SignaturePlainText = signaturePlain_.Text;
            d.SignatureHTML = signatureHtml_.Text;

            d.RelayHost = relayHost_.Text.Trim();
            // Unconditional, as before: an empty box means port 0, which this
            // dialog's own caption reads as "0 = 25".
            d.RelayPort = relayPortValue;
            d.RelayRequiresAuthentication = relayAuthOn_.IsChecked is true;
            d.RelayUsername = relayUser_.Text.Trim();
            d.RelayConnectionSecurity = DialogFields.ComboValue(relaySecurity_);

            // Only written when something was typed, so re-saving the dialog for an
            // unrelated change does not silently blank the relay password.
            if (relayPassword_.Password.Length > 0)
               d.RelayPassword = relayPassword_.Password;

            d.VacationMessageIsOn = oooOn_.IsChecked is true;
            d.VacationSubject = oooSubject_.Text.Trim();
            d.VacationMessage = oooMessage_.Text;
            d.VacationInternalSubject = oooInternalSubject_.Text.Trim();
            d.VacationInternalMessage = oooInternalMessage_.Text;
            d.VacationExternalOverride = oooExternalOverride_.IsChecked is true;
            d.VacationDays = int.TryParse(oooDays_.Text.Trim(), out int oooDays) ? Math.Max(0, Math.Min(3650, oooDays)) : 0;

            d.ExternalTagSubject = tagSubject_.IsChecked is true;
            d.ExternalTagHeader = tagHeader_.IsChecked is true;
            d.ExternalTagText = tagText_.Text.Trim();
            d.FirstContactTip = firstContact_.IsChecked is true;
            d.DisclaimerEnabled = disclaimerOn_.IsChecked is true;
            d.DisclaimerPlainText = disclaimerPlain_.Text;
            d.DisclaimerHTML = disclaimerHtml_.Text;

            if (digestOn_.IsEnabled)
            {
               d.QuarantineDigest = digestOn_.IsChecked is true;
               if (hasDigestHour) d.QuarantineDigestHour = digestHourValue;
            }

            // The server judges the address - one in a domain it hosts, or empty -
            // and its refusal reaches the dialog as the save's error.
            if (reportMailbox_.IsEnabled)
               d.ReportMailbox = reportMailbox_.Text.Trim();

            // The server canonicalises the list and refuses a name that is not
            // a domain; its sentence reaches the dialog as the save's error.
            if (restrictedDomains_.IsEnabled)
               d.RestrictedDomains = restrictedDomains_.Text.Trim();

            // The server canonicalises both lists and refuses an entry that is
            // not what the list holds; its sentence reaches the dialog as the
            // save's error.
            if (impersonationAction_.IsEnabled)
            {
               int impersonation = DialogFields.ComboValue(impersonationAction_, -1);
               if (impersonation >= 0) d.ImpersonationAction = impersonation;
               d.ProtectedPersons = protectedPersons_.Text.Trim();
               d.ProtectedPartners = protectedPartners_.Text.Trim();
            }

            // The server canonicalises the policy and refuses one it cannot read;
            // its sentence reaches the dialog as the save's error.
            if (countryPolicy_.IsEnabled)
               d.CountryPolicy = countryPolicy_.Text.Trim();

            if (publishKeys_.IsEnabled)
            {
               d.OpenPGPPublishKeys = publishKeys_.IsChecked is true;
               d.OpenPGPPublishNewAccounts = publishNewAccounts_.IsChecked is true;
            }

            if (webhooksOn_.IsEnabled)
            {
               d.WebhooksEnabled = webhooksOn_.IsChecked is true;
               d.WebhookSubjects = webhookSubjects_.IsChecked is true;
            }

            d.DKIMSignEnabled = dkimOn_.IsChecked is true;
            d.DKIMSignAliasesEnabled = dkimAliases_.IsChecked is true;
            // Only when changed here. The object above was read now, so it already
            // holds whatever a rotation has moved the selector and key file to; the
            // boxes hold what they held when the dialog opened, unless somebody typed.
            if (dkimSelector_.Text.Trim() != loadedSelector_)
               d.DKIMSelector = dkimSelector_.Text.Trim();
            if (dkimKeyFile_.Text.Trim() != loadedKeyFile_)
               d.DKIMPrivateKeyFile = dkimKeyFile_.Text.Trim();

            if (rotationSupported_)
            {
               d.DKIMRotationEnabled = rotOn_.IsChecked is true;
               if (hasRotationInterval) d.DKIMRotationIntervalDays = rotationInterval;
               if (hasRotationPropagation) d.DKIMRotationPropagationHours = rotationPropagation;
               if (hasRotationRetention) d.DKIMRotationRetentionDays = rotationRetention;
               int bits = DialogFields.ComboValue(rotKeyBits_);
               if (bits > 0) d.DKIMRotationKeyBits = bits;
            }
            int hc = DialogFields.ComboValue(dkimHeaderCanon_);
            if (hc > 0) d.DKIMHeaderCanonicalizationMethod = hc;
            int bc = DialogFields.ComboValue(dkimBodyCanon_);
            if (bc > 0) d.DKIMBodyCanonicalizationMethod = bc;
            int alg = DialogFields.ComboValue(dkimAlgorithm_);
            if (alg > 0) d.DKIMSigningAlgorithm = alg;

            d.Save();
            ServerSession.Release(d);
            Close();
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            notice_.Show(StatusLevel.Critical, F("Could not save the domain: {0}", ex.Message));
         }
         finally
         {
            ServerSession.Release(domains);
         }
      }

      // ---- UI helpers ----

      private static TextBox NewInput() => new Wpf.Ui.Controls.TextBox();

      /// <summary>A selectable, read-only box in the style of the DKIM DNS-record
      /// display above: values the user must copy exactly (host names, TXT
      /// values) live in these rather than in labels, so they can be selected.</summary>
      private static TextBox ReadOnlyBox(double minHeight) => new()
      {
         IsReadOnly = true,
         AcceptsReturn = true,
         TextWrapping = TextWrapping.Wrap,
         FontSize = Typography.Caption,
         MinHeight = minHeight,
         Padding = new Thickness(DesignTokens.Space.Sm)
      };

      private static TextBox NewMemo() => new()
      {
         AcceptsReturn = true,
         Height = 80,
         TextWrapping = TextWrapping.Wrap,
         VerticalScrollBarVisibility = ScrollBarVisibility.Auto
      };

      /// <summary>
      /// The number that belongs to the tick box above it. The three limit boxes
      /// have no caption of their own and had no accessible name either, so each
      /// announced as a bare "edit" under a tick box whose words it needs; the box
      /// takes its name from that tick box, which is the only text there is, and
      /// the row it sits in carries the validation message for it.
      /// </summary>
      private static FieldRow NumberUnder(CheckBox owner, TextBox box)
      {
         AutomationProperties.SetName(box, MnemonicText.Strip((string)owner.Content));
         box.HorizontalAlignment = HorizontalAlignment.Left;
         box.MinWidth = 160;
         return DialogFields.Field(null, box);
      }

      /// <summary>
      /// A caption and its editor as one row, which names the editor to UI
      /// Automation. A TextBlock above a control tells UI Automation nothing, so
      /// without this every field in this dialog announced to a screen reader as
      /// an anonymous "edit".
      /// </summary>
      private static FieldRow Label(string text, FrameworkElement editor) => DialogFields.Field(text, editor);
   }
}
