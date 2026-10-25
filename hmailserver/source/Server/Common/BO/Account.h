// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2005 Martin Knafve / hMailServer.com.

#pragma once

namespace HM
{

   class Rules;
   class IMAPFolders;
   class Messages;

   class Account  : public BusinessObject<Account>
   {
   public:
      enum AdminLevel
      {
         NormalUser = 0,
         DomainAdmin = 1,
         ServerAdmin = 2
      };

      // What the account is (schema 6101, Util/ResourceBooking.h,
      // docs/RoomsAndEquipment.md): a person, as every account was before, or
      // a resource - a meeting room or a piece of equipment - which answers
      // the invitations it is sent by its booking policy, is in the directory
      // as what it is, and cannot be signed into: it is administered, and its
      // delegates answer for it from its calendar.
      enum AccountKind
      {
         KindPerson = 0,
         KindRoom = 1,
         KindEquipment = 2
      };

      Account();
      Account(const String &address, AdminLevel adminLevel);
      Account(const Account &oldAccount);

      virtual ~Account();
      


      void Initialize();

      String GetName() const {return address_; }

      void SetDomainID(__int64 newVal) { domain_id_ = newVal; };
      __int64 GetDomainID() const { return domain_id_; }

      void SetAddress(const String & newVal) { address_ = newVal; };
      String GetAddress() const { return address_; }

      void SetPassword(const String & newVal);
      String GetPassword() const { return password_; }

      void SetADDomain(const String & newVal) { addomain_ = newVal; };
      String GetADDomain() const { return addomain_; }

      void SetADUsername(const String & newVal) { adusername_ = newVal; };
      String GetADUsername() const { return adusername_; }

      void SetActive(bool newVal) { active_ = newVal; };
      bool GetActive() const { return active_; }

      void SetIsAD(bool newVal) { is_ad_ = newVal; };
      bool GetIsAD() const { return is_ad_; }

      void SetAccountMaxSize(long newVal) {account_max_size_ = newVal; }
      long GetAccountMaxSize() const {return account_max_size_;}

      // Days delivered messages are kept before the retention sweep removes them:
      // 0 = the domain's policy, -1 = keep forever, else the number of days. See
      // MailboxRetentionTask.
      int GetMessageRetentionDays() const { return message_retention_days_; }
      void SetMessageRetentionDays(int days) { message_retention_days_ = days; }

      bool GetVacationMessageIsOn() const; 
      void SetVacationMessageIsOn(bool bNewVal);

      String GetVacationMessage() const {return vacation_message_; }
      void SetVacationMessage(const String &sNewVal) {vacation_message_ = sNewVal;}

      String GetVacationSubject() const{return vacation_subject_; }
      void SetVacationSubject(const String &sNewVal) {vacation_subject_ = sNewVal;}

      bool GetVacationExpires() const {return vacation_expires_; }
      void SetVacationExpires(bool bNewVal) {vacation_expires_ = bNewVal ; }

      String GetVacationExpiresDate() const{return vacation_expires_date_; }
      void SetVacationExpiresDate(const String &sNewVal) {vacation_expires_date_ = sNewVal;}

      // The other end of the window: no auto-reply before this date, so an
      // absence can be set up in advance. Reaching the date activates the reply
      // without touching the stored on-flag, so nothing has to run at midnight.
      String GetVacationBeginDate() const{return vacation_begin_date_; }
      void SetVacationBeginDate(const String &sNewVal) {vacation_begin_date_ = sNewVal;}

      bool GetVacationAbortSpamFlagged() const { return vacation_abort_spam_flagged_; }

