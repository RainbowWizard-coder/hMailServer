// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"

#include "InterfaceDomain.h"
#include "../Common/Util/CountryBlocking.h"
#include "../Common/Util/HttpsClient.h"
#include "InterfaceDomain.h"
#include "InterfaceAccounts.h"
#include "InterfaceRules.h"
#include "../Common/BO/Rules.h"
#include "InterfaceAliases.h"
#include "InterfaceDomainAliases.h"
#include "InterfaceDistributionLists.h"

#include "../Common/BO/Accounts.h"
#include "../Common/BO/DistributionLists.h"
#include "../Common/BO/Domain.h"
#include "../Common/BO/DomainAliases.h"
#include "../Common/BO/Domains.h"

#include "../Common/Persistence/PersistentDomain.h"
#include "../Common/Persistence/PersistenceMode.h"

#include "InterfaceDomains.h"

#include "COMAuthentication.h"

#include "COMError.h"
#include "InterfaceSenderListEntries.h"
#include "../Common/BO/SenderListEntries.h"
#include "../Common/BO/SenderListEntry.h"
#include "../Common/AntiSpam/DKIM/DkimRotation.h"
#include "../Common/AntiSpam/SpamTrapMonitor.h"
#include "../Common/Cache/CacheContainer.h"
#include "../Common/Util/Unicode.h"
#include "../Common/Util/RestrictedDelivery.h"
#include "../Common/Util/ImpersonationCheck.h"
#include "../Common/Util/DiscoveryHolds.h"
#include "../Common/Application/EffectiveSetting.h"
#include "../Common/Persistence/PersistentSettingOverride.h"

#include <ctime>


#ifdef _DEBUG
long InterfaceDomain::counter = 0;
#endif


STDMETHODIMP InterfaceDomain::InterfaceSupportsErrorInfo(REFIID riid)
{
   static const IID* arr[] = 
   {
      &IID_IInterfaceDomain,
   };

   for (int i=0;i<sizeof(arr)/sizeof(arr[0]);i++)
   {
      if (InlineIsEqualGUID(*arr[i],riid))
         return S_OK;
   }
   return S_FALSE;
}
   

void
InterfaceDomain::SetAuthentication(std::shared_ptr<HM::COMAuthentication> pAuthentication)
{
   authentication_ = pAuthentication;
}

STDMETHODIMP InterfaceDomain::get_ID(long *pVal)
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

