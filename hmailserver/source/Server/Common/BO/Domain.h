// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

namespace HM
{

   class DistributionLists;
   class Aliases;
   class Accounts;
   class DomainAliases;

   class Domain : public BusinessObject<Domain>
   {
   public:
      Domain();
      virtual ~Domain();

      enum AntiSpamOptions
      {
         ASUseGreylisting = 1,
         
         ASDKIMSign = 2,
         ASDKIMSimpleHeader = 4,
         ASDKIMSimpleBody = 8,
         ASDKIMSHA1 = 16,
         ASDKIMSignAliases = 32
      };

      enum DomainSignatureMethod
      {
         SMUnknown = 0,
         SMSetIfNotSpecifiedInAccount = 1,
         SMOverwriteAccountSignature = 2,
         SMAppendToAccountSignature = 3
      };
      
      enum DomainLimitations
      {
         MaxNoOfAccounts = 1,
         MaxNoOfAliases = 2,
         MaxNoOfDistributionLists = 4
      };

      String GetName() const { return name_; } 
      void SetName(const String & newVal) { name_ = newVal; };

      String GetPostmaster() const { return postmaster_; } 
      void SetPostmaster(const String & newVal) { postmaster_ = newVal; };

      String GetADDomainName() const { return addomain_name_; } 
      void SetADDomainName(const String & newVal) { addomain_name_ = newVal; };

      bool GetIsActive() const { return active_ ? true : false; } 
      void SetIsActive(bool newVal) { active_ = newVal ? 1 : 0; }
      
      int GetMaxMessageSize() const {return max_message_size_;}
      void SetMaxMessageSize(int iNewVal) {max_message_size_ = iNewVal; }
      
      bool GetUsePlusAddressing() const {return use_plus_addressing_; }
      void SetUsePlusAddressing(bool bNewVal) {use_plus_addressing_ = bNewVal; }

      String GetPlusAddressingChar() const {return plus_addressing_char_; }
      void SetPlusAddressingChar(const String &sNewVal) {plus_addressing_char_ = sNewVal; }

      int GetAntiSpamOptions() const {return anti_spam_options_;}
      void SetAntiSpamOptions(int iNewVal) {anti_spam_options_ = iNewVal; }

      bool GetASUseGreyListing() const;
      void SetASUseGreyListing(bool bNewVal);

      int GetMaxSizeMB() const;
      void SetMaxSizeMB(int iNewVal);

      // Days delivered messages are kept in this domain's mailboxes before the
      // retention sweep removes them; 0 = no policy. An account's own value overrides
      // it (see Account::GetMessageRetentionDays).
      int GetMessageRetentionDays() const { return message_retention_days_; }
      void SetMessageRetentionDays(int days) { message_retention_days_ = days; }

      // The quarantine digest: one message to each account of this domain that
      // has something newly held for it, listing what was held and carrying a
      // link that releases it. Off by default and per domain, because the digest
      // is the first mail this server sends to its own users unprompted, and an
      // upgrade must not start doing that on its own.
      bool GetQuarantineDigest() const { return quarantine_digest_ ? true : false; }
      void SetQuarantineDigest(bool newVal) { quarantine_digest_ = newVal ? 1 : 0; }

      // The hour of the local day the digest goes out, 0 to 23. The sends are
      // spread across that hour rather than fired at the top of it; see
      // QuarantineDigestTask.
      int GetQuarantineDigestHour() const { return quarantine_digest_hour_; }
      void SetQuarantineDigestHour(int newVal) { quarantine_digest_hour_ = newVal; }