      // The out-of-office policy on the account. The window: days after which a
      // sender who has already been answered is answered again while the reply
      // stays on; 0 answers each sender once per absence, as every account did
      // before. The internal text: what a sender whose address belongs to this
      // installation gets in place of the general text, when it is not empty.
      // Internal-only: the reply answers those senders and nobody else - the
      // domain's external override, when on, still answers strangers with the
      // domain's text. See SMTPVacationMessageCreator and LocalDelivery.
      int GetVacationDays() const { return vacation_days_; }
      void SetVacationDays(int iNewVal) { vacation_days_ = iNewVal; }

      String GetVacationInternalSubject() const { return vacation_internal_subject_; }
      void SetVacationInternalSubject(const String &sNewVal) { vacation_internal_subject_ = sNewVal; }

      String GetVacationInternalMessage() const { return vacation_internal_message_; }
      void SetVacationInternalMessage(const String &sNewVal) { vacation_internal_message_ = sNewVal; }

      bool GetVacationInternalOnly() const { return vacation_internal_only_; }
      void SetVacationInternalOnly(bool bNewVal) { vacation_internal_only_ = bNewVal; }
      void SetVacationAbortSpamFlagged(bool bNewVal) { vacation_abort_spam_flagged_ = bNewVal; }


      AdminLevel GetAdminLevel() const{return admin_level_;}
      void SetAdminLevel(AdminLevel iNewVal) {admin_level_ = iNewVal; }

      // The admin level an archive carries for an account node - what XMLStore
      // wrote as AdminLevel. XMLLoad does not always take it (see there), so the
      // restore reads it from the node again once the account's role is back.
      static AdminLevel ArchivedAdminLevel(XNode *accountNode);

      void SetPasswordEncryption(int iNewVal) {password_encryption_ = iNewVal; }

      // The account's TOTP secret, base32. Empty means no second factor, which is
      // every account until an administrator enrols one - so this single value is
      // the feature's on/off switch, and its emptiness is load-bearing rather than
      // incidental.
      // When the password was last SET, not last used. Stamped by the one place a
      // password is chosen, and by the 6019 upgrade for every account that already
      // existed - so enabling expiry starts everybody's clock from that upgrade
      // rather than expiring every mailbox at once.
      String GetPasswordChanged() const { return password_changed_; }
      void SetPasswordChanged(const String &newVal) { password_changed_ = newVal; }

      String GetTotpSecret() const { return totp_secret_; }
      void SetTotpSecret(const String &newVal) { totp_secret_ = newVal; }
      long GetPasswordEncryption() const {return password_encryption_; }

      std::shared_ptr<Messages> GetMessages();
      std::shared_ptr<Rules> GetRules(); 
      std::shared_ptr<IMAPFolders> GetFolders(); 

      bool SpaceAvailable(__int64 iBytes) const;

      // The same, with the limit stretched by a grace: a share of the limit
      // past which incoming mail is still accepted. 0 is the limit itself.
      bool SpaceAvailable(__int64 iBytes, int gracePercent) const;
      // Returns true if a message with iBytes bytes can fit inside the account

      bool XMLStore(XNode *pParentNode, int iBackupOptions);
      bool XMLLoad(XNode *pAccountNode, int iRestoreOptions);
      bool XMLLoadSubItems(XNode *pAccountNode, int iRestoreOptions);

      String GetForwardAddress() const {return forward_address_;}
      void SetForwardAddress(const String &sNewVal) { forward_address_ = sNewVal; }

      bool GetForwardEnabled() const;
      void SetForwardEnabled(bool bEnabled);
      
      bool GetForwardKeepOriginal() const;
      void SetForwardKeepOriginal(bool bEnabled);

      bool GetForwardAbortSpamFlagged() const { return forward_abort_spam_flagged_; }
      void SetForwardAbortSpamFlagged(bool bNewVal) { forward_abort_spam_flagged_ = bNewVal; }

