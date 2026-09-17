// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"

#include "Domain.h"
#include "Rules.h"

#include "Accounts.h"
#include "Aliases.h"
#include "DistributionLists.h"
#include "DomainAliases.h"
#include "SenderListEntries.h"
#include "SpamTrap.h"
#include "SpamTraps.h"

#include "../../SMTP/SMTPVacationMessageCreator.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // One numeric attribute of a restored domain, kept only when it is a number
      // this server would have accepted at its interfaces. An attribute that is
      // absent - every archive written before the attribute existed - leaves the
      // field as the constructor set it.
      void ReadPolicyAttribute_(XNode *node, const wchar_t *name, int lowest, int highest, int &target)
      {
         const String text = node->GetAttrValue(name);
         if (text.IsEmpty())
            return;

         const int value = _ttoi(text);
         if (value >= lowest && value <= highest)
            target = value;
      }
   }

   Domain::Domain() :
      use_plus_addressing_(false),
      active_(0),
      max_message_size_(0),
      plus_addressing_char_("+"),
      anti_spam_options_(1),
      max_size_mb_(0),
      message_retention_days_(0),
      quarantine_digest_(0),
      quarantine_digest_hour_(7),
      session_idle_seconds_(0),
      session_lifetime_seconds_(0),
      session_one_per_device_(0),
      session_bind_to_address_(0),
      webhooks_enabled_(true),
      webhook_subjects_(false),
      enable_signature_(false),
      signature_method_(SMSetIfNotSpecifiedInAccount),
      add_signatures_to_local_mail_(true),
      add_signatures_to_replies_(false),
      max_no_of_accounts_(0),
      max_no_of_aliases_(0),
      max_no_of_distribution_lists_(0),
      max_account_size_(0),
      limitations_enabled_(0),
      relay_port_(0),
      dkim_rotation_enabled_(false),
      dkim_rotation_interval_days_(180),
      dkim_rotation_propagation_hours_(72),
      dkim_rotation_retention_days_(7),
      dkim_rotation_key_bits_(2048),
      openpgp_publish_(false),
      openpgp_publish_new_accounts_(false),
      relay_requires_auth_(false),
      relay_connection_security_(CSNone),
      vacation_message_on_(false),
      vacation_external_override_(false),
      vacation_days_(0),
      quota_warning_percent_(-1),
      quota_grace_percent_(-1),
      quota_send_stop_percent_(-1),
      external_tag_subject_(false),
      external_tag_header_(false),
      first_contact_tip_(false),
      disclaimer_enabled_(false)
   {

   }

   Domain::~Domain()
   {

   }

   std::shared_ptr<Accounts>
   Domain::GetAccounts()
   {
      boost::lock_guard<boost::recursive_mutex> guard(collections_mutex_);

      if (!accounts_)
         accounts_ = std::shared_ptr<Accounts>(new Accounts(id_));

      accounts_->Refresh();
      return accounts_;
   }


   std::shared_ptr<DomainAliases>
   Domain::GetDomainAliases()
   {
      boost::lock_guard<boost::recursive_mutex> guard(collections_mutex_);

      if (!domain_aliases_)
         domain_aliases_ = std::shared_ptr<DomainAliases>(new DomainAliases(id_));

      domain_aliases_->Refresh();
      return domain_aliases_;
   }

   std::shared_ptr<Accounts>
   Domain::GetAccounts(__int64 iAccountID)
   {
      boost::lock_guard<boost::recursive_mutex> guard(collections_mutex_);

      if (!accounts_)
      {
         // Only fetch a specific account and put it in the list. This happens
         // when an API client only has limited access.
         accounts_ = std::shared_ptr<Accounts>(new Accounts(id_, iAccountID));
      }

      accounts_->Refresh();
      return accounts_;
   }

   std::shared_ptr<Aliases>
   Domain::GetAliases()
   {
      boost::lock_guard<boost::recursive_mutex> guard(collections_mutex_);

      if (!aliases_)
         aliases_ = std::shared_ptr<Aliases>(new Aliases(id_));
      
      aliases_->Refresh();

      return aliases_;
   }

   std::shared_ptr<DistributionLists>
   Domain::GetDistributionLists()
   {
      boost::lock_guard<boost::recursive_mutex> guard(collections_mutex_);

      if (!distribution_lists_)
         distribution_lists_ = std::shared_ptr<DistributionLists>(new DistributionLists(id_));

      distribution_lists_->Refresh();

      return distribution_lists_;
   }

   bool 
   Domain::XMLStore(XNode *pParentNode, int iBackupOptions)
   {
      XNode *pNode = pParentNode->AppendChild(_T("Domain"));

      pNode->AppendAttr(_T("Name"), name_);
      pNode->AppendAttr(_T("Postmaster"), postmaster_);
      pNode->AppendAttr(_T("ADDomainName"), addomain_name_);
      pNode->AppendAttr(_T("Active"), StringParser::IntToString(active_));
      pNode->AppendAttr(_T("MaxMessageSize"), StringParser::IntToString(max_message_size_));
      pNode->AppendAttr(_T("MaxSize"), StringParser::IntToString(max_size_mb_));
      pNode->AppendAttr(_T("MaxAccountSize"), StringParser::IntToString(max_account_size_));

      pNode->AppendAttr(_T("UsePlusAddressing"), use_plus_addressing_ ? _T("1") : _T("0"));
      pNode->AppendAttr(_T("PlusAddressingChar"), plus_addressing_char_);
      pNode->AppendAttr(_T("AntiSpamOptions"), StringParser::IntToString(anti_spam_options_));

      pNode->AppendAttr(_T("EnableSignature"), enable_signature_ ? _T("1") : _T("0"));
      pNode->AppendAttr(_T("SignatureMethod"), StringParser::IntToString(signature_method_));
      pNode->AppendAttr(_T("SignaturePlainText"), signature_plain_text_);
      pNode->AppendAttr(_T("SignatureHTML"), signature_html_);
      pNode->AppendAttr(_T("AddSignaturesToLocalMail"), add_signatures_to_local_mail_ ? _T("1") : _T("0"));
      pNode->AppendAttr(_T("AddSignaturesToReplies"), add_signatures_to_replies_ ? _T("1") : _T("0"));

      pNode->AppendAttr(_T("MaxNoOfAccounts"), StringParser::IntToString(max_no_of_accounts_));
      pNode->AppendAttr(_T("MaxNoOfAliases"), StringParser::IntToString(max_no_of_aliases_));
      pNode->AppendAttr(_T("MaxNoOfLists"), StringParser::IntToString(max_no_of_distribution_lists_));
      pNode->AppendAttr(_T("LimitationsEnabled"), StringParser::IntToString(limitations_enabled_));

      pNode->AppendAttr(_T("DKIMSelector"), dkim_selector_);
      pNode->AppendAttr(_T("DKIMPrivateKeyFile"), dkim_private_key_file_);
      pNode->AppendAttr(_T("DKIMSecondarySelector"), dkim_secondary_selector_);
      pNode->AppendAttr(_T("DKIMSecondaryPrivateKeyFile"), dkim_secondary_private_key_file_);
      pNode->AppendAttr(_T("DKIMRotationEnabled"), dkim_rotation_enabled_ ? _T("1") : _T("0"));
      pNode->AppendAttr(_T("DKIMRotationIntervalDays"), StringParser::IntToString(dkim_rotation_interval_days_));
      pNode->AppendAttr(_T("DKIMRotationPropagationHours"), StringParser::IntToString(dkim_rotation_propagation_hours_));
      pNode->AppendAttr(_T("DKIMRotationRetentionDays"), StringParser::IntToString(dkim_rotation_retention_days_));
      pNode->AppendAttr(_T("DKIMRotationKeyBits"), StringParser::IntToString(dkim_rotation_key_bits_));

      pNode->AppendAttr(_T("OpenPGPPublish"), openpgp_publish_ ? _T("1") : _T("0"));
      pNode->AppendAttr(_T("OpenPGPPublishNewAccounts"), openpgp_publish_new_accounts_ ? _T("1") : _T("0"));

      pNode->AppendAttr(_T("VacationMessageOn"), vacation_message_on_ ? _T("1") : _T("0"));
      pNode->AppendAttr(_T("VacationSubject"), vacation_subject_);
      pNode->AppendAttr(_T("VacationMessage"), vacation_message_);
      pNode->AppendAttr(_T("VacationInternalSubject"), vacation_internal_subject_);
      pNode->AppendAttr(_T("VacationInternalMessage"), vacation_internal_message_);
      pNode->AppendAttr(_T("VacationExternalOverride"), vacation_external_override_ ? _T("1") : _T("0"));
      pNode->AppendAttr(_T("VacationDays"), StringParser::IntToString(vacation_days_));
      pNode->AppendAttr(_T("QuotaWarningPercent"), StringParser::IntToString(quota_warning_percent_));
      pNode->AppendAttr(_T("QuotaWarningSubject"), quota_warning_subject_);
      pNode->AppendAttr(_T("QuotaWarningText"), quota_warning_text_);
      pNode->AppendAttr(_T("QuotaGracePercent"), StringParser::IntToString(quota_grace_percent_));
      pNode->AppendAttr(_T("QuotaSendStopPercent"), StringParser::IntToString(quota_send_stop_percent_));

      pNode->AppendAttr(_T("ExternalTagSubject"), external_tag_subject_ ? _T("1") : _T("0"));
      pNode->AppendAttr(_T("ExternalTagHeader"), external_tag_header_ ? _T("1") : _T("0"));
      pNode->AppendAttr(_T("ExternalTagText"), external_tag_text_);
      pNode->AppendAttr(_T("FirstContactTip"), first_contact_tip_ ? _T("1") : _T("0"));
      pNode->AppendAttr(_T("DisclaimerEnabled"), disclaimer_enabled_ ? _T("1") : _T("0"));
      pNode->AppendAttr(_T("DisclaimerPlainText"), disclaimer_plain_text_);
      pNode->AppendAttr(_T("DisclaimerHTML"), disclaimer_html_);

      pNode->AppendAttr(_T("QuarantineDigest"), quarantine_digest_ ? _T("1") : _T("0"));
      pNode->AppendAttr(_T("QuarantineDigestHour"), StringParser::IntToString(quarantine_digest_hour_));

      pNode->AppendAttr(_T("WebhooksEnabled"), webhooks_enabled_ ? _T("1") : _T("0"));
      pNode->AppendAttr(_T("WebhookSubjects"), webhook_subjects_ ? _T("1") : _T("0"));

      pNode->AppendAttr(_T("CountryPolicy"), country_policy_);

      pNode->AppendAttr(_T("SessionIdleSeconds"), StringParser::IntToString(session_idle_seconds_));
      pNode->AppendAttr(_T("SessionLifetimeSeconds"), StringParser::IntToString(session_lifetime_seconds_));
      pNode->AppendAttr(_T("SessionOnePerDevice"), StringParser::IntToString(session_one_per_device_));
      pNode->AppendAttr(_T("SessionBindToAddress"), StringParser::IntToString(session_bind_to_address_));

      if (!GetDomainAliases()->XMLStore(pNode, iBackupOptions))
         return false;

      // Accounts
      if (!GetAccounts()->XMLStore(pNode, iBackupOptions))
         return false;

      if (!GetAliases()->XMLStore(pNode, iBackupOptions))
         return false;

      if (!GetDistributionLists()->XMLStore(pNode, iBackupOptions))
         return false;

      // The domain's own rules, beside the accounts' under theirs.
      if (!GetRules()->XMLStore(pNode, iBackupOptions))
         return false;

      // The domain's allow and block list (hm_senderlist, scope domain). A restore
      // deletes the domain, and PersistentDomain::DeleteObject takes its list with
      // it, so the archive is the only place the list can come back from.
      std::shared_ptr<SenderListEntries> senderList =
         std::shared_ptr<SenderListEntries>(new SenderListEntries(SenderListEntry::ScopeDomain, id_));
      if (!senderList->Refresh())
         return false;
      if (!senderList->XMLStore(pNode, iBackupOptions))
         return false;

      // The domain's spam traps (hm_spamtraps, schema 6049). A restore deletes
      // every domain first and PersistentDomain::DeleteObject takes its traps
      // with it, so the archive is the only place they can come back from - and
      // a domain restored without them stops trapping without saying so.
      std::shared_ptr<SpamTraps> spamTraps =
         std::shared_ptr<SpamTraps>(new SpamTraps(id_));
      if (!spamTraps->Refresh())
         return false;
      if (!spamTraps->XMLStore(pNode, iBackupOptions))
         return false;

      return true;
   }

   bool 
   Domain::XMLLoad(XNode *pNode, int iRestoreOptions)
   {
      name_ = pNode->GetAttrValue(_T("Name"));
      postmaster_ = pNode->GetAttrValue(_T("Postmaster"));
      addomain_name_ = pNode->GetAttrValue(_T("ADDomainName"));
      active_ = _ttoi(pNode->GetAttrValue(_T("Active")));
      max_message_size_ = _ttoi(pNode->GetAttrValue(_T("MaxMessageSize")));
      max_size_mb_ = _ttoi(pNode->GetAttrValue(_T("MaxSize")));
      max_account_size_ = _ttoi(pNode->GetAttrValue(_T("MaxAccountSize")));

      use_plus_addressing_ = pNode->GetAttrValue(_T("UsePlusAddressing")) == _T("1");
      plus_addressing_char_ = pNode->GetAttrValue(_T("PlusAddressingChar"));
      anti_spam_options_ = _ttoi(pNode->GetAttrValue(_T("AntiSpamOptions")));

      enable_signature_ = pNode->GetAttrValue(_T("EnableSignature")) == _T("1");
      signature_method_ = (DomainSignatureMethod) _ttoi(pNode->GetAttrValue(_T("SignatureMethod")));
      signature_plain_text_ = pNode->GetAttrValue(_T("SignaturePlainText"));
      signature_html_ = pNode->GetAttrValue(_T("SignatureHTML"));

      add_signatures_to_local_mail_ = pNode->GetAttrValue(_T("AddSignaturesToLocalMail")) == _T("1");
      add_signatures_to_replies_ = pNode->GetAttrValue(_T("AddSignaturesToReplies")) == _T("1");

      max_no_of_accounts_ = _ttoi(pNode->GetAttrValue(_T("MaxNoOfAccounts")));
      max_no_of_aliases_ = _ttoi(pNode->GetAttrValue(_T("MaxNoOfAliases")));
      max_no_of_distribution_lists_ = _ttoi(pNode->GetAttrValue(_T("MaxNoOfLists")));
      
      limitations_enabled_ = _ttoi(pNode->GetAttrValue(_T("LimitationsEnabled")));

      dkim_selector_ = pNode->GetAttrValue(_T("DKIMSelector"));
      dkim_private_key_file_ = pNode->GetAttrValue(_T("DKIMPrivateKeyFile"));

      // A backup taken before the secondary selector existed has no such
      // attributes, and GetAttrValue then returns an empty string - which is
      // exactly the state of a domain that has never staged a rotation, so an
      // old backup restores to today's behaviour.
      dkim_secondary_selector_ = pNode->GetAttrValue(_T("DKIMSecondarySelector"));
      dkim_secondary_private_key_file_ = pNode->GetAttrValue(_T("DKIMSecondaryPrivateKeyFile"));

      // The rotation policy. A backup taken before it existed has none of these
      // attributes: the schedule restores off, and a period read as 0 - which no
      // valid policy has - restores as the shipped default, so switching it on
      // after the restore starts from the same values a new domain gets. The
      // rotation's state is not in the backup at all: it is rebuilt from the
      // domain's own selectors by the first pass after the restore.
      dkim_rotation_enabled_ = pNode->GetAttrValue(_T("DKIMRotationEnabled")) == _T("1");
      dkim_rotation_interval_days_ = _ttoi(pNode->GetAttrValue(_T("DKIMRotationIntervalDays")));
      dkim_rotation_propagation_hours_ = _ttoi(pNode->GetAttrValue(_T("DKIMRotationPropagationHours")));
      dkim_rotation_retention_days_ = _ttoi(pNode->GetAttrValue(_T("DKIMRotationRetentionDays")));
      dkim_rotation_key_bits_ = _ttoi(pNode->GetAttrValue(_T("DKIMRotationKeyBits")));

      // A backup taken before the Web Key Directory existed carries neither
      // attribute, and both restore as off - which is what every domain in such
      // a backup had. A restore never starts publishing a key.
      openpgp_publish_ = pNode->GetAttrValue(_T("OpenPGPPublish")) == _T("1");
      openpgp_publish_new_accounts_ = pNode->GetAttrValue(_T("OpenPGPPublishNewAccounts")) == _T("1");

      if (dkim_rotation_interval_days_ <= 0)
         dkim_rotation_interval_days_ = 180;
      if (dkim_rotation_propagation_hours_ <= 0)
         dkim_rotation_propagation_hours_ = 72;
      if (dkim_rotation_retention_days_ <= 0)
         dkim_rotation_retention_days_ = 7;
      if (dkim_rotation_key_bits_ <= 0)
         dkim_rotation_key_bits_ = 2048;

      // A backup taken before these attributes existed restores as "off with empty
      // texts", which is the state of a domain that has never configured the
      // domain-wide out-of-office reply. Assigned to the members rather than through
      // SetVacationMessageIsOn: this is a restore, not an administrator's toggle,
      // and must not touch the running server's already-answered memory.
      vacation_message_on_ = pNode->GetAttrValue(_T("VacationMessageOn")) == _T("1");
      vacation_subject_ = pNode->GetAttrValue(_T("VacationSubject"));
      vacation_message_ = pNode->GetAttrValue(_T("VacationMessage"));
      vacation_internal_subject_ = pNode->GetAttrValue(_T("VacationInternalSubject"));
      vacation_internal_message_ = pNode->GetAttrValue(_T("VacationInternalMessage"));
      vacation_external_override_ = pNode->GetAttrValue(_T("VacationExternalOverride")) == _T("1");
      vacation_days_ = pNode->GetAttrValue(_T("VacationDays")).IsEmpty() ? 0 : _ttoi(pNode->GetAttrValue(_T("VacationDays")));
      // A backup from before the quota policy carries none of it: the server's
      // values apply, as they did when the backup was taken.
      quota_warning_percent_ = pNode->GetAttrValue(_T("QuotaWarningPercent")).IsEmpty() ? -1 : _ttoi(pNode->GetAttrValue(_T("QuotaWarningPercent")));
      quota_warning_subject_ = pNode->GetAttrValue(_T("QuotaWarningSubject"));
      quota_warning_text_ = pNode->GetAttrValue(_T("QuotaWarningText"));
      quota_grace_percent_ = pNode->GetAttrValue(_T("QuotaGracePercent")).IsEmpty() ? -1 : _ttoi(pNode->GetAttrValue(_T("QuotaGracePercent")));
      quota_send_stop_percent_ = pNode->GetAttrValue(_T("QuotaSendStopPercent")).IsEmpty() ? -1 : _ttoi(pNode->GetAttrValue(_T("QuotaSendStopPercent")));

      // The same restore rule as above: a backup taken before these existed
      // restores as off with empty texts, which is a domain that transforms
      // nothing - the shipped state. Assigned to the members, because a restore
      // is not an administrator's toggle.
      external_tag_subject_ = pNode->GetAttrValue(_T("ExternalTagSubject")) == _T("1");
      external_tag_header_ = pNode->GetAttrValue(_T("ExternalTagHeader")) == _T("1");
      external_tag_text_ = pNode->GetAttrValue(_T("ExternalTagText"));
      first_contact_tip_ = pNode->GetAttrValue(_T("FirstContactTip")) == _T("1");
      disclaimer_enabled_ = pNode->GetAttrValue(_T("DisclaimerEnabled")) == _T("1");
      disclaimer_plain_text_ = pNode->GetAttrValue(_T("DisclaimerPlainText"));
      disclaimer_html_ = pNode->GetAttrValue(_T("DisclaimerHTML"));

      // The digest, under the same rule: a backup taken before it existed has no
      // attribute and restores with the digest off at the default hour, which is
      // every domain before an administrator turns it on.
      quarantine_digest_ = pNode->GetAttrValue(_T("QuarantineDigest")) == _T("1") ? 1 : 0;

      const String digestHour = pNode->GetAttrValue(_T("QuarantineDigestHour"));
      if (!digestHour.IsEmpty())
      {
         const int hour = _ttoi(digestHour);
         if (hour >= 0 && hour <= 23)
            quarantine_digest_hour_ = hour;
      }

      // The webhook flags. An archive taken before they existed has neither
      // attribute, and the constructor's values stand: events raised, subjects
      // withheld - which is what an upgraded domain gets. Read one way round
      // each, deliberately: the absent attribute must not be able to switch
      // subject disclosure ON, and must not be able to switch a domain OFF.
      const String webhooksEnabled = pNode->GetAttrValue(_T("WebhooksEnabled"));
      if (!webhooksEnabled.IsEmpty())
         webhooks_enabled_ = webhooksEnabled == _T("1");

      webhook_subjects_ = pNode->GetAttrValue(_T("WebhookSubjects")) == _T("1");

      // The same rule again: a backup taken before country blocking existed has
      // no attribute and restores with no policy, which is every domain until an
      // administrator writes one.
      country_policy_ = pNode->GetAttrValue(_T("CountryPolicy"));

      // The session policies, under the same rule: an archive written before
      // schema 6050 carries none of these attributes and restores with all four at
      // 0, which is the domain taking the server's answer - the state every domain
      // is in until an administrator says otherwise. A number outside the range the
      // interfaces accept is ignored rather than stored.
      ReadPolicyAttribute_(pNode, _T("SessionIdleSeconds"), 0, 2592000, session_idle_seconds_);
      ReadPolicyAttribute_(pNode, _T("SessionLifetimeSeconds"), 0, 2592000, session_lifetime_seconds_);
      ReadPolicyAttribute_(pNode, _T("SessionOnePerDevice"), 0, 2, session_one_per_device_);
      ReadPolicyAttribute_(pNode, _T("SessionBindToAddress"), 0, 2, session_bind_to_address_);

      return true;
   }

   bool 
   Domain::XMLLoadSubItems(XNode *pNode, int iRestoreOptions)
   {
      if (!GetDomainAliases()->XMLLoad(pNode, iRestoreOptions))
         return false;

      if (!GetAccounts()->XMLLoad(pNode, iRestoreOptions))
         return false;

      if (!GetRules()->XMLLoad(pNode, iRestoreOptions))
         return false;

      if (!GetAliases()->XMLLoad(pNode, iRestoreOptions))
         return false;

      if (!GetDistributionLists()->XMLLoad(pNode, iRestoreOptions))
         return false;

      // Under the domain's new id; see SenderListEntries::PreSaveObject.
      std::shared_ptr<SenderListEntries> senderList =
         std::shared_ptr<SenderListEntries>(new SenderListEntries(SenderListEntry::ScopeDomain, id_));
      if (!senderList->Refresh())
         return false;
      if (!senderList->XMLLoad(pNode, iRestoreOptions))
         return false;

      // Under the domain's new id; see SpamTraps::PreSaveObject.
      std::shared_ptr<SpamTraps> spamTraps =
         std::shared_ptr<SpamTraps>(new SpamTraps(id_));
      if (!spamTraps->Refresh())
         return false;
      if (!spamTraps->XMLLoad(pNode, iRestoreOptions))
         return false;

      return true;
   }

   bool 
   Domain::GetASUseGreyListing() const
   {
      return (anti_spam_options_ & ASUseGreylisting) ? true : false;
   }

   void 
   Domain::SetASUseGreyListing(bool bNewVal)
   {
      if (bNewVal)
         anti_spam_options_ = anti_spam_options_ | ASUseGreylisting;
      else
         anti_spam_options_ = anti_spam_options_ & ~ASUseGreylisting;      

   }

   int
   Domain::GetMaxSizeMB() const
   {
      return max_size_mb_;
   }

   void 
   Domain::SetMaxSizeMB(int iNewVal)
   {
      max_size_mb_ = iNewVal;
   }

   bool  
   Domain::GetEnableSignature() const
   {
      return enable_signature_;
   }

   void 
   Domain::SetEnableSignature(bool bNewVal)
   {
      enable_signature_ = bNewVal;
   }

   Domain::DomainSignatureMethod 
   Domain::GetSignatureMethod() const
   {
      return signature_method_;
   }

   void 
   Domain::SetSignatureMethod(Domain::DomainSignatureMethod eSM)
   {
      signature_method_ = eSM;
   }

   String 
   Domain::GetSignaturePlainText() const
   {
      return signature_plain_text_;
   }

   void 
   Domain::SetSignaturePlainText(const String &sSignature)
   {
      signature_plain_text_ = sSignature;
   }

   String 
   Domain::GetSignatureHTML() const
   {
      return signature_html_;
   }

   void 
   Domain::SetSignatureHTML(const String &sSignature)
   {
      signature_html_ = sSignature;
   }


   bool
   Domain::GetAddSignaturesToReplies() const
   {
      return add_signatures_to_replies_;
   }

   void 
   Domain::SetAddSignaturesToReplies(bool bNewVal)
   {
      add_signatures_to_replies_ = bNewVal;
   }

   bool
   Domain::GetAddSignaturesToLocalMail() const
   {
      return add_signatures_to_local_mail_;
   }

   void 
   Domain::SetAddSignaturesToLocalMail(bool bNewVal)
   {
      add_signatures_to_local_mail_ = bNewVal;
   }

   int
   Domain::GetMaxNoOfAccounts() const
   {
      return max_no_of_accounts_;
   }

   void 
   Domain::SetMaxNoOfAccounts(int iNewVal)
   {
      max_no_of_accounts_ = iNewVal;
   }

   int
   Domain::GetMaxNoOfAliases() const
   {
      return max_no_of_aliases_;
   }

   void 
   Domain::SetMaxNoOfAliases(int iNewVal)
   {
      max_no_of_aliases_ = iNewVal;
   }

   int
   Domain::GetMaxNoOfDistributionLists() const
   {
      return max_no_of_distribution_lists_;
   }

   void 
   Domain::SetMaxNoOfDistributionLists(int iNewVal)
   {
      max_no_of_distribution_lists_ = iNewVal;
   }

   int
   Domain::GetLimitationsEnabled() const
   {
      return limitations_enabled_;
   }

   void
   Domain::SetLimitationsEnabled(int iNewVal)
   {
      limitations_enabled_ = iNewVal;
   }

   bool
   Domain::GetMaxNoOfAccountsEnabled() const
   {
      return (limitations_enabled_ & MaxNoOfAccounts) ? true : false;
   }

   void
   Domain::SetMaxNoOfAccountsEnabled(bool bNewVal)
   {
      if (bNewVal)
         limitations_enabled_ = limitations_enabled_ | MaxNoOfAccounts;
      else
         limitations_enabled_ = limitations_enabled_ & ~MaxNoOfAccounts;
   }

   bool
   Domain::GetMaxNoOfAliasesEnabled() const
   {
      return (limitations_enabled_ & MaxNoOfAliases) ? true : false;
   }

   void 
   Domain::SetMaxNoOfAliasesEnabled(bool bNewVal)
   {
      if (bNewVal)
         limitations_enabled_ = limitations_enabled_ | MaxNoOfAliases;
      else
         limitations_enabled_ = limitations_enabled_ & ~MaxNoOfAliases;

   }

   bool
   Domain::GetMaxNoOfDistributionListsEnabled() const
   {
      return (limitations_enabled_ & MaxNoOfDistributionLists) ? true : false;
   }

   void 
   Domain::SetMaxNoOfDistributionListsEnabled(bool bNewVal)
   {
      if (bNewVal)
         limitations_enabled_ = limitations_enabled_ | MaxNoOfDistributionLists;
      else
         limitations_enabled_ = limitations_enabled_ & ~MaxNoOfDistributionLists;
   }

   int
   Domain::GetMaxAccountSize() const
   {
      return max_account_size_;
   }

   void 
   Domain::SetMaxAccountSize(int iNewVal)
   {
      max_account_size_ = iNewVal;
   }

   bool
   Domain::GetDKIMEnabled() const
   {
      return (anti_spam_options_ & ASDKIMSign) ? true : false;
   }

   void
   Domain::SetDKIMEnabled(bool newValue)
   {
      anti_spam_options_ = newValue ? anti_spam_options_ | ASDKIMSign : anti_spam_options_ & ~ASDKIMSign;
   }

   bool
   Domain::GetDKIMAliasesEnabled() const
   {
      return (anti_spam_options_ & ASDKIMSignAliases) ? true : false;
   }

   void
   Domain::SetDKIMAliasesEnabled(bool newValue)
   {
      anti_spam_options_ = newValue ? anti_spam_options_ | ASDKIMSignAliases : anti_spam_options_ & ~ASDKIMSignAliases;
   }

   AnsiString 
   Domain::GetDKIMSelector() const
   {
      return dkim_selector_;
   }

   void 
   Domain::SetDKIMSelector(const String &newValue)
   {
      dkim_selector_ = newValue;
   }

   String
   Domain::GetDKIMPrivateKeyFile() const
   {
      return dkim_private_key_file_;
   }

   void
   Domain::SetDKIMPrivateKeyFile(const String &newValue)
   {
      dkim_private_key_file_ = newValue;
   }

   AnsiString
   Domain::GetDKIMSecondarySelector() const
   {
      return dkim_secondary_selector_;
   }

   void
   Domain::SetDKIMSecondarySelector(const String &newValue)
   {
      dkim_secondary_selector_ = newValue;
   }

   String
   Domain::GetDKIMSecondaryPrivateKeyFile() const
   {
      return dkim_secondary_private_key_file_;
   }

   void
   Domain::SetDKIMSecondaryPrivateKeyFile(const String &newValue)
   {
      dkim_secondary_private_key_file_ = newValue;
   }

   bool
   Domain::PromoteDKIMSecondary(String &errorMessage)
   {
      // Promoting with nothing staged would overwrite the primary with empty
      // values, and the next message from this domain would go out unsigned
      // with only an error-log entry to show for it. Refusing here turns that
      // silent outage into an immediate, visible failure of the promote itself.
      if (dkim_secondary_selector_.IsEmpty() || dkim_secondary_private_key_file_.IsEmpty())
      {
         errorMessage = "A secondary DKIM selector and its private key file must both be configured before they can be promoted.";
         return false;
      }

      // The key file is checked now rather than at the first signing attempt,
      // because the promote below erases the old key from the configuration.
      // A promote onto a missing file would leave the domain with no usable
      // key at all - precisely the signing outage a staged rotation exists to
      // prevent - so it is refused while the old key is still in place.
      if (!FileUtilities::Exists(dkim_secondary_private_key_file_))
      {
         errorMessage = Formatter::Format("The secondary DKIM private key file {0} does not exist. Promoting it would leave the domain unable to sign.", dkim_secondary_private_key_file_);
         return false;
      }

      dkim_selector_ = dkim_secondary_selector_;
      dkim_private_key_file_ = dkim_secondary_private_key_file_;

      // The old primary is cleared rather than swapped into the secondary
      // slot. Rotation retires the old key: keeping it staged would invite a
      // second promote that resurrects a key whose DNS record the operator is
      // about to remove, and every message signed with it would then fail
      // verification.
      dkim_secondary_selector_.Empty();
      dkim_secondary_private_key_file_.Empty();

      return true;
   }

   int 
   Domain::GetDKIMHeaderCanonicalizationMethod() const
   {
      if (anti_spam_options_ & ASDKIMSimpleHeader)
         return 1;
      else
         return 2;
   }

   void 
   Domain::SetDKIMHeaderCanonicalizationMethod(int newValue)
   {
      if (newValue == 1)
         anti_spam_options_ |= ASDKIMSimpleHeader;
      else
         anti_spam_options_ &= ~ASDKIMSimpleHeader;
   }


   int 
   Domain::GetDKIMBodyCanonicalizationMethod() const
   {
      if (anti_spam_options_ & ASDKIMSimpleBody)
         return 1;
      else
         return 2;
   }

   void 
   Domain::SetDKIMBodyCanonicalizationMethod(int newValue)
   {
      if (newValue == 1)
         anti_spam_options_ |= ASDKIMSimpleBody;
      else
         anti_spam_options_ &=  ~ASDKIMSimpleBody;
   }

   int 
   Domain::GetDKIMSigningAlgorithm() const
   {
      if (anti_spam_options_ & ASDKIMSHA1)
         return 1;
      else
         return 2;
   }

   void 
   Domain::SetDKIMSigningAlgorithm(int newValue)
   {
      if (newValue == 1)
         anti_spam_options_ |=  ASDKIMSHA1;
      else
         anti_spam_options_ &= ~ASDKIMSHA1;
   }

   void
   Domain::SetVacationMessageIsOn(bool bNewVal)
   {
      if (!bNewVal && vacation_message_on_ && !name_.IsEmpty())
      {
         // The administrator has just turned the domain-wide out-of-office reply
         // off. Forget who has already been answered on this domain's behalf, the
         // same way Account::SetVacationMessageIsOn forgets for one account: without
         // this, switching the reply off and on again (a re-opened office closing
         // for the next holiday) stays silent towards everyone it answered the
         // first time, for as long as the service runs.
         //
         // This over-clears on purpose. The map does not record WHICH feature
         // answered a sender, so entries made by an account's own vacation message
         // for accounts in this domain are dropped too; the cost is one repeated
         // courtesy reply, the alternative is a reply that never comes.
         SMTPVacationMessageCreator::Instance()->DomainVacationMessageTurnedOff(name_);
      }

      vacation_message_on_ = bNewVal;
   }

   size_t
   Domain::GetEstimatedCachingSize()
   {
      return 1024;
   }

   std::shared_ptr<Rules>
   Domain::GetRules()
   {
      if (!rules_)
      {
         rules_ = std::shared_ptr<Rules>(new Rules(0, GetID()));
         rules_->Refresh();
      }

      return rules_;
   }
}