      // Outbound webhooks, per domain (see Util/WebhookManager.h). The first is an
      // off switch a domain administrator holds: with it off, nothing this domain's
      // mail or accounts do raises an event, whatever any subscription asks for. On
      // by default, because a subscription that names no domain is already the
      // administrator saying "every domain", and an installation with no
      // subscription raises nothing either way.
      //
      // The second is the one content decision in the feature: a webhook body never
      // carries a message body, and it carries the SUBJECT only when this is on AND
      // the subscription asks for it. OFF by default, on every existing domain and
      // every new one, because a subject line is what the message is about and a
      // stream of them to a third-party endpoint is a disclosure nobody asked for.
      bool GetWebhooksEnabled() const { return webhooks_enabled_; }
      void SetWebhooksEnabled(bool newVal) { webhooks_enabled_ = newVal; }

      bool GetWebhookSubjects() const { return webhook_subjects_; }
      void SetWebhookSubjects(bool newVal) { webhook_subjects_ = newVal; }

      // The country policy applied when an account of this domain signs in:
      // empty (nothing is judged), "allow:GB,IE" or "block:CN,RU". The grammar
      // and the decision are CountryBlocking's; this is only where it is kept.
      // A domain is not known when a connection is accepted, which is why this
      // is a policy about SIGNING IN and the server-wide ones are about
      // connecting.
      String GetCountryPolicy() const { return country_policy_; }
      void SetCountryPolicy(const String &newVal) { country_policy_ = newVal; }

      // The session policies for the accounts of this domain (schema 6050). The
      // two numbers are seconds, and 0 means "take the server's answer"; the two
      // switches are 0 to take the server's answer, 1 for on and 2 for off, because
      // a domain must be able to say "not here" about a server-wide policy as well
      // as "here too". Resolved in one place: SessionPolicies.
      int GetSessionIdleSeconds() const { return session_idle_seconds_; }
      void SetSessionIdleSeconds(int newVal) { session_idle_seconds_ = newVal; }

      int GetSessionLifetimeSeconds() const { return session_lifetime_seconds_; }
      void SetSessionLifetimeSeconds(int newVal) { session_lifetime_seconds_ = newVal; }

      int GetSessionOnePerDevice() const { return session_one_per_device_; }
      void SetSessionOnePerDevice(int newVal) { session_one_per_device_ = newVal; }

      int GetSessionBindToAddress() const { return session_bind_to_address_; }
      void SetSessionBindToAddress(int newVal) { session_bind_to_address_ = newVal; }

      bool GetEnableSignature() const;
      void SetEnableSignature(bool bNewVal);

      DomainSignatureMethod GetSignatureMethod() const;
      void SetSignatureMethod(DomainSignatureMethod sm);

      String GetSignaturePlainText() const;
      void SetSignaturePlainText(const String &sNewVal);

      String GetSignatureHTML() const;
      void SetSignatureHTML(const String &sNewVal);

      bool GetAddSignaturesToReplies() const;
      void SetAddSignaturesToReplies(bool bNewVal);

      bool GetAddSignaturesToLocalMail() const;
      void SetAddSignaturesToLocalMail(bool bNewVal);

      int GetMaxNoOfAccounts() const;
      void SetMaxNoOfAccounts(int iNewVal);
      int GetMaxNoOfAliases() const;
      void SetMaxNoOfAliases(int iNewVal);
      int GetMaxNoOfDistributionLists() const;
      void SetMaxNoOfDistributionLists(int iNewVal);

      int GetLimitationsEnabled() const;
      void SetLimitationsEnabled(int iNewVal);

      bool GetMaxNoOfAccountsEnabled() const;
      void SetMaxNoOfAccountsEnabled(bool iNewVal);
      bool GetMaxNoOfAliasesEnabled() const;
      void SetMaxNoOfAliasesEnabled(bool iNewVal);
      bool GetMaxNoOfDistributionListsEnabled() const;
      void SetMaxNoOfDistributionListsEnabled(bool iNewVal);

      int GetMaxAccountSize() const;
      void SetMaxAccountSize(int iNewVal);
      
      /*
         DKIM START

      */
      bool GetDKIMEnabled() const;
      void SetDKIMEnabled(bool newVal);

      bool GetDKIMAliasesEnabled() const;
      void SetDKIMAliasesEnabled(bool newVal);