      // Per-account spam-filtering overrides. All three act at LOCAL DELIVERY, on
      // this account's own copy of a message - never on the SMTP conversation,
      // which is shared by every recipient and stays governed by the global
      // thresholds. The honest consequence: an account cannot opt out of a
      // conversation-time refusal or greylisting delay, and the overrides can only
      // re-judge a message the global filter actually classified as spam, because
      // that classification (the spam flag, and the recorded X-hMailServer-
      // Reason-Score header) is all that survives from the conversation to
      // delivery. A message below the global mark threshold reaches delivery
      // carrying no score at all, so there is nothing for a stricter per-account
      // threshold to measure it against.
      //
      // false = spam filtering is off for this account: a classified message is
      // delivered to this account unmarked (flag cleared, spam headers and subject
      // tag removed from its copy), and the per-account delete threshold below is
      // ignored. Other recipients of the same message are unaffected.
      bool GetAntiSpamEnabled() const { return anti_spam_enabled_; }
      void SetAntiSpamEnabled(bool newVal) { anti_spam_enabled_ = newVal; }

      // -1 = no override (the global behaviour stands). 0 = this account's copies
      // are never marked as spam. > 0 = re-judge a classified copy against this
      // value: it is unmarked when its recorded score is known and below the value.
      int GetSpamMarkThreshold() const { return spam_mark_threshold_; }
      void SetSpamMarkThreshold(int newVal) { spam_mark_threshold_ = newVal; }

      // -1 or 0 = never delete at delivery (the global delete threshold acts
      // during the SMTP conversation and needs no help here). > 0 = this account's
      // copy of a classified message is not delivered when its score provably
      // reached the value - the recorded score when present, otherwise the global
      // mark threshold as a lower bound. Quarantined instead of dropped when the
      // quarantine store is enabled.
      int GetSpamDeleteThreshold() const { return spam_delete_threshold_; }
      void SetSpamDeleteThreshold(int newVal) { spam_delete_threshold_ = newVal; }

      // Whether this account's OpenPGP public keys are published in the Web Key
      // Directory (docs/OpenPGP.md). Off on every account that already exists,
      // and a new account starts from its domain's OpenPGPPublishNewAccounts.
      // It is the person's own switch, changed over /api/v1/me/openpgp/publish
      // as well as by an administrator: holding a key so that a mail client can
      // find it is not the same decision as telling the whole internet about it.
      // The server setting and the domain's switch still have to be on for
      // anything to be served.
      bool GetOpenPGPPublish() const { return openpgp_publish_; }
      void SetOpenPGPPublish(bool newVal) { openpgp_publish_ = newVal; }

      // Whether this is a zero-access mailbox (docs/ZeroAccess.md, schema
      // 6083): every message delivered to it is encrypted, after the filters
      // have run, to the account's own OpenPGP key, and the key is held in a
      // form only the person's password or their recovery key opens. The
      // person switches it on from the webmail's Security page, once the
      // server-wide ZeroAccessAllowed setting permits it; an administrator
      // reads it (COM Account.ZeroAccess, REST zero_access) and cannot set it,
      // because a switch an administrator could throw is not a promise about
      // what an administrator cannot read. Only ZeroAccess::Enable and
      // ZeroAccess::Disable write it.
      bool GetZeroAccess() const { return zero_access_; }
      void SetZeroAccess(bool newVal) { zero_access_ = newVal; }

      // Mailbox encryption at rest (Util/MailboxKeys.h): the account's mailbox
      // key wrapped by a password that is being set on this object, made by
      // MailboxKeys::PrepareForNewPassword and stored by PersistentAccount::
      // SaveObject once the account is saved, so that the key is never wrapped
      // by a password the account does not have. Held on this object only:
      // never a column, never in a backup. A copy of the object carries it with
      // the password it goes with, so that saving the copy stores the two
      // together and never the one without the other.
      AnsiString GetMailboxKeyWrap() const { return mailbox_key_wrap_; }
      void SetMailboxKeyWrap(const AnsiString &newVal) { mailbox_key_wrap_ = newVal; }