STDMETHODIMP InterfaceDomain::get_Name(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetName().AllocSysString();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::put_Name(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      // The server administrator's, not a domain administrator's: a rename makes
      // the domain accept mail for a different name, and which names the server
      // answers for is the server's to decide, as which domains exist is. A
      // domain administrator administers what is inside its domain and keeps
      // every other setting on it; it does not rename it.
      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();

      HM::String sName = newVal;
      sName.Trim();

      object_->SetName(sName);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_Postmaster(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();
   
      *pVal = object_->GetPostmaster().AllocSysString();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::put_Postmaster(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

     
      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();
   
      object_->SetPostmaster(newVal);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_ADDomainName(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();
   
      *pVal = object_->GetADDomainName().AllocSysString();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::put_ADDomainName(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();
   
      // Set the active directory domain name
      object_->SetADDomainName(newVal);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::Save()
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      // The relay host a credential that is not the server administrator sets is
      // held to a public address, as a reader's fetch host is: delivery connects
      // to it with no address rule (SMTP/ServerTargetResolver), so an internal
      // name or address here would be a way for a domain administrator to have
      // the server reach its own network. The server administrator may still set
      // an internal smart host. Judged only when a host is set, so clearing it,
      // or saving a domain that has none, is never refused on it.
      if (!authentication_->GetIsServerAdmin())
      {
         const HM::String relayHost = object_->GetRelayHost();
         HM::String relayRefusal;
         if (!relayHost.IsEmpty() && !HM::HttpsClient::HostIsPublic(HM::AnsiString(relayHost), relayRefusal))
            return COMError::GenerateError(L"The relay host must be a public address: " + relayRefusal);
      }

      // Save object in the database.
      HM::String sErrorMessage;
      if (HM::PersistentDomain::SaveObject(object_, sErrorMessage, HM::PersistenceModeNormal))
      {
         // Add to parent collection
         AddToParentCollection();
         return S_OK;
      }
   
      return COMError::GenerateError("Failed to save object. " + sErrorMessage);
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_Active(VARIANT_BOOL *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();
   
      if (object_->GetIsActive())
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

STDMETHODIMP InterfaceDomain::put_Active(VARIANT_BOOL newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();
   
      if (newVal == VARIANT_TRUE)
         object_->SetIsActive(true);
      else
         object_->SetIsActive(false);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_PlusAddressingEnabled(VARIANT_BOOL *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();
      
      *pVal  = object_->GetUsePlusAddressing() ? VARIANT_TRUE : VARIANT_FALSE;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::put_PlusAddressingEnabled(VARIANT_BOOL newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();
   
      // A gate (docs/SettingLevels.md): every local+tag address of every mailbox
      // of the domain, opened at once. The server administrator's, as REST's
      // domain route has always held it; a domain administrator writing back the
      // value the domain already has - a dialog saving every field - is not
      // refused, since nothing changes.
      if (!authentication_->GetIsServerAdmin() && object_->GetUsePlusAddressing() != (newVal == VARIANT_TRUE))
         return COMError::GenerateError("Plus addressing is the server administrator's to switch for a domain.");

      object_->SetUsePlusAddressing(newVal == VARIANT_TRUE);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_Accounts(IInterfaceAccounts **pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      CComObject<InterfaceAccounts>* pItem = new CComObject<InterfaceAccounts>();
      pItem->SetAuthentication(authentication_);
   
      std::shared_ptr<HM::Accounts> pAccounts;
   
      if (authentication_->GetIsDomainAdmin())
         pAccounts = object_->GetAccounts();
      else
         pAccounts = object_->GetAccounts(authentication_->GetAccountID());
   
      if (pAccounts)
      {
         pItem->SetAuthentication(authentication_);
         pItem->Attach(pAccounts);
         pItem->SetDomain(object_->GetID());
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

STDMETHODIMP InterfaceDomain::Delete()
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();
   
      // A legal hold on the domain or an account in it forbids it, and the
      // caller is told why (docs/Discovery.md).
      if (HM::DiscoveryHolds::IsDomainOrAnyAccountHeld(object_->GetID()))
         return COMError::GenerateError(HM::DiscoveryHolds::DeleteDomainRefusal(object_->GetName()));

      if (!parent_collection_)
         return HM::PersistentDomain::DeleteObject(object_) ? S_OK : S_FALSE;
   
      parent_collection_->DeleteItemByDBID(object_->GetID());
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::SynchronizeDirectory()
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_Aliases(IInterfaceAliases **pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();
   
      CComObject<InterfaceAliases>* pItem = new CComObject<InterfaceAliases>();
      pItem->SetAuthentication(authentication_);
   
      std::shared_ptr<HM::Aliases> pAliases = object_->GetAliases();
      pAliases->Refresh();
   
      if (pAliases)
      {
         pItem->Attach(pAliases);
         pItem->SetDomain(object_->GetID());
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

STDMETHODIMP InterfaceDomain::get_DomainAliases(IInterfaceDomainAliases **pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();
   
      CComObject<InterfaceDomainAliases>* pItem = new CComObject<InterfaceDomainAliases>();
      pItem->SetAuthentication(authentication_);
   
      std::shared_ptr<HM::DomainAliases> pDA = std::shared_ptr<HM::DomainAliases>(new HM::DomainAliases(object_->GetID()));
   
      if (pDA)
      {
         // Refresh the collection from the database.
         pDA->Refresh();
   
         pItem->Attach(pDA);
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

STDMETHODIMP InterfaceDomain::get_DistributionLists(IInterfaceDistributionLists **pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();
   
      CComObject<InterfaceDistributionLists>* pItem = new CComObject<InterfaceDistributionLists>();
      pItem->SetAuthentication(authentication_);
   
      std::shared_ptr<HM::DistributionLists> pDistLists = object_->GetDistributionLists();
      pDistLists->Refresh();
   
      if (pDistLists)
      {
         pItem->Attach(pDistLists);
         pItem->SetDomain(object_->GetID());
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

STDMETHODIMP InterfaceDomain::get_MaxMessageSize(long *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();
   
      *pVal = object_->GetMaxMessageSize();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::put_MaxMessageSize(long pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();
   
      object_->SetMaxMessageSize(pVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_MessageRetentionDays(long *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetMessageRetentionDays();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::put_MessageRetentionDays(long pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      // A policy that deletes users' mail is the server administrator's to set, like
      // the domain's size limit.
      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();

      if (pVal < 0)
         return COMError::GenerateError("MessageRetentionDays must be 0 (no policy) or a number of days.");

      object_->SetMessageRetentionDays(pVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_MaxSize(long *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();
   
      *pVal = object_->GetMaxSizeMB();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::put_MaxSize(long pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();
   
      object_->SetMaxSizeMB(pVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_Size(long *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();
   
      *pVal = HM::PersistentDomain::GetSize(object_);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_AllocatedSize(hyper  *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();
   
      *pVal = HM::PersistentDomain::GetAllocatedSize(object_);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_PlusAddressingCharacter(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();
   
      *pVal = object_->GetPlusAddressingChar().AllocSysString();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::put_PlusAddressingCharacter(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();
   
      object_->SetPlusAddressingChar(newVal);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_AntiSpamEnableGreylisting(VARIANT_BOOL *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();
   
      *pVal  = object_->GetASUseGreyListing() ? VARIANT_TRUE : VARIANT_FALSE;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::put_AntiSpamEnableGreylisting(VARIANT_BOOL newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();
   
      object_->SetASUseGreyListing(newVal == VARIANT_TRUE);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_SignatureEnabled(VARIANT_BOOL *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();
      *pVal  = object_->GetEnableSignature() ? VARIANT_TRUE : VARIANT_FALSE;
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::put_SignatureEnabled(VARIANT_BOOL newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();
      object_->SetEnableSignature(newVal == VARIANT_TRUE);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_SignatureMethod(eDomainSignatureMethod *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();
      *pVal  = (eDomainSignatureMethod) object_->GetSignatureMethod();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::put_SignatureMethod(eDomainSignatureMethod newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();
      object_->SetSignatureMethod((HM::Domain::DomainSignatureMethod) newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_SignaturePlainText(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();
      *pVal  = object_->GetSignaturePlainText().AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::put_SignaturePlainText(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();
      object_->SetSignaturePlainText(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_SignatureHTML(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();
      *pVal  = object_->GetSignatureHTML().AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::put_SignatureHTML(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();
      object_->SetSignatureHTML(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_AddSignaturesToReplies(VARIANT_BOOL *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();
   
      *pVal = object_->GetAddSignaturesToReplies() ? VARIANT_TRUE : VARIANT_FALSE;
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::put_AddSignaturesToReplies(VARIANT_BOOL newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();
   
      object_->SetAddSignaturesToReplies(newVal == VARIANT_TRUE);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_AddSignaturesToLocalMail(VARIANT_BOOL *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();
   
      *pVal = object_->GetAddSignaturesToLocalMail() ? VARIANT_TRUE : VARIANT_FALSE;
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::put_AddSignaturesToLocalMail(VARIANT_BOOL newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();
   
      object_->SetAddSignaturesToLocalMail(newVal == VARIANT_TRUE);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_MaxNumberOfAccounts(long *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();;
   
      *pVal = object_->GetMaxNoOfAccounts();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::put_MaxNumberOfAccounts(long pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();
   
      object_->SetMaxNoOfAccounts(pVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_MaxNumberOfAliases(long *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();
   
      *pVal = object_->GetMaxNoOfAliases();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::put_MaxNumberOfAliases(long pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();
   
      object_->SetMaxNoOfAliases(pVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_MaxNumberOfDistributionLists(long *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();
   
      *pVal = object_->GetMaxNoOfDistributionLists();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::put_MaxNumberOfDistributionLists(long pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();
   
      object_->SetMaxNoOfDistributionLists(pVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_MaxAccountSize(long *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();
   
      *pVal = object_->GetMaxAccountSize();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::put_MaxAccountSize(long pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();
   
      object_->SetMaxAccountSize(pVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_MaxNumberOfAccountsEnabled(VARIANT_BOOL *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();;
   
      *pVal = object_->GetMaxNoOfAccountsEnabled() ? VARIANT_TRUE : VARIANT_FALSE;
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::put_MaxNumberOfAccountsEnabled(VARIANT_BOOL pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();
   
      object_->SetMaxNoOfAccountsEnabled(pVal == VARIANT_TRUE);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_MaxNumberOfAliasesEnabled(VARIANT_BOOL *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();
   
      *pVal = object_->GetMaxNoOfAliasesEnabled() ? VARIANT_TRUE : VARIANT_FALSE;
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::put_MaxNumberOfAliasesEnabled(VARIANT_BOOL pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();
   
      object_->SetMaxNoOfAliasesEnabled(pVal == VARIANT_TRUE);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_MaxNumberOfDistributionListsEnabled(VARIANT_BOOL *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();
   
      *pVal = object_->GetMaxNoOfDistributionListsEnabled() ? VARIANT_TRUE : VARIANT_FALSE;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::put_MaxNumberOfDistributionListsEnabled(VARIANT_BOOL pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();
   
      object_->SetMaxNoOfDistributionListsEnabled(pVal == VARIANT_TRUE);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_DKIMSignEnabled(VARIANT_BOOL *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();
   
      *pVal  = object_->GetDKIMEnabled() ? VARIANT_TRUE : VARIANT_FALSE;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::put_DKIMSignEnabled(VARIANT_BOOL newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();
   
      object_->SetDKIMEnabled(newVal == VARIANT_TRUE);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_DKIMSignAliasesEnabled(VARIANT_BOOL *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      *pVal = object_->GetDKIMAliasesEnabled() ? VARIANT_TRUE : VARIANT_FALSE;

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::put_DKIMSignAliasesEnabled(VARIANT_BOOL newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      object_->SetDKIMAliasesEnabled(newVal == VARIANT_TRUE);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_DKIMSelector(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();
   
      *pVal = object_->GetDKIMSelector().AllocSysString();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::put_DKIMSelector(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();
   
      object_->SetDKIMSelector(newVal);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_DKIMPrivateKeyFile(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();
   
      *pVal = object_->GetDKIMPrivateKeyFile().AllocSysString();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::put_DKIMPrivateKeyFile(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();
   
      object_->SetDKIMPrivateKeyFile(newVal);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_DKIMHeaderCanonicalizationMethod(eDKIMCanonicalizationMethod *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();
   
      *pVal = (eDKIMCanonicalizationMethod)object_->GetDKIMHeaderCanonicalizationMethod();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::put_DKIMHeaderCanonicalizationMethod(eDKIMCanonicalizationMethod newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();
   
      object_->SetDKIMHeaderCanonicalizationMethod(newVal);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_DKIMBodyCanonicalizationMethod(eDKIMCanonicalizationMethod *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();
   
      *pVal = (eDKIMCanonicalizationMethod)object_->GetDKIMBodyCanonicalizationMethod();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::put_DKIMBodyCanonicalizationMethod(eDKIMCanonicalizationMethod newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();
   
      object_->SetDKIMBodyCanonicalizationMethod(newVal);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_DKIMSigningAlgorithm(eDKIMAlgorithm *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();
   
      *pVal = (eDKIMAlgorithm) object_->GetDKIMSigningAlgorithm();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::put_DKIMSigningAlgorithm(eDKIMAlgorithm newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      object_->SetDKIMSigningAlgorithm(newVal);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_DKIMSecondarySelector(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      *pVal = object_->GetDKIMSecondarySelector().AllocSysString();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::put_DKIMSecondarySelector(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      object_->SetDKIMSecondarySelector(newVal);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_DKIMSecondaryPrivateKeyFile(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      *pVal = object_->GetDKIMSecondaryPrivateKeyFile().AllocSysString();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::put_DKIMSecondaryPrivateKeyFile(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      object_->SetDKIMSecondaryPrivateKeyFile(newVal);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::DKIMPromoteSecondary()
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      // Only the in-memory object changes here, matching every other mutator
      // on this interface: the caller decides when to Save(), and a promote it
      // then thinks better of is discarded along with any other unsaved edit
      // rather than half-persisted.
      HM::String sErrorMessage;
      if (!object_->PromoteDKIMSecondary(sErrorMessage))
         return COMError::GenerateError(sErrorMessage);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

// The domain's own outbound relay - see IInterfaceDomain in the IDL. Every one of
// these is domain-admin level, matching the DKIM settings above it: a domain
// administrator who can already choose the domain's signing key can choose where
// its mail leaves through.

STDMETHODIMP InterfaceDomain::get_VacationMessageIsOn(VARIANT_BOOL *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      *pVal = object_->GetVacationMessageIsOn() ? VARIANT_TRUE : VARIANT_FALSE;

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::put_VacationMessageIsOn(VARIANT_BOOL newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      object_->SetVacationMessageIsOn(newVal == VARIANT_TRUE);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_VacationSubject(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      *pVal = object_->GetVacationSubject().AllocSysString();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::put_VacationSubject(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      object_->SetVacationSubject(newVal);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_VacationMessage(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      *pVal = object_->GetVacationMessage().AllocSysString();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::put_VacationMessage(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      object_->SetVacationMessage(newVal);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_VacationInternalSubject(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      *pVal = object_->GetVacationInternalSubject().AllocSysString();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::put_VacationInternalSubject(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      object_->SetVacationInternalSubject(newVal);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_VacationInternalMessage(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      *pVal = object_->GetVacationInternalMessage().AllocSysString();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::put_VacationInternalMessage(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      object_->SetVacationInternalMessage(newVal);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_VacationExternalOverride(VARIANT_BOOL *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      *pVal = object_->GetVacationExternalOverride() ? VARIANT_TRUE : VARIANT_FALSE;

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::put_VacationExternalOverride(VARIANT_BOOL newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      object_->SetVacationExternalOverride(newVal == VARIANT_TRUE);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_VacationDays(long *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      *pVal = object_->GetVacationDays();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::put_VacationDays(long newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      if (newVal < 0 || newVal > 3650)
         return COMError::GenerateError("VacationDays must be between 0 and 3650.");

      object_->SetVacationDays((int) newVal);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_RelayHost(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      *pVal = object_->GetRelayHost().AllocSysString();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::put_RelayHost(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      object_->SetRelayHost(newVal);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_RelayPort(long *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      *pVal = object_->GetRelayPort();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::put_RelayPort(long newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      object_->SetRelayPort(newVal);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_RelayRequiresAuthentication(VARIANT_BOOL *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      *pVal = object_->GetRelayRequiresAuth() ? VARIANT_TRUE : VARIANT_FALSE;

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::put_RelayRequiresAuthentication(VARIANT_BOOL newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      object_->SetRelayRequiresAuth(newVal == VARIANT_TRUE);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_RelayUsername(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      *pVal = object_->GetRelayUsername().AllocSysString();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::put_RelayUsername(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      object_->SetRelayUsername(newVal);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_RelayPassword(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      *pVal = object_->GetRelayPassword().AllocSysString();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::put_RelayPassword(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      object_->SetRelayPassword(newVal);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_RelayConnectionSecurity(eConnectionSecurity *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      *pVal = (eConnectionSecurity) object_->GetRelayConnectionSecurity();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::put_RelayConnectionSecurity(eConnectionSecurity newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      object_->SetRelayConnectionSecurity((HM::ConnectionSecurity) newVal);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_ExternalTagSubject(VARIANT_BOOL *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      *pVal = object_->GetExternalTagSubject() ? VARIANT_TRUE : VARIANT_FALSE;

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::put_ExternalTagSubject(VARIANT_BOOL newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      object_->SetExternalTagSubject(newVal == VARIANT_TRUE);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_ExternalTagHeader(VARIANT_BOOL *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      *pVal = object_->GetExternalTagHeader() ? VARIANT_TRUE : VARIANT_FALSE;

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::put_ExternalTagHeader(VARIANT_BOOL newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      object_->SetExternalTagHeader(newVal == VARIANT_TRUE);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_ExternalTagText(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      *pVal = object_->GetExternalTagText().AllocSysString();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::put_ExternalTagText(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      // The column holds a hundred characters. Refused rather than truncated,
      // because a tag cut off in the middle is a tag every reader learns to
      // ignore.
      HM::String tagText = newVal;
      if (tagText.GetLength() > 100)
         return COMError::GenerateError("ExternalTagText must be 100 characters or fewer.");

      object_->SetExternalTagText(newVal);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_FirstContactTip(VARIANT_BOOL *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      *pVal = object_->GetFirstContactTip() ? VARIANT_TRUE : VARIANT_FALSE;

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::put_FirstContactTip(VARIANT_BOOL newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      object_->SetFirstContactTip(newVal == VARIANT_TRUE);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_DisclaimerEnabled(VARIANT_BOOL *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      *pVal = object_->GetDisclaimerEnabled() ? VARIANT_TRUE : VARIANT_FALSE;

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::put_DisclaimerEnabled(VARIANT_BOOL newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      object_->SetDisclaimerEnabled(newVal == VARIANT_TRUE);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_DisclaimerPlainText(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      *pVal = object_->GetDisclaimerPlainText().AllocSysString();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::put_DisclaimerPlainText(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      object_->SetDisclaimerPlainText(newVal);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_DisclaimerHTML(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      *pVal = object_->GetDisclaimerHTML().AllocSysString();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::put_DisclaimerHTML(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      object_->SetDisclaimerHTML(newVal);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}


STDMETHODIMP InterfaceDomain::get_SenderList(IInterfaceSenderListEntries **pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      // A fresh collection scoped to this domain. Nothing holds it: the server
      // reads the list through SenderListCache, out of its own copy of the whole
      // table, which every save and delete invalidates.
      std::shared_ptr<HM::SenderListEntries> entries =
         std::shared_ptr<HM::SenderListEntries>(new HM::SenderListEntries(HM::SenderListEntry::ScopeDomain, object_->GetID()));

      entries->Refresh();

      CComObject<InterfaceSenderListEntries>* pInterface = new CComObject<InterfaceSenderListEntries>();
      pInterface->SetAuthentication(authentication_);
      pInterface->Attach(entries);
      pInterface->AddRef();

      *pVal = pInterface;

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_QuarantineDigest(VARIANT_BOOL *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      *pVal = object_->GetQuarantineDigest() ? VARIANT_TRUE : VARIANT_FALSE;

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::put_QuarantineDigest(VARIANT_BOOL newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      object_->SetQuarantineDigest(newVal == VARIANT_TRUE);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_CountryPolicy(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      *pVal = object_->GetCountryPolicy().AllocSysString();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::put_CountryPolicy(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      // The same grammar, the same refusal sentence and the same canonical form
      // as Settings.CountryPolicy: one concept, spelled one way everywhere.
      HM::String canonical;
      HM::String error;

      if (!HM::CountryBlocking::Canonicalise(HM::String(newVal), canonical, error))
         return COMError::GenerateError(error);

      object_->SetCountryPolicy(canonical);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_QuarantineDigestHour(long *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      *pVal = (long) object_->GetQuarantineDigestHour();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::put_QuarantineDigestHour(long newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      // Refused rather than clamped. An hour of 25 is a mistake somebody made,
      // and silently turning it into 23 would send the digests at a time they
      // never chose and then show them the 23 as though they had.
      if (newVal < 0 || newVal > 23)
         return COMError::GenerateError("The digest hour is a whole number from 0 to 23.");

      object_->SetQuarantineDigestHour((int) newVal);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

// ---------------------------------------------------------------------------
// DKIM keys that rotate.
//
// The five policy properties are ordinary domain properties: set in memory,
// validated here, persisted by Save. The six members below them act at once -
// unlike DKIMPromoteSecondary - because a rotation is the server's to carry out
// and its state is not the object's, and each one reads the domain's four key
// columns back into this object afterwards, so that a Save on the same object
// later cannot write the selectors it had before the operation moved them.
// Domain-administrator level, like every other DKIM member of this interface.
// ---------------------------------------------------------------------------

namespace
{
   void RefreshDkimKeys_(std::shared_ptr<HM::Domain> object)
   {
      std::shared_ptr<HM::Domain> stored = std::shared_ptr<HM::Domain>(new HM::Domain());
      if (!HM::PersistentDomain::ReadObject(stored, object->GetID()))
         return;

      object->SetDKIMSelector(HM::String(stored->GetDKIMSelector()));
      object->SetDKIMPrivateKeyFile(stored->GetDKIMPrivateKeyFile());
      object->SetDKIMSecondarySelector(HM::String(stored->GetDKIMSecondarySelector()));
      object->SetDKIMSecondaryPrivateKeyFile(stored->GetDKIMSecondaryPrivateKeyFile());
   }

   HRESULT DkimRotationAnswer_(const HM::DkimRotation::Result &result, BSTR *out)
   {
      if (!result.Succeeded())
         return COMError::GenerateError(result.error);

      // The document is UTF-8, as REST sends it; a domain or a path outside the
      // ANSI code page must reach a COM client as the characters it is.
      HM::String json;
      HM::Unicode::MultiByteToWide(result.json, json);
      *out = json.AllocSysString();
      return S_OK;
   }
}

// The domain's half of the Web Key Directory (docs/OpenPGP.md). Saved with the
// domain, like every other property here; a domain administrator may set it for
// their own domain, which is the same rule the DKIM rotation policy follows.

STDMETHODIMP InterfaceDomain::get_OpenPGPPublishKeys(VARIANT_BOOL *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      *pVal = object_->GetOpenPGPPublish() ? VARIANT_TRUE : VARIANT_FALSE;
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::put_OpenPGPPublishKeys(VARIANT_BOOL newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      object_->SetOpenPGPPublish(newVal == VARIANT_TRUE);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_OpenPGPPublishNewAccounts(VARIANT_BOOL *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      *pVal = object_->GetOpenPGPPublishNewAccounts() ? VARIANT_TRUE : VARIANT_FALSE;
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::put_OpenPGPPublishNewAccounts(VARIANT_BOOL newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      // Only what an account CREATED from now on starts with. Nothing here
      // touches an account that already exists, which is what keeps an
      // administrator's change of a default from publishing anybody's key.
      object_->SetOpenPGPPublishNewAccounts(newVal == VARIANT_TRUE);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_DKIMRotationEnabled(VARIANT_BOOL *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      *pVal = object_->GetDKIMRotationEnabled() ? VARIANT_TRUE : VARIANT_FALSE;
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::put_DKIMRotationEnabled(VARIANT_BOOL newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      object_->SetDKIMRotationEnabled(newVal == VARIANT_TRUE);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_DKIMRotationIntervalDays(long *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      *pVal = object_->GetDKIMRotationIntervalDays();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::put_DKIMRotationIntervalDays(long newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      // Refused rather than clamped, as every period here is: a value quietly
      // changed on the way in is a schedule nobody chose.
      if (newVal < 1 || newVal > 3650)
         return COMError::GenerateError("DKIMRotationIntervalDays must be between 1 and 3650.");

      object_->SetDKIMRotationIntervalDays((int) newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_DKIMRotationPropagationHours(long *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      *pVal = object_->GetDKIMRotationPropagationHours();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::put_DKIMRotationPropagationHours(long newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      if (newVal < 1 || newVal > 720)
         return COMError::GenerateError("DKIMRotationPropagationHours must be between 1 and 720.");

      object_->SetDKIMRotationPropagationHours((int) newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_DKIMRotationRetentionDays(long *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      *pVal = object_->GetDKIMRotationRetentionDays();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::put_DKIMRotationRetentionDays(long newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      if (newVal < 1 || newVal > 365)
         return COMError::GenerateError("DKIMRotationRetentionDays must be between 1 and 365.");

      object_->SetDKIMRotationRetentionDays((int) newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_DKIMRotationKeyBits(long *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      *pVal = object_->GetDKIMRotationKeyBits();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::put_DKIMRotationKeyBits(long newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      if (newVal != 2048 && newVal != 3072 && newVal != 4096)
         return COMError::GenerateError("DKIMRotationKeyBits must be 2048, 3072 or 4096.");

      object_->SetDKIMRotationKeyBits((int) newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_DKIMRotationStatus(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      if (object_->GetID() <= 0)
         return COMError::GenerateError("Save the domain before asking for its DKIM key rotation.");

      return DkimRotationAnswer_(HM::DkimRotation::Status(object_->GetID(), (__int64) time(nullptr)), pVal);
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::DKIMRotationStart(BSTR Selector, BSTR *Status)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      if (object_->GetID() <= 0)
         return COMError::GenerateError("Save the domain before rotating its DKIM key.");

      HM::String selector = Selector;
      HM::DkimRotation::Result result = HM::DkimRotation::Start(object_->GetID(), HM::AnsiString(selector), (__int64) time(nullptr));
      RefreshDkimKeys_(object_);
      return DkimRotationAnswer_(result, Status);
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::DKIMRotationCheck(BSTR *Status)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      if (object_->GetID() <= 0)
         return COMError::GenerateError("Save the domain before checking its DKIM records.");

      HM::DkimRotation::Result result = HM::DkimRotation::CheckNow(object_->GetID(), (__int64) time(nullptr));
      RefreshDkimKeys_(object_);
      return DkimRotationAnswer_(result, Status);
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::DKIMRotationSwitch(VARIANT_BOOL Force, BSTR *Status)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      if (object_->GetID() <= 0)
         return COMError::GenerateError("Save the domain before switching its DKIM key.");

      HM::DkimRotation::Result result = HM::DkimRotation::Switch(object_->GetID(), Force == VARIANT_TRUE, (__int64) time(nullptr));
      RefreshDkimKeys_(object_);
      return DkimRotationAnswer_(result, Status);
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::DKIMRotationRetire(BSTR *Status)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      if (object_->GetID() <= 0)
         return COMError::GenerateError("Save the domain before retiring its DKIM key.");

      HM::DkimRotation::Result result = HM::DkimRotation::Retire(object_->GetID(), (__int64) time(nullptr));
      RefreshDkimKeys_(object_);
      return DkimRotationAnswer_(result, Status);
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::DKIMRotationCancel(BSTR *Status)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      if (object_->GetID() <= 0)
         return COMError::GenerateError("Save the domain before cancelling its DKIM key rotation.");

      HM::DkimRotation::Result result = HM::DkimRotation::Cancel(object_->GetID(), (__int64) time(nullptr));
      RefreshDkimKeys_(object_);
      return DkimRotationAnswer_(result, Status);
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_WebhooksEnabled(VARIANT_BOOL *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      *pVal = object_->GetWebhooksEnabled() ? VARIANT_TRUE : VARIANT_FALSE;
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::put_WebhooksEnabled(VARIANT_BOOL newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      object_->SetWebhooksEnabled(newVal == VARIANT_TRUE);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_WebhookSubjects(VARIANT_BOOL *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      *pVal = object_->GetWebhookSubjects() ? VARIANT_TRUE : VARIANT_FALSE;
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::put_WebhookSubjects(VARIANT_BOOL newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      object_->SetWebhookSubjects(newVal == VARIANT_TRUE);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_ReportMailbox(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      *pVal = object_->GetReportMailbox().AllocSysString();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::put_ReportMailbox(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      // The same rule, in the same sentence, as PUT /api/v1/domains/{domain}
      // applies: empty, or one address in a domain this server hosts - this
      // one, under the name it is being saved with, or another. A provider
      // delivers a report to the address the rua= tag names, so an address
      // somewhere else would read no report and never say so. Whether an
      // account or alias answers at it is the administrator's to arrange; the
      // domain may be new and its mailboxes not yet made.
      HM::String mailbox = newVal;
      mailbox.Trim();
      mailbox.ToLower();

      if (!mailbox.IsEmpty())
      {
         const HM::String mailboxDomain = HM::StringParser::ExtractDomain(mailbox);
         const bool own = !mailboxDomain.IsEmpty() && mailboxDomain.CompareNoCase(object_->GetName()) == 0;
         const bool hosted = own || (!mailboxDomain.IsEmpty() && HM::CacheContainer::Instance()->GetDomain(mailboxDomain));

         if (!HM::StringParser::IsValidEmailAddress(mailbox) || !hosted)
            return COMError::GenerateError("ReportMailbox is one address in a domain this server hosts, or empty; the DMARC and TLS-RPT reports are delivered to it.");

         // A mailbox in ANOTHER hosted domain makes this server open every
         // attachment delivered to that address alone, so a credential that
         // reaches a list of domains rather than every one may name a mailbox
         // only in its own domain; the cross-domain form is the server
         // administrator's, as it is over REST.
         if (!own && !authentication_->ReachesEveryDomain())
            return COMError::GenerateError("ReportMailbox names an address in another domain; a domain administrator may name a mailbox only in the domain it administers.");
      }

      object_->SetReportMailbox(mailbox);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_RestrictedDomains(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      *pVal = object_->GetRestrictedDomains().AllocSysString();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::put_RestrictedDomains(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      // The same rule, in the same sentence, as PUT /api/v1/domains/{domain}
      // applies: the list is canonicalised - trimmed, lower-cased, the names
      // separated by ", " - and refused with the reason when a name is not a
      // domain, the text is not ASCII or it is longer than 2,000 characters.
      HM::String canonical;
      HM::String refusal;

      if (!HM::RestrictedDelivery::CanonicaliseList(HM::String(newVal), canonical, refusal))
         return COMError::GenerateError(refusal);

      object_->SetRestrictedDomains(canonical);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_ImpersonationAction(long *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      *pVal = object_->GetImpersonationAction();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::put_ImpersonationAction(long newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      // The same three values PUT /api/v1/domains/{domain} takes, refused in
      // the same sentence.
      if (newVal < HM::ImpersonationCheck::Off || newVal > HM::ImpersonationCheck::Quarantine)
         return COMError::GenerateError("ImpersonationAction must be 0 (off), 1 (flag) or 2 (quarantine).");

      object_->SetImpersonationAction((int) newVal);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_ProtectedPersons(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      *pVal = object_->GetProtectedPersons().AllocSysString();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::put_ProtectedPersons(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      // Canonicalised - one entry per line, addresses lower-cased - and refused
      // with the reason when an entry's address is not an address or the text
      // is longer than 2,000 characters, as PUT /api/v1/domains/{domain} does.
      HM::String canonical;
      HM::String refusal;

      if (!HM::ImpersonationCheck::CanonicalisePersons(HM::String(newVal), canonical, refusal))
         return COMError::GenerateError(refusal);

      object_->SetProtectedPersons(canonical);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_ProtectedPartners(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      *pVal = object_->GetProtectedPartners().AllocSysString();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::put_ProtectedPartners(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      // The restricted-delivery list's shape: trimmed, lower-cased, the names
      // separated by ", ", and refused with the reason when a name is an
      // address, not a domain name, not ASCII, or the text is too long.
      HM::String canonical;
      HM::String refusal;

      if (!HM::ImpersonationCheck::CanonicalisePartners(HM::String(newVal), canonical, refusal))
         return COMError::GenerateError(refusal);

      object_->SetProtectedPartners(canonical);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_SpamTraps(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      std::vector<HM::SpamTrapMonitor::Listed> traps;

      if (!HM::SpamTrapMonitor::List(object_, traps))
         return COMError::GenerateError("The spam traps could not be read; see the error log.");

      // One address a line, which is what a script iterates over and what an
      // administration page shows. The REST listing is the one that carries the
      // id and the time each was declared.
      HM::String text;

      for (const HM::SpamTrapMonitor::Listed &trap : traps)
      {
         if (!text.IsEmpty())
            text += _T("\r\n");

         text += trap.address;
      }

      *pVal = text.AllocSysString();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::AddSpamTrap(BSTR Address)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      HM::String canonical;
      HM::String refusal;

      const HM::SpamTrapMonitor::AddResult result =
         HM::SpamTrapMonitor::Add(object_, HM::String(Address), canonical, refusal);

      // An address that is already a trap is not an error: asking twice for the
      // same state is the same state, and a configuration tool that applies a
      // list of traps must not fail on the second run.
      if (result == HM::SpamTrapMonitor::AddCreated || result == HM::SpamTrapMonitor::AddAlreadyATrap)
         return S_OK;

      return COMError::GenerateError(refusal);
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::RemoveSpamTrap(BSTR Address, VARIANT_BOOL *Removed)
{
   try
   {
      if (!Removed)
         return COMError::GenerateGenericMessage();

      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      HM::String canonical;
      bool found = false;

      if (!HM::SpamTrapMonitor::Remove(object_, HM::String(Address), canonical, found))
         return COMError::GenerateError("The spam trap could not be removed; see the error log.");

      *Removed = found ? VARIANT_TRUE : VARIANT_FALSE;

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_QuotaWarningPercent(long *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      *pVal = object_->GetQuotaWarningPercent();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::put_QuotaWarningPercent(long newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      if (newVal < -1 || newVal > 100)
         return COMError::GenerateError("QuotaWarningPercent must be between -1 and 100.");

      object_->SetQuotaWarningPercent((int) newVal);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_QuotaWarningSubject(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      *pVal = object_->GetQuotaWarningSubject().AllocSysString();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::put_QuotaWarningSubject(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      HM::String value = newVal;

      if (value.GetLength() > 200)
         return COMError::GenerateError("QuotaWarningSubject may be at most 200 characters.");

      object_->SetQuotaWarningSubject(value);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_QuotaWarningText(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      *pVal = object_->GetQuotaWarningText().AllocSysString();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::put_QuotaWarningText(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      HM::String value = newVal;

      if (value.GetLength() > 20000)
         return COMError::GenerateError("QuotaWarningText may be at most 20000 characters.");

      object_->SetQuotaWarningText(value);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_QuotaGracePercent(long *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      *pVal = object_->GetQuotaGracePercent();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::put_QuotaGracePercent(long newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      if (newVal < -1 || newVal > 1000)
         return COMError::GenerateError("QuotaGracePercent must be between -1 and 1000.");

      object_->SetQuotaGracePercent((int) newVal);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_QuotaSendStopPercent(long *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      *pVal = object_->GetQuotaSendStopPercent();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::put_QuotaSendStopPercent(long newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      if (newVal < -1 || newVal > 1000)
         return COMError::GenerateError("QuotaSendStopPercent must be between -1 and 1000.");

      object_->SetQuotaSendStopPercent((int) newVal);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_Rules(IInterfaceRules **pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      CComObject<InterfaceRules>* pItem = new CComObject<InterfaceRules>();
      pItem->SetAuthentication(authentication_);

      std::shared_ptr<HM::Rules> pRules = object_->GetRules();
      if (pRules)
      {
         pItem->Attach(pRules);
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

// One value, three levels (Common/Application/EffectiveSetting.h,
// docs/SettingLevels.md): the domain's own value of a setting that may be set
// below the server, what is in effect for its accounts that hold none of their
// own, and where that came from. The same names, values and refusals as the
// domain's fields on GET and PUT /api/v1/domains/{domain}.
STDMETHODIMP InterfaceDomain::get_SettingOverride(BSTR Name, BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      const HM::EffectiveSetting::Definition *definition = HM::EffectiveSetting::Find(HM::String(Name));
      if (!definition)
         return COMError::GenerateError(HM::EffectiveSetting::NotThreeLevelSentence(HM::String(Name)));

      HM::String value;
      if (!HM::EffectiveSetting::DomainValue(*definition, object_, value))
         value.Empty();

      *pVal = value.AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::put_SettingOverride(BSTR Name, BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      // Written at once, with no Save to pass through: the credential's grant
      // must reach this domain, as a role confined to named domains is asked
      // everywhere else it writes.
      if (object_->GetID() > 0 && !authentication_->AllowsForDomain(HM::RolePermission::AdministerDomain, object_->GetName()))
         return COMError::GenerateError("You do not have access to change the setting values of this domain.");

      const HM::String name(Name);
      const HM::EffectiveSetting::Definition *definition = HM::EffectiveSetting::Find(name);
      if (!definition)
         return COMError::GenerateError(HM::EffectiveSetting::NotThreeLevelSentence(name));

      // A level held in the domain's own field is refused as that, first.
      if (!HM::EffectiveSetting::OverlayHolds(*definition, HM::PersistentSettingOverride::ScopeDomain))
         return COMError::GenerateError(HM::EffectiveSetting::ColumnSentence(*definition, HM::PersistentSettingOverride::ScopeDomain));

      // A gate is the server administrator's to open or close for a domain; a
      // limit anybody who administers the domain may tighten, and nobody loosen
      // past the server's (EffectiveSetting::SetOverride says so).
      if (definition->server_administrators_only() && !authentication_->GetIsServerAdmin())
         return COMError::GenerateError(HM::String(definition->name) + _T(" is a capability the server grants, and the server administrator's to set for a domain."));

      HM::String value(newVal);
      HM::String refusal;

      const bool done = value.IsEmpty()
         ? HM::EffectiveSetting::ClearOverride(name, HM::PersistentSettingOverride::ScopeDomain, object_->GetID(), refusal)
         : HM::EffectiveSetting::SetOverride(name, HM::PersistentSettingOverride::ScopeDomain, object_->GetID(), value, refusal);

      if (!done)
         return COMError::GenerateError(refusal);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_EffectiveSetting(BSTR Name, BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      const HM::String name(Name);
      if (!HM::EffectiveSetting::Find(name))
         return COMError::GenerateError(HM::EffectiveSetting::NotThreeLevelSentence(name));

      *pVal = HM::EffectiveSetting::Resolve(name, object_, std::shared_ptr<const HM::Account>()).value.AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDomain::get_EffectiveSettingSource(BSTR Name, BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      const HM::String name(Name);
      if (!HM::EffectiveSetting::Find(name))
         return COMError::GenerateError(HM::EffectiveSetting::NotThreeLevelSentence(name));

      const HM::EffectiveSetting::Answer answer = HM::EffectiveSetting::Resolve(name, object_, std::shared_ptr<const HM::Account>());
      *pVal = HM::String(HM::EffectiveSetting::SourceWord(answer.source)).AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}