      AnsiString GetDKIMSelector() const;
      void SetDKIMSelector(const String &newValue);

      String GetDKIMPrivateKeyFile() const;
      void SetDKIMPrivateKeyFile(const String &newValue);

      // The secondary selector/key pair stages a key rotation. It never signs
      // anything: it is configuration parked next to the primary so that the
      // new public key can be published in DNS and confirmed to resolve while
      // the primary key continues to sign every message. Without it, rotating
      // a key means editing the one selector in place, and every message
      // signed between that edit and DNS propagation fails verification.
      AnsiString GetDKIMSecondarySelector() const;
      void SetDKIMSecondarySelector(const String &newValue);

      String GetDKIMSecondaryPrivateKeyFile() const;
      void SetDKIMSecondaryPrivateKeyFile(const String &newValue);

      // The cutover: the secondary becomes the primary and the secondary slot
      // is cleared. Fails, with a reason in errorMessage, rather than promote
      // a half-configured or unusable pair - a bad promote would stop DKIM
      // signing for the domain, which is the outage this scheme exists to
      // prevent. The caller is responsible for persisting the domain afterwards.
      bool PromoteDKIMSecondary(String &errorMessage);

      // The rotation policy: whether the server rotates this domain's key on a
      // schedule, how often, how long a new key's record must have been seen in
      // DNS before signing moves to it, how long the old selector is kept for
      // the verifiers that arrive late, and the size of the keys it generates.
      // Off on every domain until somebody switches it on; the rotation itself -
      // the next and retiring selectors and every time and DNS check - is kept
      // in hm_dkimrotations by DkimRotation, which is where to read about it.
      // Validated by DkimRotationPlan::Policy::Validate at the interfaces.
      bool GetDKIMRotationEnabled() const { return dkim_rotation_enabled_; }
      void SetDKIMRotationEnabled(bool newValue) { dkim_rotation_enabled_ = newValue; }

      int GetDKIMRotationIntervalDays() const { return dkim_rotation_interval_days_; }
      void SetDKIMRotationIntervalDays(int newValue) { dkim_rotation_interval_days_ = newValue; }

      int GetDKIMRotationPropagationHours() const { return dkim_rotation_propagation_hours_; }
      void SetDKIMRotationPropagationHours(int newValue) { dkim_rotation_propagation_hours_ = newValue; }

      int GetDKIMRotationRetentionDays() const { return dkim_rotation_retention_days_; }
      void SetDKIMRotationRetentionDays(int newValue) { dkim_rotation_retention_days_ = newValue; }

      int GetDKIMRotationKeyBits() const { return dkim_rotation_key_bits_; }
      void SetDKIMRotationKeyBits(int newValue) { dkim_rotation_key_bits_ = newValue; }

      // Whether this domain's accounts are published in the Web Key Directory
      // (docs/OpenPGP.md). Off on every domain until somebody switches it on,
      // and with it off no key of this domain is served whatever the server
      // setting and whatever each account says. The second switch is what a
      // NEW account in this domain starts with: a domain that wants everybody
      // published sets it, and a domain where publishing is a personal choice
      // leaves it off. Neither ever changes an account that already exists.
      bool GetOpenPGPPublish() const { return openpgp_publish_; }
      void SetOpenPGPPublish(bool newValue) { openpgp_publish_ = newValue; }

      bool GetOpenPGPPublishNewAccounts() const { return openpgp_publish_new_accounts_; }
      void SetOpenPGPPublishNewAccounts(bool newValue) { openpgp_publish_new_accounts_ = newValue; }

      // An outbound relay chosen by the SENDING domain, which is a different
      // question from the one routes answer. A route says "mail addressed to this
      // domain goes to that host"; this says "mail FROM this domain leaves through
      // that host", which is what a server hosting several independent domains
      // needs when each of them has bought its own delivery provider. Empty host
      // means the domain has no opinion and the global relayer applies, so an
      // upgraded installation behaves exactly as before until somebody fills one in.
      String GetRelayHost() const { return relay_host_; }
      void SetRelayHost(const String &newValue) { relay_host_ = newValue; }

