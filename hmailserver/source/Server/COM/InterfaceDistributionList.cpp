// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#include "stdafx.h"
#include "InterfaceDistributionList.h"
#include "InterfaceDistributionListRecipients.h"

#include "../Common/Persistence/PersistentDistributionList.h"
#include "../Common/Persistence/PersistentDistributionListRecipient.h"

#include "../Common/BO/DistributionLists.h"
#include "../Common/BO/DistributionListRecipients.h"
#include "../Common/BO/Domain.h"
#include "../Common/Cache/CacheContainer.h"
#include "../Common/Util/DistributionListMembership.h"
#include "../Common/Util/ListManager.h"
#include "../Common/Util/ListArchive.h"

#include "COMAuthentication.h"
#include "COMError.h"
#include "../Common/Util/AuditTrail.h"


#ifdef _DEBUG
   long InterfaceDistributionList::counter = 0;
#endif

STDMETHODIMP InterfaceDistributionList::InterfaceSupportsErrorInfo(REFIID riid)
{
   try
   {
      static const IID* arr[] = 
      {
         &IID_IInterfaceDistributionList,
      };
   
      for (int i=0;i<sizeof(arr)/sizeof(arr[0]);i++)
      {
         if (InlineIsEqualGUID(*arr[i],riid))
            return S_OK;
      }
      return S_FALSE;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDistributionList::get_ID(long *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = (long) object_->GetID();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDistributionList::get_Address(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

   
   
      HM::String sName = object_->GetAddress();
   
      *pVal = sName.AllocSysString();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDistributionList::put_Address(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

   
   
      HM::String sName = newVal;
      object_->SetAddress(sName);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDistributionList::get_RequireSenderAddress(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

   
   
      HM::String sAddress = object_->GetRequireAddress();
   
      *pVal = sAddress.AllocSysString();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDistributionList::put_RequireSenderAddress(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

   
   
      HM::String sName = newVal;
      object_->SetRequireAddress(sName);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDistributionList::get_RequireSMTPAuth(VARIANT_BOOL *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

   
   
      if (object_->GetRequireAuth())
         *pVal = VARIANT_TRUE;
      else
         *pVal = VARIANT_FALSE;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDistributionList::put_RequireSMTPAuth(VARIANT_BOOL newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

   
   
      if (newVal == VARIANT_TRUE)
         object_->SetRequireAuth(true);
      else
         object_->SetRequireAuth(false);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDistributionList::get_Active(VARIANT_BOOL *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

   
   
      if (object_->GetActive())
         *pVal = VARIANT_TRUE;
      else
         *pVal = VARIANT_FALSE;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDistributionList::put_Active(VARIANT_BOOL newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

   
   
      if (newVal == VARIANT_TRUE)
         object_->SetActive(true);
      else
         object_->SetActive(false);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDistributionList::Delete()
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!object_)
         return GetAccessDenied();

   
   
      if (!object_)
         return S_FALSE;
   
      HM::PersistentDistributionList::DeleteObject(object_);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDistributionList::Save()
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!object_)
         return GetAccessDenied();

      if (!object_)
         return S_FALSE;
   
      HM::String sErrorMessage;
      if (HM::PersistentDistributionList::SaveObject(object_, sErrorMessage, HM::PersistenceModeNormal))
      {
         // Add to parent collection
         AddToParentCollection();
   
         return S_OK;
      }
   
      return COMError::GenerateError("Failed to save object. " +  sErrorMessage);
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDistributionList::get_Recipients(IInterfaceDistributionListRecipients **pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      CComObject<InterfaceDistributionListRecipients>* pItem = new CComObject<InterfaceDistributionListRecipients>();
      pItem->SetAuthentication(authentication_);
   
      pItem->SetListID(object_->GetID());
      std::shared_ptr<HM::DistributionListRecipients> pRecipients = object_->GetMembers();
   
      if (pRecipients)
      {
         pItem->Attach(pRecipients);
         pItem->AddRef();
         *pVal = pItem;
      }
   
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDistributionList::get_ModeratorAddress(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      HM::String sAddress = object_->GetModeratorAddress();

      *pVal = sAddress.AllocSysString();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDistributionList::put_ModeratorAddress(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      HM::String sAddress = newVal;
      object_->SetModeratorAddress(sAddress);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDistributionList::get_BounceAddress(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      HM::String sAddress = object_->GetBounceAddress();

      *pVal = sAddress.AllocSysString();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDistributionList::put_BounceAddress(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      HM::String sAddress = newVal;
      object_->SetBounceAddress(sAddress);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDistributionList::get_Membership(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      HM::String sMembership = object_->GetMembership();

      *pVal = sMembership.AllocSysString();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDistributionList::put_Membership(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      // Judged here as PreSaveLimitationsCheck will judge it at Save, so that a
      // script learns of a text that is none of the forms at the assignment
      // rather than at the save, in the same sentence; Save judges it again,
      // since a domain the query names can go between the two.
      HM::String sMembership = newVal;
      HM::String normalised, reason;

      if (!HM::DistributionListMembership::Validate(sMembership, normalised, reason))
         return COMError::GenerateError(reason);

      // A query that reaches beyond the domains this credential administers -
      // another tenant's accounts, a pattern across every domain, everyone - is
      // the server administrator's to set: a domain administrator would
      // otherwise make a list in their domain that delivers to every account of
      // another tenant's.
      //
      // The domains are the grant's, which is what the REST handler passes
      // (RestApiAdministration.cpp, MembershipReachesPastCaller), so an
      // administrator whose grant names two domains may set domain:<the other
      // one> here exactly as they may over REST. Passing the list's own domain
      // alone would confine the Control Panel more tightly than the API for the
      // same person, which is a difference nobody would find written down.
      // A grant with no domains that does not reach every domain is not an
      // administrator at all; it is confined to the list's own domain, which is
      // the narrowest honest reading and never wider than before.
      HM::DistributionListMembership::Query query;
      HM::String parseReason;
      HM::DistributionListMembership::Parse(normalised, query, parseReason);

      std::shared_ptr<const HM::Domain> domain = HM::CacheContainer::Instance()->GetDomain(object_->GetDomainID());
      const HM::String ownDomain = domain ? domain->GetName() : HM::StringParser::ExtractDomain(object_->GetAddress());

      if (!authentication_->ReachesEveryDomain())
      {
         std::vector<HM::String> callerDomains = authentication_->GetGrant().domains;

         if (callerDomains.empty())
            callerDomains.push_back(ownDomain);

         if (!HM::DistributionListMembership::WithinDomains(query, ownDomain, callerDomains))
            return COMError::GenerateError("Membership names accounts outside the domains this administrator holds; a domain administrator may set domain, a pattern in a domain they hold, or nothing. The other forms are the server administrator's.");
      }

      object_->SetMembership(normalised);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDistributionList::get_Mode(eDistributionListMode *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      switch (object_->GetListMode())
      {
      case HM::DistributionList::LMPublic:
         *pVal = eLMPublic;
         break;
      case HM::DistributionList::LMMembership:
         *pVal = eLMMembership;
         break;
      case HM::DistributionList::LMAnnouncement:
         *pVal = eLMAnnouncement;
         break;
      case HM::DistributionList::LMDomainMembers:
         *pVal = eLMDomainMembers;
         break;
      default:
         *pVal = eLMPublic;
         break;
      }
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDistributionList::put_Mode(eDistributionListMode newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      /*
         The seed value used to be LMPublic and the switch had no default, so a
         value this server does not implement fell straight through and stored
         "anyone may send" - the single most permissive mode there is.

         That is not a theoretical hole. The IDL declares eLMServerMembers = 4
         ("anyone with an account on this server"), which nothing in the server
         implements: DistributionList::ListMode stops at LMDomainMembers = 3 and
         RecipientParser::UserCanSendToList_ has no branch for a fifth mode. So
         a caller selecting the more restrictive-sounding of the two "anyone..."
         options - to keep outsiders off a list - silently got a list that
         accepts mail from anywhere on the internet. get_Mode's own default then
         reported it back as "Public", so the only visible symptom was a
         selection that appeared not to have stuck.

         Failing closed is not the answer either: silently substituting the most
         restrictive mode would leave the caller equally misinformed, just in the
         other direction. The value is refused, with a message naming what was
         asked for, so the caller learns that the mode does not exist. This is
         the same correction put_AdminLevel needed for the same reason.
      */
      HM::DistributionList::ListMode iMode;

      switch (newVal)
      {
      case eLMPublic:
         iMode = HM::DistributionList::LMPublic;
         break;
      case eLMMembership:
         iMode = HM::DistributionList::LMMembership;
         break;
      case eLMAnnouncement:
         iMode = HM::DistributionList::LMAnnouncement;
         break;
      case eLMDomainMembers:
         iMode = HM::DistributionList::LMDomainMembers;
         break;
      default:
         // HM::Formatter::Format, spelled the way InterfaceScripting.cpp spells it -
         // the COM sources reach it through the precompiled header rather than a
         // direct include.
         return COMError::GenerateError(
            HM::Formatter::Format("The distribution list mode {0} is not supported by this server. "
                                  "Valid modes are 0 (Public), 1 (Membership), 2 (Announcement) and "
                                  "3 (Domain members). The list has not been changed.", (int) newVal));
      }

      object_->SetListMode(iMode);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

// ---- the list manager (Util/ListManager.h) ------------------------------------

STDMETHODIMP InterfaceDistributionList::get_Managed(VARIANT_BOOL *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetManaged() ? VARIANT_TRUE : VARIANT_FALSE;

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDistributionList::put_Managed(VARIANT_BOOL newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      object_->SetManaged(newVal == VARIANT_TRUE);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDistributionList::get_SubscribePolicy(long *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = (long) object_->GetSubscribePolicy();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDistributionList::put_SubscribePolicy(long newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      // Refused at the assignment, in the sentence the save would give, so a
      // script learns of the value where it wrote it.
      if (newVal < 0 || newVal > 2)
         return COMError::GenerateError("The subscribe policy must be 0 (closed), 1 (confirm) or 2 (moderated).");

      object_->SetSubscribePolicy((int) newVal);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDistributionList::get_Archive(long *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = (long) object_->GetArchive();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDistributionList::put_Archive(long newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      // Refused at the assignment, in the sentence the save would give, so a
      // script learns of the value where it wrote it.
      if (newVal < 0 || newVal > 2)
         return COMError::GenerateError("The archive must be 0 (none), 1 (members) or 2 (public).");

      object_->SetArchive((int) newVal);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDistributionList::get_ArchiveDays(long *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = (long) object_->GetArchiveDays();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDistributionList::put_ArchiveDays(long newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      // Refused at the assignment, in the sentence the save would give, so a
      // script learns of the value where it wrote it.
      if (newVal < 0 || newVal > 3650)
         return COMError::GenerateError("The archive's days must be between 0 (for ever) and 3650.");

      object_->SetArchiveDays((int) newVal);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDistributionList::get_Digest(VARIANT_BOOL *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetDigest() ? VARIANT_TRUE : VARIANT_FALSE;

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDistributionList::put_Digest(VARIANT_BOOL newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      object_->SetDigest(newVal == VARIANT_TRUE);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDistributionList::get_DigestHour(long *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = (long) object_->GetDigestHour();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDistributionList::put_DigestHour(long newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      // Refused at the assignment, in the sentence the save would give, so a
      // script learns of the value where it wrote it.
      if (newVal < 0 || newVal > 23)
         return COMError::GenerateError("The digest hour must be between 0 and 23.");

      object_->SetDigestHour((int) newVal);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDistributionList::get_ReplyToList(VARIANT_BOOL *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetReplyToList() ? VARIANT_TRUE : VARIANT_FALSE;

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDistributionList::put_ReplyToList(VARIANT_BOOL newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      object_->SetReplyToList(newVal == VARIANT_TRUE);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDistributionList::get_SubjectPrefix(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      HM::String value = object_->GetSubjectPrefix();
      *pVal = value.AllocSysString();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDistributionList::put_SubjectPrefix(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      HM::String value = newVal;
      value.Trim();
      if (value.GetLength() > HM::DistributionList::MaxSubjectPrefixLength)
         return COMError::GenerateError("The subject prefix is at most 64 characters.");

      object_->SetSubjectPrefix(value);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDistributionList::get_Footer(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      HM::String value = object_->GetFooter();
      *pVal = value.AllocSysString();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDistributionList::put_Footer(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      HM::String value = newVal;
      if (value.GetLength() > HM::DistributionList::MaxFooterLength)
         return COMError::GenerateError("The footer is at most 2000 characters.");

      object_->SetFooter(value);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDistributionList::get_Verp(VARIANT_BOOL *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetVerp() ? VARIANT_TRUE : VARIANT_FALSE;

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDistributionList::put_Verp(VARIANT_BOOL newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      object_->SetVerp(newVal == VARIANT_TRUE);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDistributionList::get_Owner(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      HM::String value = object_->GetOwner();
      *pVal = value.AllocSysString();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDistributionList::put_Owner(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      HM::String value = newVal;
      value.Trim();

      object_->SetOwner(value);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDistributionList::SendDigest(long *pVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      // A forced digest: the hour and the once-a-day rule are the scheduler's,
      // and an administrator - or a test - pressing "send it now" is not asking
      // to wait for either. An empty digest is still not sent. Read from the
      // database rather than from this object, which may be older than the
      // last digest the task sent.
      std::shared_ptr<HM::DistributionList> current = HM::ListManager::ListById(object_->GetID());
      *pVal = current ? (long) HM::ListArchive::SendDigest(current, true) : 0;

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}