      // Restricted delivery (schema 6084, Util/RestrictedDelivery.h): 0 follows
      // the domain's list of named domains, 1 is restricted even when the
      // domain names nothing (the account is then confined to its own
      // domain), 2 is exempt in a restricted domain. COM Account.
      // RestrictedDelivery, REST restricted_delivery.
      int GetRestrictedDelivery() const { return restricted_delivery_; }
      void SetRestrictedDelivery(int newVal) { restricted_delivery_ = newVal; }

      // The organisation's identity provider's own id for the person this
      // account is - SCIM's externalId (schema 6096, docs/Scim.md): Entra's
      // object id, Okta's user id. Up to 255 characters, compared exactly, and
      // empty on every account nothing provisioned. COM Account.ExternalId.
      String GetExternalId() const { return external_id_; }
      void SetExternalId(const String &newVal) { external_id_ = newVal; }

      // A person (KindPerson), a room or a piece of equipment. COM
      // Account.Kind, REST kind. A value outside the three is kept as a person.
      int GetKind() const { return kind_; }
      void SetKind(int newVal) { kind_ = (newVal == KindRoom || newVal == KindEquipment) ? newVal : KindPerson; }
      bool IsResource() const { return kind_ == KindRoom || kind_ == KindEquipment; }

      bool GetEnableSignature() const;
      void SetEnableSignature(bool bEnabled);

      String GetSignaturePlainText() const;
      void SetSignaturePlainText(const String &sNewVal);

      String GetSignatureHTML() const;
      void SetSignatureHTML(const String &sNewVal);

      String GetLastLogonTime() const;
      void SetLastLogonTime(const String &sNewVal);

      String GetPersonFirstName() const;
      void SetPersonFirstName(const String &sNewVal);

      String GetPersonLastName() const;
      void SetPersonLastName(const String &sNewVal);

      size_t GetEstimatedCachingSize();

   protected:
      __int64 domain_id_;
      
      unsigned int account_max_size_;
      // Maximum account size. MB
      int message_retention_days_;

      long password_encryption_;

      AnsiString address_;
      AnsiString password_;
      String totp_secret_;
      String password_changed_;
      String addomain_;
      String adusername_;

      String person_first_name_;
      String person_last_name_;
      String external_id_;

      String vacation_message_;
      String vacation_subject_;
      bool vacation_expires_;
      String vacation_expires_date_;
      String vacation_begin_date_;
      bool vacation_abort_spam_flagged_;
      int vacation_days_;
      String vacation_internal_subject_;
      String vacation_internal_message_;
      bool vacation_internal_only_;
      
      String signature_plain_text_;
      String signature_html_;

      AnsiString forward_address_;
      bool forward_enabled_;
      bool forward_keep_original_;
      bool forward_abort_spam_flagged_;
      bool anti_spam_enabled_;
      int spam_mark_threshold_;
      int spam_delete_threshold_;
      bool openpgp_publish_;
      bool zero_access_;
      int restricted_delivery_;
      int kind_;
      bool active_;
      bool is_ad_;
      bool vacation_message_is_on_;
      bool enable_signature_;

      std::shared_ptr<Messages> messages_;
      std::shared_ptr<Rules> rules_;
      std::shared_ptr<IMAPFolders> folders_;
      
      AdminLevel admin_level_;

      AnsiString last_logon_time_;

      AnsiString mailbox_key_wrap_;
   };

   // The copy constructor held to every member: an account with every field
   // set away from its default - a positive retention, then -1, and a mailbox
   // key wrap among them - copied into memory filled with a pattern first, so
   // that a member the copy does not set reads as the pattern rather than as
   // whatever the heap held, and every getter compared. No database, no
   // configuration. ClassTester runs it on Windows, business-object-tests
   // (platform/business_object_tests_main.cpp) on Linux. Throws
   // std::logic_error naming the first member a copy lost.
   class AccountTester
   {
   public:
      void Test();
   };

}