      // 0 means "unset", and the resolver reads it as 25 - the same rule the global
      // relayer already uses, so the two cannot disagree about what a blank port means.
      long GetRelayPort() const { return relay_port_; }
      void SetRelayPort(long newValue) { relay_port_ = newValue; }

      bool GetRelayRequiresAuth() const { return relay_requires_auth_; }
      void SetRelayRequiresAuth(bool newValue) { relay_requires_auth_ = newValue; }

      String GetRelayUsername() const { return relay_username_; }
      void SetRelayUsername(const String &newValue) { relay_username_ = newValue; }

      String GetRelayPassword() const { return relay_password_; }
      void SetRelayPassword(const String &newValue) { relay_password_ = newValue; }

      ConnectionSecurity GetRelayConnectionSecurity() const { return relay_connection_security_; }
      void SetRelayConnectionSecurity(ConnectionSecurity newValue) { relay_connection_security_ = newValue; }

      // The domain-wide out-of-office reply. It answers mail to accounts that have
      // no active vacation message of their own (a closed office, a decommissioned
      // department), and it can say different things to different senders:
      //
      //   * GetVacationSubject/GetVacationMessage is the EXTERNAL text - what a
      //     sender from outside the server's own domains receives. It is also the
      //     fallback for internal senders when no internal text is configured, so
      //     the safe, generic wording belongs here.
      //   * GetVacationInternalSubject/GetVacationInternalMessage is the optional
      //     INTERNAL text - what a sender who resolves to an account or alias
      //     hosted on this server receives. Empty means "internal senders get the
      //     external text too".
      //
      // "Internal" is decided by SMTPVacationMessageCreator::IsInternalSender, and
      // its guarantee is deliberately weak: the envelope sender NAMES a local
      // mailbox, not that the message came from that mailbox's owner. The envelope
      // is forgeable wherever the installation's IP ranges accept a local MAIL FROM
      // without authentication, so the internal text must never carry anything that
      // cannot be shown to a stranger.
      //
      // Precedence against an account's own vacation message: the account wins, the
      // domain answers only for accounts with nothing of their own, and exactly one
      // reply is ever produced per delivered message. Two replies would double the
      // loop pressure and can contradict each other ("back on Monday" beside "this
      // office has closed").
      bool GetVacationMessageIsOn() const { return vacation_message_on_; }
      void SetVacationMessageIsOn(bool bNewVal);

      String GetVacationSubject() const { return vacation_subject_; }
      void SetVacationSubject(const String &sNewVal) { vacation_subject_ = sNewVal; }

      String GetVacationMessage() const { return vacation_message_; }
      void SetVacationMessage(const String &sNewVal) { vacation_message_ = sNewVal; }

      String GetVacationInternalSubject() const { return vacation_internal_subject_; }
      void SetVacationInternalSubject(const String &sNewVal) { vacation_internal_subject_ = sNewVal; }

      String GetVacationInternalMessage() const { return vacation_internal_message_; }
      void SetVacationInternalMessage(const String &sNewVal) { vacation_internal_message_ = sNewVal; }

      // The organisation-wide privacy policy for accounts that DO have their own
      // vacation message: when this is on and the domain's external text is
      // configured, an external sender receives the domain's external text in place
      // of the account's personal one, while internal senders still receive what
      // the account wrote. Off (the default) preserves the pre-existing behaviour -
      // the account's single message goes to everyone - so an upgraded installation
      // changes nothing until an administrator decides otherwise.
      bool GetVacationExternalOverride() const { return vacation_external_override_; }

