// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"
#include "DistributionList.h"

#include "DistributionListRecipients.h"

#include "../Persistence/PersistentDistributionListRecipient.h"
#include "../Util/ListManager.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   DistributionList::DistributionList(void) :
      domain_id_(false),
      require_auth_(false),
      list_mode_(LMPublic),
      enabled_(false),
      managed_(false),
      subscribe_policy_(SubscribeClosed),
      archive_(ArchiveNone),
      archive_days_(0),
      digest_(false),
      digest_hour_(7),
      digest_last_id_(0),
      digest_last_time_(0),
      reply_to_list_(false),
      verp_(false)
   {
      
   }

   DistributionList::~DistributionList(void)
   {
   }

   bool 
   DistributionList::XMLStore(XNode *pParentNode, int iOptions)
   {
      XNode *pNode = pParentNode->AppendChild(_T("DistributionList"));

      String sListMode;
      sListMode.Format(_T("%d"), list_mode_);

      pNode->AppendAttr(_T("Name"), address_);
      pNode->AppendAttr(_T("Active"), enabled_ ? _T("1") : _T("0"));
      pNode->AppendAttr(_T("RequiresAuth"), require_auth_ ? _T("1") : _T("0"));
      pNode->AppendAttr(_T("RequiresAuthAddress"), require_address_);
      pNode->AppendAttr(_T("ListMode"), sListMode);
      pNode->AppendAttr(_T("ModeratorAddress"), moderator_address_);
      pNode->AppendAttr(_T("BounceAddress"), bounce_address_);
      pNode->AppendAttr(_T("Membership"), membership_);

      // The list manager's settings (ListManager.h). Its runtime state - where
      // the last digest stopped, the archive - is not configuration and is not
      // in the backup: a restored list starts its digest from the next posting.
      pNode->AppendAttr(_T("Managed"), managed_ ? _T("1") : _T("0"));
      pNode->AppendAttr(_T("SubscribePolicy"), StringParser::IntToString(subscribe_policy_));
      pNode->AppendAttr(_T("Archive"), StringParser::IntToString(archive_));
      pNode->AppendAttr(_T("ArchiveDays"), StringParser::IntToString(archive_days_));
      pNode->AppendAttr(_T("Digest"), digest_ ? _T("1") : _T("0"));
      pNode->AppendAttr(_T("DigestHour"), StringParser::IntToString(digest_hour_));
      pNode->AppendAttr(_T("ReplyToList"), reply_to_list_ ? _T("1") : _T("0"));
      pNode->AppendAttr(_T("SubjectPrefix"), subject_prefix_);
      pNode->AppendAttr(_T("Footer"), footer_);
      pNode->AppendAttr(_T("Verp"), verp_ ? _T("1") : _T("0"));
      pNode->AppendAttr(_T("Owner"), owner_);

      // The list's secret goes with it, so that the links in mail the list has
      // already sent to the accounts its query answers still work after a
      // restore - the backup keeps each member's token for the same reason.
      pNode->AppendAttr(_T("Secret"), secret_);

      return GetMembers()->XMLStore(pNode, iOptions);
   }

   bool
   DistributionList::XMLLoad(XNode *pNode, int iRestoreOptions)
   {
      address_ = pNode->GetAttrValue(_T("Name"));
      enabled_ = pNode->GetAttrValue(_T("Active")) == _T("1");
      require_auth_ = pNode->GetAttrValue(_T("RequiresAuth")) == _T("1");
      require_address_ = pNode->GetAttrValue(_T("RequiresAuthAddress"));
      list_mode_ = (ListMode) _ttoi(pNode->GetAttrValue(_T("ListMode")));

      // Absent in backups taken before these fields existed; GetAttrValue returns
      // an empty string then, and empty is exactly the off state both fields need.
      moderator_address_ = pNode->GetAttrValue(_T("ModeratorAddress"));
      bounce_address_ = pNode->GetAttrValue(_T("BounceAddress"));

      // Absent in backups taken before the membership query existed, and empty
      // is the static list every one of those was.
      membership_ = pNode->GetAttrValue(_T("Membership"));

      // Absent in backups taken before the list manager existed; every one of
      // those is an unmanaged list, which is what the empty strings read as.
      // The digest hour is the one field whose absent value is not 0.
      managed_ = pNode->GetAttrValue(_T("Managed")) == _T("1");
      subscribe_policy_ = _ttoi(pNode->GetAttrValue(_T("SubscribePolicy")));
      archive_ = _ttoi(pNode->GetAttrValue(_T("Archive")));
      archive_days_ = _ttoi(pNode->GetAttrValue(_T("ArchiveDays")));
      digest_ = pNode->GetAttrValue(_T("Digest")) == _T("1");
      const String digestHour = pNode->GetAttrValue(_T("DigestHour"));
      digest_hour_ = digestHour.IsEmpty() ? 7 : _ttoi(digestHour);
      reply_to_list_ = pNode->GetAttrValue(_T("ReplyToList")) == _T("1");
      subject_prefix_ = pNode->GetAttrValue(_T("SubjectPrefix"));
      footer_ = pNode->GetAttrValue(_T("Footer"));
      verp_ = pNode->GetAttrValue(_T("Verp")) == _T("1");
      owner_ = pNode->GetAttrValue(_T("Owner"));
      secret_ = pNode->GetAttrValue(_T("Secret"));

      return true;
   }

   bool
   DistributionList::ValidateManager(String &reason) const
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // The manager's fields, judged whether or not the manager is on: a value a
   // column holds is a value somebody will switch on later, and an hour of 25
   // saved today is a digest that never goes tomorrow.
   //---------------------------------------------------------------------------()
   {
      if (subscribe_policy_ < SubscribeClosed || subscribe_policy_ > SubscribeModerated)
      {
         reason = "The subscribe policy must be 0 (closed), 1 (confirm) or 2 (moderated).";
         return false;
      }

      if (archive_ < ArchiveNone || archive_ > ArchivePublic)
      {
         reason = "The archive must be 0 (none), 1 (members) or 2 (public).";
         return false;
      }

      if (archive_days_ < 0 || archive_days_ > 3650)
      {
         reason = "The archive's days must be between 0 (for ever) and 3650.";
         return false;
      }

      if (digest_hour_ < 0 || digest_hour_ > 23)
      {
         reason = "The digest hour must be between 0 and 23.";
         return false;
      }

      if (subject_prefix_.GetLength() > MaxSubjectPrefixLength)
      {
         reason = "The subject prefix is at most 64 characters.";
         return false;
      }

      if (subject_prefix_.Find(_T("\r")) >= 0 || subject_prefix_.Find(_T("\n")) >= 0)
      {
         reason = "The subject prefix is one line.";
         return false;
      }

      if (footer_.GetLength() > MaxFooterLength)
      {
         reason = "The footer is at most 2000 characters.";
         return false;
      }

      String owner = owner_;
      owner.Trim();

      if (!owner.IsEmpty())
      {
         // Exactly one address: the notices are handed to RecipientParser,
         // which would read "a@x, b@y" as two.
         if (owner.GetLength() > 255 || !StringParser::IsValidEmailAddress(owner))
         {
            reason = "The owner must be one e-mail address.";
            return false;
         }

         // The list's own address, or one of the addresses the manager
         // answers for it, would hand every approval and every bounce notice
         // back to the list: the owner's mail would be a posting, or a command,
         // or a loop. Read with the manager's own grammar, so that team-lead@
         // may own team@ - it is none of team's addresses.
         if (!address_.IsEmpty() && ListManager::IsListOwnAddress(owner, address_))
         {
            reason = "The owner cannot be the list itself or one of the addresses it answers.";
            return false;
         }
      }

      return true;
   }

   bool
   DistributionList::XMLLoadSubItems(XNode *pNode, int iRestoreOptions)
   {
      std::shared_ptr<DistributionListRecipients> pDistListRecipients = GetMembers();
      return pDistListRecipients->XMLLoad(pNode, iRestoreOptions);
   }

   String
   DistributionList::GetRfc2919ListId() const
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Builds the RFC 2919 List-Id value from the list address. The identifier is
   // required to have the syntax of a domain name, so any character of the local
   // part which is not legal in a DNS label is replaced with a hyphen. The result
   // is never resolved - RFC 2919 identifiers are opaque - but it does have to
   // parse, and hMailServer permits list addresses (underscores, quoted local
   // parts) which would otherwise produce a syntactically invalid header.
   //---------------------------------------------------------------------------()
   {
      // Both halves of the address have to exist before it can be split. This is
      // checked here rather than left to ExtractAddress/ExtractDomain because those
      // split on the last @ without validating: handed an address with no @ at all
      // they return the whole string as the domain, which would turn a malformed
      // list address into a plausible-looking but meaningless identifier.
      const int atSignPosition = address_.ReverseFind(_T("@"));

      if (atSignPosition < 1 || atSignPosition == address_.GetLength() - 1)
         return "";

      const String localPart = StringParser::ExtractAddress(address_);
      const String domainPart = StringParser::ExtractDomain(address_);

      if (localPart.IsEmpty() || domainPart.IsEmpty())
         return "";

      String sanitizedLocalPart;

      for (const wchar_t character : localPart)
      {
         const bool legalInLabel =
            (character >= L'a' && character <= L'z') ||
            (character >= L'A' && character <= L'Z') ||
            (character >= L'0' && character <= L'9') ||
            character == L'-' || character == L'.';

         sanitizedLocalPart += legalInLabel ? character : L'-';
      }

      String listId;
      listId += "<";
      listId += sanitizedLocalPart;
      listId += ".";
      listId += domainPart;
      listId += ">";

      return listId;
   }

   std::shared_ptr<DistributionListRecipients>
   DistributionList::GetMembers() const
   {
      std::shared_ptr<DistributionListRecipients> members = std::shared_ptr<DistributionListRecipients> (new DistributionListRecipients(id_)) ;
      members->Refresh();
      return members;
   }

   size_t
   DistributionList::GetEstimatedCachingSize()
   {
      return 1024;
   }

}
