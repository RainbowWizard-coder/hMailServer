// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Collections.Generic;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using hMailServer.ControlPanel.Services;
using static hMailServer.ControlPanel.Views.CollectionEditorView;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Views
{
   /// <summary>
   /// Factory for the data-driven list panes. Each method returns a fully
   /// configured <see cref="CollectionEditorView"/> bound to a COM collection.
   /// </summary>
   public static class CollectionSpecs
   {
      private static dynamic AntiSpam => ServerSession.Current.Application.Settings.AntiSpam;
      private static dynamic Settings => ServerSession.Current.Application.Settings;

      /// <summary>
      /// hMailServer's eConnectionSecurity, in the order the enum declares it, and
      /// worded the same way the SSL/TLS and Delivery pages word it so the same
      /// choice does not read as two different things in two places. The wording
      /// carries the port each one implies, because choosing the security without
      /// changing the port is the single commonest way an external account is
      /// configured and then never downloads anything.
      /// </summary>
      private static readonly (int Value, string Label)[] ServerTypeOptions =
      {
         (0, L("POP3 - download, then delete from the server as Days to keep says")),
         (1, L("IMAP - the INBOX, collected once by UID; deleted from the server only as Days to keep says"))
      };

      private static readonly (int Value, string Label)[] ConnectionSecurityOptions =
      {
         (0, L("None - no encryption")),
         (1, L("SSL/TLS - encrypted from the first byte")),
         (2, L("STARTTLS, optional - upgrade if offered, continue in the clear if not")),
         (3, L("STARTTLS, required - refuse to download unless the upgrade succeeds"))
      };

      public static CollectionEditorView SurblServers() => new(new CollectionSpec
      {
         PageKey = "surbl",
         Subtitle = L("Spam URI Realtime Block Lists. Messages whose body links resolve on these hosts gain spam score."),
         ItemNoun = L("SURBL server"),
         AddTitle = N("Add a SURBL server"),
         EditTitle = N("Edit the SURBL server"),
         CountOne = N("1 SURBL server"),
         CountMany = N("{0} SURBL servers"),
         SearchPlaceholder = N("Search SURBL servers"),
         DeletePrompt = N("Delete the SURBL server {0}?"),
         DeleteNameProps = new[] { "DNSHost" }, // no-loc: the properties that name an entry
         EmptyText = L("No SURBL servers, so the links in a message body are not looked up in any list."),
         GetCollection = () => AntiSpam.SURBLServers,
         TickBoxesLast = true,
         ActiveProp = "Active", // no-loc: the property that switches an entry on
         Fields =
         {
            // A new list starts active, its box after the fields that say what
            // the list is (TickBoxesLast): one added and saved was never asked
            // (review finding F174). The host is required. The expected result
            // is not: empty, any answer but the list's own error codes is a
            // listing (SURBL.cpp). Each field says what goes in it.
            new FieldSpec { Prop = "Active", Label = L("Active"), Kind = FieldKind.Bool, GridWidth = 70, Default = true },
            new FieldSpec { Prop = "DNSHost", Label = L("DNS host"), Required = true, Hint = L("Such as multi.surbl.org or dbl.spamhaus.org") },
            new FieldSpec { Prop = "ExpectedResult", Label = L("Expected result"), GridWidth = 130, Hint = L("The answer that means listed: 127.0.0.2, a range such as 127.0.0.2-126, or several joined by |. Left empty, any answer but the list's own error codes is a listing.") },
            new FieldSpec { Prop = "Score", Label = L("Score"), Kind = FieldKind.Number, GridWidth = 80, Default = 5, Hint = L("Added to the message's spam score when this list matches") },
            // A noun, not "Reject message", which read as a command and was
            // translated as one - "Nachricht ablehnen" (review finding F268).
            // Last, in the order the DNS blacklists page shows its columns too.
            new FieldSpec { Prop = "RejectMessage", Label = L("Refusal text"), Hint = L("What the sender is told when this list refuses the message") }
         }
      });

      public static CollectionEditorView DnsBlackLists() => new(new CollectionSpec
      {
         PageKey = "dnsbl",
         ItemNoun = L("block list"),
         AddTitle = N("Add a DNS block list"),
         EditTitle = N("Edit the DNS block list"),
         CountOne = N("1 block list"),
         CountMany = N("{0} block lists"),
         SearchPlaceholder = N("Search DNS block lists"),
         DeletePrompt = N("Delete the DNS block list {0}?"),
         DeleteNameProps = new[] { "DNSHost" }, // no-loc: the properties that name an entry
         EmptyText = L("No block lists, so connecting addresses are not looked up in any."),
         GetCollection = () => AntiSpam.DNSBlackLists,
         TickBoxesLast = true,
         ActiveProp = "Active", // no-loc: the property that switches an entry on
         Fields =
         {
            // As for a SURBL server, but the expected result is required here:
            // the server expands it into the answers that mean listed, and an
            // empty one matches no answer at all (BLCheck::ClientExistsInDNSBL),
            // so a list saved without one was never a match (review finding F174).
            new FieldSpec { Prop = "Active", Label = L("Active"), Kind = FieldKind.Bool, GridWidth = 70, Default = true },
            new FieldSpec { Prop = "DNSHost", Label = L("DNS host"), Required = true, Hint = L("Such as zen.spamhaus.org or bl.spamcop.net") },
            new FieldSpec { Prop = "ExpectedResult", Label = L("Expected result"), GridWidth = 130, Required = true, Hint = L("The answer that means listed: 127.0.0.2, a range such as 127.0.0.2-11, a wildcard such as 127.0.0.*, or several joined by |") },
            new FieldSpec { Prop = "Score", Label = L("Score"), Kind = FieldKind.Number, GridWidth = 80, Default = 5, Hint = L("Added to the message's spam score when this list matches") },
            new FieldSpec { Prop = "RejectMessage", Label = L("Refusal text"), Hint = L("What the sender is told when this list refuses the message") }
         }
      });

      public static CollectionEditorView SpamWhiteList() => new(new CollectionSpec
      {
         PageKey = "spamwhitelist",
         ItemNoun = L("spam exemption"),
         AddTitle = N("Add a spam exemption"),
         EditTitle = N("Edit the spam exemption"),
         CountOne = N("1 spam exemption"),
         CountMany = N("{0} spam exemptions"),
         SearchPlaceholder = N("Search spam exemptions"),
         DeletePrompt = N("Delete the spam exemption {0}? Mail it covers goes through the spam checks again."),
         DeleteNameProps = new[] { "Description", "EmailAddress", "LowerIPAddress" }, // no-loc: the properties that name an entry
         EmptyText = L("No exemptions, so no sender is let past spam protection by this list."),
         GetCollection = () => AntiSpam.WhiteListAddresses,
         Fields =
         {
            new FieldSpec { Prop = "LowerIPAddress", Label = L("Lower IP"), GridWidth = 150 },
            new FieldSpec { Prop = "UpperIPAddress", Label = L("Upper IP"), GridWidth = 150 },
            new FieldSpec { Prop = "EmailAddress", Label = L("Email address") },
            new FieldSpec { Prop = "Description", Label = L("Description") }
         }
      });

      public static CollectionEditorView BlockedSenders() => new(new CollectionSpec
      {
         PageKey = "blockedsenders",
         Subtitle = L("Envelope senders refused outright, or scored. An entry with an @ is one exact address; without one it is a whole domain including its subdomains."),
         ItemNoun = L("blocked sender"),
         AddTitle = N("Add a blocked sender"),
         EditTitle = N("Edit the blocked sender"),
         CountOne = N("1 blocked sender"),
         CountMany = N("{0} blocked senders"),
         SearchPlaceholder = N("Search blocked senders"),
         DeletePrompt = N("Delete the blocked sender {0}?"),
         DeleteNameProps = new[] { "Address" }, // no-loc: the properties that name an entry
         EmptyText = L("No blocked senders, so no envelope sender is refused or scored for its address alone."),
         GetCollection = () => AntiSpam.BlockedSenders,
         Fields =
         {
            new FieldSpec { Prop = "Address", Label = L("Address or domain"), GridWidth = 260, Required = true, Hint = L("An address such as spammer@example.com is that address alone; a domain such as example.com is the domain and its subdomains.") },
            // A number, as the server's property is, where it was a text box whose
            // default was the string "100" (review finding F209).
            new FieldSpec { Prop = "Score", Label = L("Score"), Kind = FieldKind.Number, GridWidth = 80, Default = 100, Hint = L("Added to the message's spam score; 100, the default, refuses the message during the SMTP conversation.") },
            new FieldSpec { Prop = "Description", Label = L("Description") }
         }
      });

      /// <summary>
      /// The server's allow and block list: one row per address or domain, allowed
      /// or blocked, with the reason kept. The same rows at domain scope are edited
      /// on the domain's own dialog, and an account's are its owner's, in the
      /// webmail. Consulted at delivery, after the account's own list and its
      /// domain's - the nearest scope with an opinion is the only one that decides.
      /// </summary>
      public static CollectionEditorView SenderList() => new(new CollectionSpec
      {
         PageKey = "senderlist",
         SearchPlaceholder = N("Search the allow and block list"),
         Subtitle = L("Addresses and whole domains that every account wants, or does not. An allowed sender's mail skips the spam verdict; a blocked sender's is held in the quarantine where that is on, and marked as spam where it is not - never deleted."),
         ItemNoun = L("list entry"),
         AddTitle = N("Add a list entry"),
         EditTitle = N("Edit the list entry"),
         CountOne = N("1 list entry"),
         CountMany = N("{0} list entries"),
         DeletePrompt = N("Delete the list entry {0}?"),
         DeleteNameProps = new[] { "Value" }, // no-loc: the properties that name an entry
         EmptyText = L("No entries, so this list allows and blocks nobody; an account's own list and its domain's still apply."),
         GetCollection = () => AntiSpam.SenderList,
         Fields = SenderListFields(260)
      });

      /// <summary>
      /// The allow and block list's fields, the server's and a domain's alike.
      /// In the grid the dialog's sentences are short headers: Action, which
      /// reads Block or Allow where the tick box's "Block rather than allow" read
      /// "Block rath…" and its cells Yes and No; and one Expires column, showing
      /// the date, or Never, where the grid showed whether a date was set and
      /// kept the date itself off it.
      /// </summary>
      private static List<FieldSpec> SenderListFields(double valueWidth) => new()
      {
         new FieldSpec { Prop = "Value", Label = L("Address, domain or URL (https://host/...)"), GridLabel = L("Address or domain"), GridWidth = valueWidth },
         new FieldSpec { Prop = "Blocked", Label = L("Block rather than allow"), GridLabel = L("Action"), GridTrue = L("Block"), GridFalse = L("Allow"), Kind = FieldKind.Bool, GridWidth = 90, Default = false },
         new FieldSpec { Prop = "Reason", Label = L("Reason") },
         new FieldSpec { Prop = "Expires", Label = L("Stops mattering at a date"), Kind = FieldKind.Bool, ShowInGrid = false, Default = false },
         new FieldSpec { Prop = "ExpiresTime", Label = L("Expires on (YYYY-MM-DD HH:MM:SS)"), GridLabel = L("Expires"), GridOnlyWhen = "Expires", GridEmpty = L("Never"), GridWidth = 150 } // no-loc: GridOnlyWhen names the Bool property
      };

      public static CollectionEditorView GreyListWhiteList() => new(new CollectionSpec
      {
         PageKey = "greylistwhitelist",
         ItemNoun = L("IP address"),
         AddTitle = N("Add a greylisting exemption"),
         EditTitle = N("Edit the greylisting exemption"),
         CountOne = N("1 IP address"),
         CountMany = N("{0} IP addresses"),
         SearchPlaceholder = N("Search greylisting exemptions"),
         DeletePrompt = N("Delete the greylisting exemption {0}? Mail from it is greylisted again."),
         DeleteNameProps = new[] { "IPAddress" }, // no-loc: the properties that name an entry
         EmptyText = L("No addresses, so no server is let past greylisting for its IP address."),
         GetCollection = () => AntiSpam.GreyListingWhiteAddresses,
         StateNotice = GreylistingStateNotice_,
         Fields =
         {
            new FieldSpec { Prop = "IPAddress", Label = L("IP address"), GridWidth = 200 },
            new FieldSpec { Prop = "Description", Label = L("Description") }
         }
      });

      /// <summary>
      /// GreyListingBypassEntry.MatchOn, in the order the enum declares it, worded
      /// as what is compared rather than what the value is called.
      /// </summary>
      private static readonly (int Value, string Label)[] GreyListBypassMatchOptions =
      {
         (1, L("The envelope sender's domain (MAIL FROM)")),
         (2, L("The HELO/EHLO host")),
         (3, L("Either of them"))
      };

      /// <summary>
      /// The per-domain greylisting bypass list (discussion #284): a privilege
      /// granted to a named domain on the authentication results its mail
      /// carries, beside the global "Bypass on SPF success", which every SPF
      /// pass in the world enjoys - a hijacked mailbox at a well-configured
      /// provider included. Empty by default, and then greylisting is unchanged.
      /// </summary>
      public static CollectionEditorView GreyListBypass() => new(new CollectionSpec
      {
         PageKey = "greylistbypass",
         Subtitle = L("Envelope domains and HELO suffixes let past greylisting when the message carries the authentication results the entry requires - SPF, DKIM, DMARC, any combination."),
         ItemNoun = L("bypass entry"),
         AddTitle = N("Add a bypass entry"),
         EditTitle = N("Edit the bypass entry"),
         CountOne = N("1 bypass entry"),
         CountMany = N("{0} bypass entries"),
         SearchPlaceholder = N("Search the greylisting bypass list"),
         DeletePrompt = N("Delete the greylisting bypass entry {0}? Mail it matched is greylisted again."),
         DeleteNameProps = new[] { "Pattern" }, // no-loc: the properties that name an entry
         EmptyText = L("No entries, so no domain is let past greylisting for the authentication results its mail carries."),
         GetCollection = () => AntiSpam.GreyListingBypassEntries,
         StateNotice = GreylistingStateNotice_,
         Fields =
         {
            new FieldSpec { Prop = "Pattern", Label = L("Domain, or .suffix"), GridWidth = 220, Default = "" },
            new FieldSpec { Prop = "MatchOn", Label = L("Matched against"), Kind = FieldKind.Combo, Options = GreyListBypassMatchOptions, GridWidth = 200, Default = 1 },
            // Headed by the result's name alone, reading Required or a dash: the
            // three tick boxes' captions headed 80-pixel columns, in which all
            // three read "Requires…".
            new FieldSpec { Prop = "RequireSPF", Label = L("Requires an SPF pass"), GridLabel = L("SPF"), GridTrue = L("Required"), GridFalse = "—", Kind = FieldKind.Bool, GridWidth = 72, Default = true },
            new FieldSpec { Prop = "RequireDKIM", Label = L("Requires a verified DKIM signature (decided after DATA)"), GridLabel = L("DKIM"), GridTrue = L("Required"), GridFalse = "—", Kind = FieldKind.Bool, GridWidth = 72, Default = false },
            new FieldSpec { Prop = "RequireDMARC", Label = L("Requires a DMARC pass (decided after DATA)"), GridLabel = L("DMARC"), GridTrue = L("Required"), GridFalse = "—", Kind = FieldKind.Bool, GridWidth = 72, Default = false },
            new FieldSpec { Prop = "Note", Label = L("Note"), Default = "" }
         }
      });

      public static CollectionEditorView BlockedAttachments() => new(new CollectionSpec
      {
         PageKey = "blockedattachments",
         // What the server does with a match (VirusScanner::BlockAttachments
         // replaces the part's content with the server message ATTACHMENT_REMOVED
         // and adds .txt to its name), for every message it delivers; whether it
         // does so now is the switch's, said by the notice above the list. The
         // entries are patterns throughout, where the page called them
         // wildcards, its pill rules and the virus overview patterns.
         Subtitle = L("Attachments whose file names match one of these patterns are replaced by a short note saying so before the message is delivered."),
         ItemNoun = L("pattern"),
         AddTitle = N("Add a blocked attachment pattern"),
         EditTitle = N("Edit the blocked attachment pattern"),
         CountOne = N("1 pattern"),
         CountMany = N("{0} patterns"),
         SearchPlaceholder = N("Search blocked attachment patterns"),
         DeletePrompt = N("Delete the blocked attachment pattern {0}? Attachments it matched are no longer replaced."),
         DeleteNameProps = new[] { "Wildcard" }, // no-loc: the properties that name an entry
         EmptyText = L("No patterns, so no attachment is replaced for its file name."),
         GetCollection = () => Settings.AntiVirus.BlockedAttachments,
         StateNotice = BlockingStateNotice_,
         Fields =
         {
            new FieldSpec { Prop = "Wildcard", Label = L("File-name pattern"), GridWidth = 220, Default = "*.exe", Hint = L("Such as *.exe or invoice*.zip: * stands for any run of characters and ? for any one.") },
            new FieldSpec { Prop = "Description", Label = L("Description") }
         }
      });

      /// <summary>
      /// What the blocked attachments are while attachment blocking is off: a
      /// Warning, since a list of patterns reads as patterns at work, and none is
      /// (review finding F267). Nothing while it is on.
      /// </summary>
      private static ListStateNotice BlockingStateNotice_(int patterns)
      {
         if ((bool)Settings.AntiVirus.EnableAttachmentBlocking)
            return null;

         return new ListStateNotice
         {
            Level = StatusLevel.Warning,
            Text = patterns == 0 ? L("Attachment blocking is switched off, so a pattern added here is not applied until it is turned on.")
               : patterns == 1 ? L("Attachment blocking is switched off, so this pattern is not applied.")
               : Plural(patterns,
                  "Attachment blocking is switched off, so none of these {0} pattern is applied.",
                  "Attachment blocking is switched off, so none of these {0} patterns is applied."),
            Page = "antivirus",
            ActionCaption = L("_Open Anti-virus settings")
         };
      }

      /// <summary>
      /// What the greylisting lists are while greylisting is off: Information,
      /// since an exemption from a delay nobody suffers changes nothing and is
      /// no fault (review finding F267). Nothing while it is on.
      /// </summary>
      private static ListStateNotice GreylistingStateNotice_(int _)
      {
         if ((bool)AntiSpam.GreyListingEnabled)
            return null;

         return new ListStateNotice
         {
            Level = StatusLevel.Information,
            Text = L("Greylisting is switched off: no sender is delayed, so this list changes nothing until it is turned on."),
            Page = "antispam",
            ActionCaption = L("_Open Anti-spam settings")
         };
      }

      /// <summary>
      /// The Groups page. Not a bare <see cref="CollectionEditorView"/> like its
      /// neighbours, because a group is more than its name: it is a member list,
      /// and without a way to edit that list here a group could be created and
      /// granted folder permissions but never actually cover anyone - the feature
      /// only worked over the COM API.
      /// </summary>
      public static UserControl Groups() => new GroupsPageView(new CollectionSpec
      {
         PageKey = "groups",
         Subtitle = L("Security groups used to grant shared-folder (IMAP ACL) permissions to several accounts at once. A group's permissions cover exactly the accounts on its member list - select a group and choose Members to edit that list."),
         ItemNoun = L("security group"),
         AddTitle = N("Add a security group"),
         EditTitle = N("Edit the security group"),
         CountOne = N("1 security group"),
         CountMany = N("{0} security groups"),
         SearchPlaceholder = N("Search security groups"),
         DeletePrompt = N("Delete the security group {0}? The folder permissions and calendar shares given to it are removed, so its members lose them."),
         DeleteNameProps = new[] { "Name" }, // no-loc: the properties that name an entry
         EmptyText = L("No groups yet. A group gives several accounts the same folder permissions at once, and Members chooses which."),
         GetCollection = () => Settings.Groups,
         Fields =
         {
            new FieldSpec { Prop = "Name", Label = L("Group name") }
         }
      });

      /// <summary>
      /// hMailServer's eRemoteTlsRequirement, in the order the enum declares it.
      ///
      /// The wording says what the server will do rather than what the value is
      /// called, because the difference between "encrypted" and "verified" is the
      /// difference between a partner whose self-signed certificate is fine and a
      /// bank whose is not, and nobody should have to read the roadmap to know
      /// which one they are choosing.
      /// </summary>
      private static readonly (int Value, string Label)[] RemoteTlsOptions =
      {
         (0, L("Whatever the server would do anyway - including MTA-STS and DANE")),
         (1, L("Encrypted - STARTTLS must succeed; the certificate is not judged")),
         (2, L("Verified - and the certificate must chain and name the host")),
         (3, L("DANE - and the certificate must match a DNSSEC-validated TLSA record"))
      };

      /// <summary>
      /// hMailServer's eRemoteEncryption: gateway encryption, the message itself
      /// encrypted at this server to the partner's key (docs/RemoteDomains.md).
      /// </summary>
      private static readonly (int Value, string Label)[] RemoteEncryptionOptions =
      {
         (0, L("None - sent as it was written")),
         (1, L("OpenPGP - to the partner's public key")),
         (2, L("S/MIME - to the partner's certificate"))
      };

      /// <summary>
      /// What happens to a message the policy says to encrypt and that cannot be.
      /// The refusal first, because it is the default and the reason to have
      /// the setting at all.
      /// </summary>
      private static readonly (int Value, string Label)[] RemoteEncryptFallbackOptions =
      {
         (0, L("Refuse it and tell the sender")),
         (1, L("Send it unencrypted and log it"))
      };

      /// <summary>
      /// What this server will do when it talks to a named remote domain.
      ///
      /// Three questions an administrator asks about a partner, a customer or a
      /// provider, in one record because they are one subject: how much transport
      /// security is demanded of it in each direction, what this server will
      /// attempt to push at it, and whether a recipient is checked with the
      /// domain's own server before mail for it is accepted.
      ///
      /// Deliberately not the Routes page. A route says WHERE mail for a domain
      /// goes and replaces the MX lookup; this says what this server will and
      /// will not do when it gets there, whatever found the server, and a domain
      /// may have both.
      /// </summary>
      public static CollectionEditorView RemoteDomains() => new(new CollectionSpec
      {
         PageKey = "remotedomains",
         Subtitle = L("What this server does when it talks to a named remote domain: the TLS it demands of the connection in each direction, the largest message and the number of connections it will attempt, whether an automatic reply or a forward may go there, and whether a recipient is verified with the domain's own server before mail for it is accepted. A name may carry a wildcard - the most specific active entry governs a delivery. A TLS requirement here is added to whatever the domain publishes for itself: an MTA-STS or DANE policy is never weakened by an entry on this page. Recipient verification is for a domain this server is a backup MX for, and for nothing else."),
         ItemNoun = L("remote domain policy"),
         AddTitle = N("Add a remote domain policy"),
         EditTitle = N("Edit the remote domain policy"),
         CountOne = N("1 remote domain policy"),
         CountMany = N("{0} remote domain policies"),
         SearchPlaceholder = N("Search remote domains"),
         DeletePrompt = N("Delete the remote domain policy {0}? Mail to and from that domain follows the server-wide settings again."),
         DeleteNameProps = new[] { "DomainName" }, // no-loc: the properties that name an entry
         EmptyText = L("No policies, so the server talks to every remote domain on its general settings."),
         GetCollection = () => Settings.RemoteDomainPolicies,
         Fields =
         {
            // First in the dialog as well as the grid: the policy's master
            // switch, on a dialog of twenty fields that scrolls, where an Edit
            // opened to switch the policy off must find it in sight.
            new FieldSpec { Prop = "Active", Label = L("Active"), Kind = FieldKind.Bool, GridWidth = 70, Default = true },
            new FieldSpec { Prop = "DomainName", Label = L("Domain or pattern"), GridWidth = 200, Default = "" },
            new FieldSpec { Prop = "OutboundTls", Label = L("Outbound TLS"), Kind = FieldKind.Combo, Options = RemoteTlsOptions, GridWidth = 160, Default = 0 },
            // Short headers over the narrow columns, the dialog's sentences in
            // their tool tips: "Require TLS on mail from this domain" read
            // "Require TLS o" over 90 pixels.
            new FieldSpec { Prop = "RequireInboundTls", Label = L("Require TLS on mail from this domain"), GridLabel = L("Inbound TLS"), GridTrue = L("Required"), GridFalse = "—", Kind = FieldKind.Bool, GridWidth = 125, Default = false },
            new FieldSpec { Prop = "MaxMessageSizeKB", Label = L("Largest message (KB; 0 = no limit)"), GridLabel = L("Largest (KB)"), Kind = FieldKind.Number, GridWidth = 110, Default = 0 },
            new FieldSpec { Prop = "MaxConnections", Label = L("Simultaneous connections (0 = no limit)"), GridLabel = L("Connections"), Kind = FieldKind.Number, GridWidth = 110, Default = 0 },
            new FieldSpec { Prop = "MaxMessagesPerMinute", Label = L("Messages a minute (0 = no limit)"), Kind = FieldKind.Number, ShowInGrid = false, Default = 0 },
            new FieldSpec { Prop = "AllowAutomaticReplies", Label = L("Automatic replies may go to this domain"), Kind = FieldKind.Bool, ShowInGrid = false, Default = true },
            new FieldSpec { Prop = "AllowForwarding", Label = L("Mail may be forwarded to this domain"), Kind = FieldKind.Bool, ShowInGrid = false, Default = true },
            new FieldSpec { Prop = "CalloutEnabled", Label = L("Verify a recipient with the domain's own server"), GridLabel = L("Recipient check"), Kind = FieldKind.Bool, GridWidth = 125, Default = false },
            new FieldSpec { Prop = "CalloutHost", Label = L("The server to ask (empty = the domain's MX hosts)"), ShowInGrid = false, Default = "" },
            new FieldSpec { Prop = "CalloutPort", Label = L("The port to ask on"), Kind = FieldKind.Number, ShowInGrid = false, Default = 25 },
            new FieldSpec { Prop = "CalloutTimeoutSeconds", Label = L("How long to wait (seconds)"), Kind = FieldKind.Number, ShowInGrid = false, Default = 10 },
            new FieldSpec { Prop = "CalloutCacheMinutes", Label = L("How long an answer is remembered (minutes)"), Kind = FieldKind.Number, ShowInGrid = false, Default = 60 },
            new FieldSpec { Prop = "CalloutMaxPerMinute", Label = L("How often one domain is asked, per minute"), Kind = FieldKind.Number, ShowInGrid = false, Default = 10 },
            // Gateway encryption. The key is written and never read back - the
            // server answers it empty - so the box opens empty and a key is sent
            // only when one is pasted; what is held is shown by the summary
            // below it, which the server writes: the fingerprint to check with
            // the partner, the expiry, and whether a message can be encrypted
            // to it now.
            new FieldSpec { Prop = "Encrypt", Label = L("Encrypt mail to this domain at this server"), GridLabel = L("Encryption"), Kind = FieldKind.Combo, Options = RemoteEncryptionOptions, GridWidth = 125, Default = 0 },
            new FieldSpec { Prop = "EncryptKey", Label = L("The partner's key (empty = keep the key held)"), Hint = L("An OpenPGP public key, or PEM certificates."), Kind = FieldKind.Multiline, WriteOnly = true, ShowInGrid = false, Default = "" },
            new FieldSpec { Prop = "EncryptKeySummary", Label = L("The key held"), ReadOnly = true, ShowInGrid = false },
            new FieldSpec { Prop = "EncryptFallback", Label = L("When a message cannot be encrypted"), Kind = FieldKind.Combo, Options = RemoteEncryptFallbackOptions, ShowInGrid = false, Default = 0 },
            new FieldSpec { Prop = "Description", Label = L("Description"), Default = "" }
         }
      });

      public static CollectionEditorView ServerMessages() => new(new CollectionSpec
      {
         PageKey = "servermessages",
         Subtitle = L("The text templates the server returns to clients (greetings, bounce and error messages). These are a fixed set you can edit."),
         ItemNoun = L("server message"),
         EditTitle = N("Edit the server message"),
         CountOne = N("1 server message"),
         CountMany = N("{0} server messages"),
         SearchPlaceholder = N("Search server messages"),
         CanAdd = false,
         CanDelete = false,
         GetCollection = () => Settings.ServerMessages,
         Fields =
         {
            new FieldSpec { Prop = "Name", Label = L("Name"), GridWidth = 260 },
            new FieldSpec { Prop = "Text", Label = L("Text"), Kind = FieldKind.Multiline }
         }
      });

      // ---- per-object collections (embedded in dialogs) ----------------------

      private static dynamic OpenDomain(string domainName)
      {
         dynamic domains = ServerSession.Current.Application.Domains;
         dynamic domain = domains.ItemByName[domainName];
         ServerSession.Release(domains);
         return domain;
      }

      private static dynamic OpenAccount(string domainName, string address)
      {
         dynamic domain = OpenDomain(domainName);
         dynamic accounts = domain.Accounts;
         dynamic account = accounts.ItemByAddress[address];
         ServerSession.Release(accounts);
         ServerSession.Release(domain);
         return account;
      }

      /// <summary>One domain's allow and block list (embedded in the Domain editor).</summary>
      public static CollectionEditorView DomainSenderList(string domainName) => new(new CollectionSpec
      {
         Title = L("Allow and block list"),
         Subtitle = F("Addresses and whole domains that the accounts of {0} want, or do not. Consulted after an account's own list and before the server's.", domainName),
         SavesAsMadeInDialog = true,
         ItemNoun = L("list entry"),
         AddTitle = N("Add a list entry"),
         EditTitle = N("Edit the list entry"),
         CountOne = N("1 list entry"),
         CountMany = N("{0} list entries"),
         DeletePrompt = N("Delete the list entry {0}?"),
         DeleteNameProps = new[] { "Value" }, // no-loc: the properties that name an entry
         EmptyText = L("No entries, so this domain's list allows and blocks nobody; each account's own list and the server's still apply."),
         GetCollection = () =>
         {
            dynamic domain = OpenDomain(domainName);
            dynamic list = domain.SenderList;
            ServerSession.Release(domain);
            return list;
         },
         // 200 pixels of address, not the server page's 260: the grid has 564 in
         // the dialog's tab (640 less the frame's and the card's padding, the
         // borders and the vertical bar the grid always keeps), and its four
         // columns - the address, Action at 90, Reason's floor of 120 and Expires
         // at 150 - have to fit them, or the grid scrolls sideways by the
         // difference, under a bar, at the only width the dialog has.
         Fields = SenderListFields(200)
      }, embedded: true, onDialogTab: true);

      /// <summary>Domain-name aliases for one domain (embedded in the Domain editor).</summary>
      public static CollectionEditorView DomainAliases(string domainName) => new(new CollectionSpec
      {
         Title = L("Domain aliases"),
         Subtitle = F("Alternative domain names treated as this domain. Mail sent to user@alias is delivered to user@{0}.", domainName),
         SavesAsMadeInDialog = true,
         ItemNoun = L("alias"),
         AddTitle = N("Add a domain alias"),
         EditTitle = N("Edit the domain alias"),
         CountOne = N("1 alias"),
         CountMany = N("{0} aliases"),
         DeletePrompt = N("Delete the domain alias {0}? Mail to addresses at that name is no longer delivered to this domain."),
         DeleteNameProps = new[] { "AliasName" }, // no-loc: the properties that name an entry
         EmptyText = L("No aliases, so this domain receives mail under its own name only."),
         GetCollection = () =>
         {
            dynamic domain = OpenDomain(domainName);
            dynamic aliases = domain.DomainAliases;
            ServerSession.Release(domain);
            return aliases;
         },
         Fields =
         {
            // The example stays in the dialog's caption; the column is headed by the name alone.
            new FieldSpec { Prop = "AliasName", Label = L("Alias domain name (e.g. example.net)"), GridLabel = L("Alias domain"), Default = "" }
         }
      }, embedded: true, onDialogTab: true);

      /// <summary>External POP3 download (fetch) accounts for one account.</summary>
      public static CollectionEditorView FetchAccounts(string domainName, string address) => new(new CollectionSpec
      {
         Title = L("External accounts"),
         Subtitle = L("POP3 or IMAP mailboxes hMailServer collects mail from on behalf of this account. Connection security and port go together: SSL/TLS on 995 (POP3) or 993 (IMAP), STARTTLS on 110 or 143."),
         SavesAsMadeInDialog = true,
         ItemNoun = L("external account"),
         AddTitle = N("Add an external account"),
         EditTitle = N("Edit the external account"),
         CountOne = N("1 external account"),
         CountMany = N("{0} external accounts"),
         DeletePrompt = N("Delete the external account {0}? Mail is no longer downloaded from it; what was downloaded stays."),
         DeleteNameProps = new[] { "Name", "ServerAddress" }, // no-loc: the properties that name an entry
         EmptyText = L("No external accounts, so no mail is collected from elsewhere for this account."),
         GetCollection = () =>
         {
            dynamic account = OpenAccount(domainName, address);
            dynamic fetch = account.FetchAccounts;
            ServerSession.Release(account);
            return fetch;
         },
         Fields =
         {
            // First in the dialog as well as the grid: the account's master
            // switch, on a dialog of seventeen fields that scrolls, where an
            // Edit opened to switch the account off must find it in sight.
            new FieldSpec { Prop = "Enabled", Label = L("Enabled"), Kind = FieldKind.Bool, GridWidth = 70, Default = true },
            // The grid is the server's: the one column that says which mailbox a
            // row collects from, and the one star column, had about 110 pixels of
            // the dialog's 578 beside four fixed ones and read "pop.oldprovide…"
            // (review finding F199). The name is narrower, the port as wide as
            // its five digits, and the connection security, whose words are
            // sentences, is in the dialog only, beside the port it goes with:
            // the server has some 280 pixels.
            new FieldSpec { Prop = "Name", Label = L("Name"), GridWidth = 130, Default = "" },
            // Off the grid: its wording is a sentence about collection and
            // deletion, and five fixed columns left the server's name, the one
            // star column, about 40 pixels ("Ser…") in the account dialog.
            new FieldSpec { Prop = "ServerType", Label = L("Server type"), Kind = FieldKind.Combo, Options = ServerTypeOptions, ShowInGrid = false, Default = 0 },
            new FieldSpec { Prop = "ServerAddress", Label = L("Server"), Default = "" },
            new FieldSpec { Prop = "Port", Label = L("Port"), Kind = FieldKind.Number, GridWidth = 60, Default = 110 },
            new FieldSpec { Prop = "Username", Label = L("User name"), ShowInGrid = false, Default = "" },
            new FieldSpec { Prop = "Password", Label = L("Password"), Kind = FieldKind.Password, ShowInGrid = false, Default = "" },
            new FieldSpec { Prop = "MinutesBetweenFetch", Label = L("Minutes between downloads"), Kind = FieldKind.Number, ShowInGrid = false, Default = 15 },
            // The server's one meaning, for POP3 and IMAP alike (FetchAccount.h): this
            // said "0 = delete after download" from 6.2.0 to 6.3.5, while the POP3
            // fetcher read 0 as "never delete".
            new FieldSpec { Prop = "DaysToKeepMessages", Label = L("Days to keep on server (0 = never delete, -1 = delete after download)"), Kind = FieldKind.Number, ShowInGrid = false, Default = 0 },
            // Was a "Use SSL/TLS" checkbox, which could not express STARTTLS at
            // all. The COM property behind that checkbox is UseSSL, and its
            // getter is ConnectionSecurity == CSSSL while its setter writes
            // CSSSL or CSNone - so an account already set to STARTTLS read back
            // as unticked, and ticking anything on that dialog rewrote it to
            // implicit TLS on the next save. Implicit TLS on port 110 does not
            // connect, which made STARTTLS effectively unreachable from the GUI
            // for the very providers that require it.
            //
            // The server has supported all four values on the fetch path
            // throughout: POP3ClientConnection sends CAPA, issues STLS, and on
            // CSSTARTTLSRequired abandons the download with a log line when the
            // far end does not advertise STLS.
            new FieldSpec
            {
               Prop = "ConnectionSecurity",
               Label = L("Connection security"),
               Kind = FieldKind.Combo,
               Options = ConnectionSecurityOptions,
               ShowInGrid = false,
               Default = 0
            },
            new FieldSpec { Prop = "UseAntiSpam", Label = L("Run anti-spam on downloaded mail"), Kind = FieldKind.Bool, ShowInGrid = false, Default = true },
            new FieldSpec { Prop = "UseAntiVirus", Label = L("Run anti-virus on downloaded mail"), Kind = FieldKind.Bool, ShowInGrid = false, Default = true },
            // A mailbox that collects for several people - one catch-all at the
            // provider for a whole domain: each message is delivered to the
            // accounts its headers and its Received lines name, and only to those
            // that are accounts here (or, with the route option, a route's too),
            // and to this account when it names none (ExternalFetchClientBase).
            new FieldSpec { Prop = "ProcessMIMERecipients", Label = L("Deliver each message to the accounts its headers name"), Hint = L("For a mailbox shared by several people."), Kind = FieldKind.Bool, ShowInGrid = false, Default = false },
            new FieldSpec { Prop = "MIMERecipientHeaders", Label = L("Headers those addresses are read from"), Hint = L("Used by the option above - for example To, CC, X-RCPT-TO or X-Envelope-To."), ShowInGrid = false, Default = "" },
            new FieldSpec { Prop = "EnableRouteRecipients", Label = L("Deliver to addresses a route covers as well as to this server's accounts"), Kind = FieldKind.Bool, ShowInGrid = false, Default = false },
            new FieldSpec { Prop = "ProcessMIMEDate", Label = L("Date each message by its Received header rather than by when it was downloaded"), Kind = FieldKind.Bool, ShowInGrid = false, Default = false },
            // IMAP accounts only; the server ignores it for POP3. A migration rather than a
            // collection: every folder, verbatim, flags and dates kept, nothing delivered.
            new FieldSpec { Prop = "MirrorFolders", Label = L("Mirror every folder (IMAP only)"), Hint = L("A migration rather than a collection: every folder, verbatim, with its flags and dates."), Kind = FieldKind.Bool, ShowInGrid = false, Default = false }
         }
      }, embedded: true, onDialogTab: true);

      /// <summary>Account-level rules (enable/disable and delete; criteria/actions are not edited here).</summary>
      public static CollectionEditorView AccountRules(string domainName, string address) => new(new CollectionSpec
      {
         Title = L("Rules"),
         Subtitle = L("Per-account rules. You can add, rename, enable/disable and delete rules here; matching criteria and actions are managed on the server."),
         SavesAsMadeInDialog = true,
         ItemNoun = L("rule"),
         AddTitle = N("Add a rule"),
         EditTitle = N("Edit the rule"),
         CountOne = N("1 rule"),
         CountMany = N("{0} rules"),
         DeletePrompt = N("Delete the rule {0}?"),
         DeleteNameProps = new[] { "Name" }, // no-loc: the properties that name an entry
         EmptyText = L("This account has no rules of its own; the server's rules still apply."),
         GetCollection = () =>
         {
            dynamic account = OpenAccount(domainName, address);
            dynamic rules = account.Rules;
            ServerSession.Release(account);
            return rules;
         },
         // Add rule opens on the name, the Active box after it.
         TickBoxesLast = true,
         Fields =
         {
            new FieldSpec { Prop = "Active", Label = L("Active"), Kind = FieldKind.Bool, GridWidth = 70, Default = true },
            new FieldSpec { Prop = "Name", Label = L("Rule name"), Default = "" }
         }
      }, embedded: true, onDialogTab: true);
   }

   /// <summary>
   /// The Groups page: the generic collection editor for the list itself, plus the
   /// one thing the generic editor cannot offer - a way into the selected group's
   /// member list (<see cref="GroupMembersDialog"/>).
   ///
   /// Members… is the list's first verb, beside Edit, Delete and Add, enabled
   /// while a group is selected, and Enter and a double-click on a group open it;
   /// F2 is Edit, the group's rename (review finding F201). It was a lone button
   /// under the list, 760 pixels down at 1280 by 800, with a hint beside it that
   /// changed as a group was chosen without being announced, and a double-click
   /// on a group - the page's reason to exist being its members - renamed it. What
   /// the hint said is the empty state's and the subtitle's.
   /// </summary>
   public sealed class GroupsPageView : UserControl, IPageLifecycle
   {
      private readonly CollectionEditorView editor_;
      private readonly Wpf.Ui.Controls.Button membersButton_;

      public GroupsPageView(CollectionSpec spec)
      {
         // The editor in its page mode, as every other list of the kind: its
         // header with the title, the count and the page's actions, its search
         // (review finding F112: the page had none, the domains page beside it
         // did) and the list, with Members… first among its verbs. The page drew
         // its own header around the editor's embedded form, whose toolbar had no
         // search.
         editor_ = new CollectionEditorView(spec);

         membersButton_ = new Wpf.Ui.Controls.Button
         {
            Content = L("_Members…"),
            Appearance = Wpf.Ui.Controls.ControlAppearance.Secondary,
            MinWidth = 110,
            IsEnabled = false
         };
         ControlHelp.Explain(membersButton_, L("Edit the members of the selected group"));
         System.Windows.Automation.AutomationProperties.SetAutomationId(membersButton_, "groups-members");
         membersButton_.Click += (_, _) => OpenMembers();
         editor_.AddRowVerb(membersButton_, opensRow: true);

         Content = editor_;
      }

      private void OpenMembers()
      {
         (int id, string name) = editor_.SelectedEntry();
         if (id <= 0)
            return;

         var dialog = new GroupMembersDialog(Window.GetWindow(this), id, name ?? "");
         dialog.ShowDialog();
      }

      public void OnEnter() => editor_.OnEnter();
      public void OnLeave() => editor_.OnLeave();
   }
}