      // The window on the domain reply: days after which a sender who has been
      // answered is answered again while the reply stays on; 0 answers each
      // sender once per absence.
      int GetVacationDays() const { return vacation_days_; }
      void SetVacationDays(int iNewVal) { vacation_days_ = iNewVal; }

      // The quota policy for this domain's mailboxes. -1 on a number means the
      // server's own value applies; an empty text means the server message
      // does. The warning threshold is a share of the mailbox's limit (0 warns
      // nobody), the grace is how far past the limit incoming mail is still
      // accepted, and the send-stop is the share at which the account can no
      // longer send - before it can no longer receive, when it is under 100.
      int GetQuotaWarningPercent() const { return quota_warning_percent_; }
      void SetQuotaWarningPercent(int iNewVal) { quota_warning_percent_ = iNewVal; }
      String GetQuotaWarningSubject() const { return quota_warning_subject_; }
      void SetQuotaWarningSubject(const String &sNewVal) { quota_warning_subject_ = sNewVal; }
      String GetQuotaWarningText() const { return quota_warning_text_; }
      void SetQuotaWarningText(const String &sNewVal) { quota_warning_text_ = sNewVal; }
      int GetQuotaGracePercent() const { return quota_grace_percent_; }
      void SetQuotaGracePercent(int iNewVal) { quota_grace_percent_ = iNewVal; }
      int GetQuotaSendStopPercent() const { return quota_send_stop_percent_; }
      void SetQuotaSendStopPercent(int iNewVal) { quota_send_stop_percent_ = iNewVal; }
      void SetVacationExternalOverride(bool bNewVal) { vacation_external_override_ = bNewVal; }

      /*
         WHAT THIS DOMAIN DOES TO A MESSAGE

         Three transformations, all off by default, all applied to this domain's
         own copies rather than to the queued message: see
         hmailserver/docs/DomainTransforms.md.
      */

      // Put the tag text at the front of the Subject of a message this domain
      // receives from outside. Breaks any DKIM signature the sender made over
      // their Subject, exactly as the anti-spam subject prefix does and for the
      // same reason - which is why it is separable from the header below, and
      // why the header is the one to prefer where the reader can show it.
      bool GetExternalTagSubject() const { return external_tag_subject_; }
      void SetExternalTagSubject(bool bNewVal) { external_tag_subject_ = bNewVal; }

      // Stamp X-hMailServer-External on a message this domain receives from
      // outside. Prepending a field is signature-safe - DKIM selects duplicate
      // fields from the bottom up - so this costs the sender's signature
      // nothing, and the webmail turns it into a banner.
      bool GetExternalTagHeader() const { return external_tag_header_; }
      void SetExternalTagHeader(bool bNewVal) { external_tag_header_ = bNewVal; }

      // The text put at the front of the subject. Empty means the shipped
      // [EXTERNAL]. Per domain so that a domain whose readers do not read
      // English can say it in the language they do.
      String GetExternalTagText() const { return external_tag_text_; }
      void SetExternalTagText(const String &sNewVal) { external_tag_text_ = sNewVal; }

      // Tell the reader when a sender has not written to them before. Carries a
      // per-message cost in the database - see PersistentKnownSender.
      bool GetFirstContactTip() const { return first_contact_tip_; }
      void SetFirstContactTip(bool bNewVal) { first_contact_tip_ = bNewVal; }

      // Append the legal footer below to mail this domain sends outside the
      // organisation. Never to a signed or encrypted message: see
      // DisclaimerAdder.
      bool GetDisclaimerEnabled() const { return disclaimer_enabled_; }
      void SetDisclaimerEnabled(bool bNewVal) { disclaimer_enabled_ = bNewVal; }

      String GetDisclaimerPlainText() const { return disclaimer_plain_text_; }
      void SetDisclaimerPlainText(const String &sNewVal) { disclaimer_plain_text_ = sNewVal; }

      // Empty means the plain text is used, its line breaks turned into <br>,
      // which is what the signature does and what an administrator who fills in
      // one box expects.
      String GetDisclaimerHTML() const { return disclaimer_html_; }
      void SetDisclaimerHTML(const String &sNewVal) { disclaimer_html_ = sNewVal; }

      int GetDKIMHeaderCanonicalizationMethod() const;
      void SetDKIMHeaderCanonicalizationMethod(int newValue);

      int GetDKIMBodyCanonicalizationMethod() const;
      void SetDKIMBodyCanonicalizationMethod(int newValue);

      int GetDKIMSigningAlgorithm() const;
      void SetDKIMSigningAlgorithm(int newValue);
      /*
         DKIM END
      */

      std::shared_ptr<Accounts> GetAccounts();
      std::shared_ptr<Accounts> GetAccounts(__int64 iAccountID);
      std::shared_ptr<Aliases> GetAliases();
      std::shared_ptr<DistributionLists> GetDistributionLists();
      std::shared_ptr<DomainAliases> GetDomainAliases();

      bool XMLStore(XNode *pParentNode, int iBackupOptions);
      bool XMLLoad(XNode *pDomainNode, int iRestoreOptions);
      bool XMLLoadSubItems(XNode *pDomainNode, int iRestoreOptions);

      size_t GetEstimatedCachingSize();
   protected:

      int anti_spam_options_;
      
      String name_;
      String postmaster_;
      String addomain_name_;

      bool use_plus_addressing_;
      String plus_addressing_char_;
      int session_idle_seconds_;
      int session_lifetime_seconds_;
      int session_one_per_device_;
      int session_bind_to_address_;
      String country_policy_;

      bool enable_signature_;
      DomainSignatureMethod signature_method_;
      String signature_plain_text_;
      String signature_html_;
      bool add_signatures_to_local_mail_;
      bool add_signatures_to_replies_;
      int active_;
      unsigned int max_message_size_;
      int max_size_mb_;
      int message_retention_days_;
      int quarantine_digest_;
      int quarantine_digest_hour_;

      bool webhooks_enabled_;
      bool webhook_subjects_;
      int max_no_of_accounts_;
      int max_no_of_aliases_;
      int max_no_of_distribution_lists_;
      int limitations_enabled_;
      int max_account_size_;

      String dkim_selector_;
      String dkim_private_key_file_;

      String dkim_secondary_selector_;
      String dkim_secondary_private_key_file_;

      bool dkim_rotation_enabled_;
      int dkim_rotation_interval_days_;
      int dkim_rotation_propagation_hours_;
      int dkim_rotation_retention_days_;
      int dkim_rotation_key_bits_;

      bool openpgp_publish_;
      bool openpgp_publish_new_accounts_;

      String relay_host_;
      long relay_port_;
      bool relay_requires_auth_;
      String relay_username_;
      String relay_password_;
      ConnectionSecurity relay_connection_security_;

      bool vacation_message_on_;
      String vacation_subject_;
      String vacation_message_;
      String vacation_internal_subject_;
      String vacation_internal_message_;
      bool vacation_external_override_;
      int vacation_days_;
      int quota_warning_percent_;
      String quota_warning_subject_;
      String quota_warning_text_;
      int quota_grace_percent_;
      int quota_send_stop_percent_;

      bool external_tag_subject_;
      bool external_tag_header_;
      String external_tag_text_;
      bool first_contact_tip_;
      bool disclaimer_enabled_;
      String disclaimer_plain_text_;
      String disclaimer_html_;

      // Guards the lazy creation of the collections below. A Domain is shared by
      // every session of every account in it, and two sessions asking for a
      // collection that does not exist yet each built a private one - two Accounts
      // objects for one domain, one of which the other never saw (upstream #566).
      boost::recursive_mutex collections_mutex_;

      std::shared_ptr<Accounts> accounts_;
      std::shared_ptr<Aliases> aliases_;
      std::shared_ptr<DistributionLists> distribution_lists_;
      std::shared_ptr<DomainAliases> domain_aliases_;
   };
}