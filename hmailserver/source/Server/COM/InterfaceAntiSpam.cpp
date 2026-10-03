// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"
#include "InterfaceQuarantine.h"
#include "COMError.h"
#include "../Common/Util/AuditTrail.h"


#include "../Common/AntiSpam/SpamAssassin/SpamAssassinTestConnect.h"

#include "../Common/AntiSpam/AntiSpamConfiguration.h"
#include "../Common/AntiSpam/DomainPolicyList.h"
#include "../Common/Util/Unicode.h"
#include "../Common/Application/IniFileSettings.h"
#include "../Common/AntiSpam/DKIM/DKIM.h"

#include "../Common/BO/WhiteListAddresses.h"
#include "../Common/BO/BlockedSenders.h"

#include "InterfaceAntiSpam.h"
#include "InterfaceGreyListingWhiteAddresses.h"
#include "InterfaceGreyListingBypassEntries.h"
#include "InterfaceWhiteListAddresses.h"
#include "InterfaceBlockedSenders.h"
#include "InterfaceSenderListEntries.h"
#include "../Common/Application/QuarantineDigestTask.h"
#include "../Common/BO/SenderListEntries.h"
#include "../Common/BO/SenderListEntry.h"
#include "InterfaceSURBLServers.h"
#include "InterfaceDNSBlackLists.h"

InterfaceAntiSpam::InterfaceAntiSpam() :
   config_(nullptr)
{

}

bool 
InterfaceAntiSpam::LoadSettings()
{
   if (!GetIsServerAdmin())
      return false;

   config_ = HM::Configuration::Instance();

   return true;
}


STDMETHODIMP InterfaceAntiSpam::get_SURBLServers(IInterfaceSURBLServers **pVal)
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      CComObject<InterfaceSURBLServers>* pSURBLServersInterface = new CComObject<InterfaceSURBLServers>();
      pSURBLServersInterface->SetAuthentication(authentication_);
      pSURBLServersInterface->LoadSettings();
   
      pSURBLServersInterface->AddRef();
      *pVal = pSURBLServersInterface;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::get_CheckHostInHelo(VARIANT_BOOL *pVal)
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      *pVal = config_->GetAntiSpamConfiguration().GetCheckHostInHelo() ? VARIANT_TRUE : VARIANT_FALSE;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::put_CheckHostInHelo(VARIANT_BOOL newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return GetAccessDenied();

      config_->GetAntiSpamConfiguration().SetCheckHostInHelo(newVal == VARIANT_TRUE ? true : false);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::get_CheckHostInHeloScore(long *pVal)
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      *pVal = config_->GetAntiSpamConfiguration().GetCheckHostInHeloScore();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::put_CheckHostInHeloScore(long newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return GetAccessDenied();

      config_->GetAntiSpamConfiguration().SetCheckHostInHeloScore(newVal);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::get_CheckPTR(VARIANT_BOOL *pVal)
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      *pVal = config_->GetAntiSpamConfiguration().GetCheckPTR() ? VARIANT_TRUE : VARIANT_FALSE;

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::put_CheckPTR(VARIANT_BOOL newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return GetAccessDenied();

      config_->GetAntiSpamConfiguration().SetCheckPTR(newVal == VARIANT_TRUE ? true : false);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::get_CheckPTRScore(long *pVal)
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      *pVal = config_->GetAntiSpamConfiguration().GetCheckPTRScore();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::put_CheckPTRScore(long newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return GetAccessDenied();

      config_->GetAntiSpamConfiguration().SetCheckPTRScore(newVal);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::get_SpamMarkThreshold(long *pVal)
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      *pVal = config_->GetAntiSpamConfiguration().GetSpamMarkThreshold();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::put_SpamMarkThreshold(long newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return GetAccessDenied();

      config_->GetAntiSpamConfiguration().SetSpamMarkThreshold(newVal);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::get_Quarantine(IInterfaceQuarantine** pVal)
{
   try
   {
      CComObject<InterfaceQuarantine>* pItem = new CComObject<InterfaceQuarantine>();
      pItem->SetAuthentication(authentication_);
      pItem->AddRef();

      // Returned unrefreshed. Reading Count is the common case and costs one query;
      // loading a page of rows is what Refresh is for, and doing it here would make
      // merely touching the property expensive on the servers with most to hold.
      *pVal = pItem;

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::get_SpamDeleteThreshold(long *pVal)
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      *pVal = config_->GetAntiSpamConfiguration().GetSpamDeleteThreshold();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::put_SpamDeleteThreshold(long newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return GetAccessDenied();

      config_->GetAntiSpamConfiguration().SetSpamDeleteThreshold(newVal);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::get_GreyListingEnabled(VARIANT_BOOL *pVal)
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      *pVal = config_->GetAntiSpamConfiguration().GetUseGreyListing() ? VARIANT_TRUE : VARIANT_FALSE;
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::put_GreyListingEnabled(VARIANT_BOOL newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return GetAccessDenied();

      config_->GetAntiSpamConfiguration().SetUseGreyListing(newVal == VARIANT_TRUE);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::get_GreyListingInitialDelay(long *pVal)
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      *pVal = config_->GetAntiSpamConfiguration().GetGreyListingInitialDelay();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::put_GreyListingInitialDelay(long newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return GetAccessDenied();

      config_->GetAntiSpamConfiguration().SetGreyListingInitialDelay(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::get_GreyListingInitialDelete(long *pVal)
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      *pVal = config_->GetAntiSpamConfiguration().GetGreyListingInitialDelete();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::put_GreyListingInitialDelete(long newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return GetAccessDenied();

      config_->GetAntiSpamConfiguration().SetGreyListingInitialDelete(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::get_GreyListingFinalDelete(long *pVal)
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      *pVal = config_->GetAntiSpamConfiguration().GetGreyListingFinalDelete();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::put_GreyListingFinalDelete(long newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return GetAccessDenied();

      config_->GetAntiSpamConfiguration().SetGreyListingFinalDelete(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::get_AddHeaderSpam(VARIANT_BOOL *pVal)
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      *pVal = config_->GetAntiSpamConfiguration().GetAddHeaderSpam() ? VARIANT_TRUE : VARIANT_FALSE;
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::put_AddHeaderSpam(VARIANT_BOOL newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return GetAccessDenied();

      config_->GetAntiSpamConfiguration().SetAddHeaderSpam(newVal == VARIANT_TRUE);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::get_AddHeaderReason(VARIANT_BOOL *pVal)
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      *pVal = config_->GetAntiSpamConfiguration().GetAddHeaderReason() ? VARIANT_TRUE : VARIANT_FALSE;
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::put_AddHeaderReason(VARIANT_BOOL newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return GetAccessDenied();

      config_->GetAntiSpamConfiguration().SetAddHeaderReason(newVal == VARIANT_TRUE);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::get_PrependSubject(VARIANT_BOOL *pVal)
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      *pVal = config_->GetAntiSpamConfiguration().GetPrependSubject() ? VARIANT_TRUE : VARIANT_FALSE;
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::put_PrependSubject(VARIANT_BOOL newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return GetAccessDenied();

      config_->GetAntiSpamConfiguration().SetPrependSubject(newVal == VARIANT_TRUE);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::get_PrependSubjectText(BSTR *pVal)
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      *pVal = config_->GetAntiSpamConfiguration().GetPrependSubjectText().AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::put_PrependSubjectText(BSTR newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return GetAccessDenied();

      config_->GetAntiSpamConfiguration().SetPrependSubjectText(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::get_GreyListingWhiteAddresses(IInterfaceGreyListingWhiteAddresses **pVal)
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      if (!config_)
         return COMError::WithoutMessage(DISP_E_BADINDEX);
   
      CComObject<InterfaceGreyListingWhiteAddresses>* pGLWhiteList = new CComObject<InterfaceGreyListingWhiteAddresses>();
      pGLWhiteList->SetAuthentication(authentication_);
      pGLWhiteList->Attach(HM::Configuration::Instance()->GetAntiSpamConfiguration().GetGreyListingWhiteAddresses());
      pGLWhiteList->AddRef();
   
      *pVal = pGLWhiteList;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::get_WhiteListAddresses(IInterfaceWhiteListAddresses **pVal)
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      if (!config_)
         return COMError::WithoutMessage(DISP_E_BADINDEX);
   
      CComObject<InterfaceWhiteListAddresses>* pGLWhiteList = new CComObject<InterfaceWhiteListAddresses>();
      pGLWhiteList->SetAuthentication(authentication_);
      pGLWhiteList->Attach(HM::Configuration::Instance()->GetAntiSpamConfiguration().GetWhiteListAddresses());
      pGLWhiteList->AddRef();

      *pVal = pGLWhiteList;

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::get_BlockedSenders(IInterfaceBlockedSenders **pVal)
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      CComObject<InterfaceBlockedSenders>* pBlockedSenders = new CComObject<InterfaceBlockedSenders>();
      pBlockedSenders->SetAuthentication(authentication_);
      pBlockedSenders->Attach(HM::Configuration::Instance()->GetAntiSpamConfiguration().GetBlockedSenders());
      pBlockedSenders->AddRef();

      *pVal = pBlockedSenders;

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::get_SenderList(IInterfaceSenderListEntries **pVal)
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      // A fresh collection every time rather than one the configuration holds.
      // The server does not read this collection - SenderListCache does, out of
      // its own copy of the whole table, which every save and delete
      // invalidates - so there is no live list here to keep in step, and a
      // caller that held a stale one would be editing yesterday's rows.
      std::shared_ptr<HM::SenderListEntries> entries =
         std::shared_ptr<HM::SenderListEntries>(new HM::SenderListEntries(HM::SenderListEntry::ScopeServer, 0));

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

STDMETHODIMP InterfaceAntiSpam::SendQuarantineDigests(long *pVal)
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();

      // A forced pass: the hour and the once-a-day rule are the scheduler's,
      // and an administrator pressing "send it now" is not asking to wait for
      // either. Everything that is the reader's or the configuration's still
      // applies - see QuarantineDigestTask::RunPass.
      *pVal = (long) HM::QuarantineDigestTask::RunPass(true);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::SendQuarantineDigestsAtHour(long Hour, long *pVal)
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();

      // The pass the scheduler would make in that hour, less the spread and
      // the twenty-hour rule: the digest switch and its hour are settings with
      // levels (docs/SettingLevels.md), and this is how an account's own hour
      // is shown to be the one that decides - see QuarantineDigestTask.
      *pVal = (long) HM::QuarantineDigestTask::RunPassAtHour((int) Hour);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::get_UseSPF(VARIANT_BOOL *pVal)
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      *pVal = config_->GetAntiSpamConfiguration().GetUseSPF() ? VARIANT_TRUE : VARIANT_FALSE;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::put_UseSPF(VARIANT_BOOL newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return GetAccessDenied();

      config_->GetAntiSpamConfiguration().SetUseSPF(newVal == VARIANT_TRUE ? true : false);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::get_UseSPFScore(long *pVal)
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      *pVal = config_->GetAntiSpamConfiguration().GetUseSPFScore();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::put_UseSPFScore(long newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return GetAccessDenied();

      config_->GetAntiSpamConfiguration().SetUseSPFScore(newVal);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::get_UseMXChecks(VARIANT_BOOL *pVal)
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      *pVal = config_->GetAntiSpamConfiguration().GetUseMXChecks() ? VARIANT_TRUE : VARIANT_FALSE;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::put_UseMXChecks(VARIANT_BOOL newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return GetAccessDenied();

      config_->GetAntiSpamConfiguration().SetUseMXChecks(newVal == VARIANT_TRUE ? true : false);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::get_UseMXChecksScore(long *pVal)
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      *pVal = config_->GetAntiSpamConfiguration().GetUseMXChecksScore();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::put_UseMXChecksScore(long newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return GetAccessDenied();

      config_->GetAntiSpamConfiguration().SetUseMXChecksScore(newVal);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::get_DNSBlackLists(IInterfaceDNSBlackLists **pVal)
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      CComObject<InterfaceDNSBlackLists>* pDNSBlackListsInterface = new CComObject<InterfaceDNSBlackLists>();
      pDNSBlackListsInterface->SetAuthentication(authentication_);
      pDNSBlackListsInterface->LoadSettings();
   
      pDNSBlackListsInterface->AddRef();
      *pVal = pDNSBlackListsInterface;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

// The recipient tarpit. These two properties were stubs marked obsolete from 5.x
// until 5 September 2026 - reads answered 0 and writes were ignored - and are real
// again, backed by SmtpTarpitDelaySeconds / SmtpTarpitCount in hMailServer.ini
// rather than by the settings rows 5.4 deleted. See SMTPConnection::TarpitRecipient_.

STDMETHODIMP InterfaceAntiSpam::get_TarpitDelay(long *pVal)
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      *pVal = HM::IniFileSettings::Instance()->GetSmtpTarpitDelaySeconds();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::put_TarpitDelay(long newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return GetAccessDenied();

      if (newVal < 0 || newVal > HM::IniFileSettings::TARPIT_MAX_SECONDS)
         return COMError::GenerateError("TarpitDelay must be between 0 and 30 seconds.");

      if (!HM::IniFileSettings::Instance()->SetSmtpTarpitDelaySeconds((int) newVal))
         return COMError::GenerateError("The setting could not be written to hMailServer.INI. Nothing has been changed.");

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::get_TarpitCount(long *pVal)
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      *pVal = HM::IniFileSettings::Instance()->GetSmtpTarpitCount();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::put_TarpitCount(long newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return GetAccessDenied();

      if (newVal < 0)
         return COMError::GenerateError("TarpitCount cannot be negative.");

      if (!HM::IniFileSettings::Instance()->SetSmtpTarpitCount((int) newVal))
         return COMError::GenerateError("The setting could not be written to hMailServer.INI. Nothing has been changed.");

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::get_SpamAssassinEnabled(VARIANT_BOOL *pVal)
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      *pVal = config_->GetAntiSpamConfiguration().GetSpamAssassinEnabled() ? VARIANT_TRUE : VARIANT_FALSE;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::put_SpamAssassinEnabled(VARIANT_BOOL newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return GetAccessDenied();

      config_->GetAntiSpamConfiguration().SetSpamAssassinEnabled(newVal == VARIANT_TRUE ? true : false);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::get_SpamAssassinScore(long *pVal)
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      *pVal = config_->GetAntiSpamConfiguration().GetSpamAssassinScore();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::put_SpamAssassinScore(long newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return GetAccessDenied();

      config_->GetAntiSpamConfiguration().SetSpamAssassinScore(newVal);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::get_SpamAssassinMergeScore(VARIANT_BOOL *pVal)
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      *pVal = config_->GetAntiSpamConfiguration().GetSpamAssassinMergeScore() ? VARIANT_TRUE : VARIANT_FALSE;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::put_SpamAssassinMergeScore(VARIANT_BOOL newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return GetAccessDenied();

      config_->GetAntiSpamConfiguration().SetSpamAssassinMergeScore(newVal == VARIANT_TRUE);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::get_SpamAssassinHost(BSTR *pVal)
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      *pVal = config_->GetAntiSpamConfiguration().GetSpamAssassinHost().AllocSysString();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::put_SpamAssassinHost(BSTR newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return GetAccessDenied();

      config_->GetAntiSpamConfiguration().SetSpamAssassinHost(newVal);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::get_SpamAssassinPort(long *pVal)
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      *pVal = config_->GetAntiSpamConfiguration().GetSpamAssassinPort();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::put_SpamAssassinPort(long newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return GetAccessDenied();

      config_->GetAntiSpamConfiguration().SetSpamAssassinPort(newVal);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::get_MaximumMessageSize(long *pVal)
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      *pVal = config_->GetAntiSpamConfiguration().GetAntiSpamMaxSizeKB();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::put_MaximumMessageSize(long newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return GetAccessDenied();

      config_->GetAntiSpamConfiguration().SetAntiSpamMaxSizeKB(newVal);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::ClearGreyListingTriplets()
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      config_->GetAntiSpamConfiguration().ClearGreyListingTriplets();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::DKIMVerify(BSTR pVal, eDKIMResult *pResult)
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      HM::DKIM dkim;
      *pResult = (eDKIMResult) dkim.Verify(pVal);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::TestSpamAssassinConnection(BSTR hostname, long port, BSTR *messageText, VARIANT_BOOL *pResult)
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      HM::SpamAssassinTestConnect testClient;
      
      HM::String text;
      *pResult = testClient.TestConnect(hostname, port, text) ? VARIANT_TRUE : VARIANT_FALSE;
      *messageText = text.AllocSysString();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::get_DKIMVerificationEnabled(VARIANT_BOOL *pVal)
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      *pVal = config_->GetAntiSpamConfiguration().GetDKIMVerificationEnabled() ? VARIANT_TRUE : VARIANT_FALSE;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::put_DKIMVerificationEnabled(VARIANT_BOOL newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return GetAccessDenied();

      config_->GetAntiSpamConfiguration().SetDKIMVerificationEnabled(newVal == VARIANT_TRUE);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::get_DKIMVerificationFailureScore(long *pVal)
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      *pVal = config_->GetAntiSpamConfiguration().GetDKIMVerificationFailureScore();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::put_DKIMVerificationFailureScore(long newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return GetAccessDenied();

      config_->GetAntiSpamConfiguration().SetDKIMVerificationFailureScore(newVal);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::get_DMARCEnabled(VARIANT_BOOL *pVal)
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      *pVal = config_->GetAntiSpamConfiguration().GetDMARCEnabled() ? VARIANT_TRUE : VARIANT_FALSE;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::put_DMARCEnabled(VARIANT_BOOL newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return GetAccessDenied();

      config_->GetAntiSpamConfiguration().SetDMARCEnabled(newVal == VARIANT_TRUE);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::get_ArcFilteringEnabled(VARIANT_BOOL *pVal)
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      *pVal = config_->GetAntiSpamConfiguration().GetArcFilteringEnabled() ? VARIANT_TRUE : VARIANT_FALSE;

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::put_ArcFilteringEnabled(VARIANT_BOOL newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return GetAccessDenied();

      config_->GetAntiSpamConfiguration().SetArcFilteringEnabled(newVal == VARIANT_TRUE);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::get_ArcTrustedSealers(BSTR *pVal)
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      *pVal = config_->GetAntiSpamConfiguration().GetArcTrustedSealers().AllocSysString();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::put_ArcTrustedSealers(BSTR newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return GetAccessDenied();

      config_->GetAntiSpamConfiguration().SetArcTrustedSealers(newVal);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::get_DMARCFailureScore(long *pVal)
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      *pVal = config_->GetAntiSpamConfiguration().GetDMARCFailureScore();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::put_DMARCFailureScore(long newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return GetAccessDenied();

      config_->GetAntiSpamConfiguration().SetDMARCFailureScore(newVal);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::get_BypassGreylistingOnSPFSuccess(VARIANT_BOOL *pVal)
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      *pVal = config_->GetAntiSpamConfiguration().GetBypassGreyListingOnSPFSuccess() ? VARIANT_TRUE : VARIANT_FALSE;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::put_BypassGreylistingOnSPFSuccess(VARIANT_BOOL newValue)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return GetAccessDenied();

      config_->GetAntiSpamConfiguration().SetBypassGreyListingOnSPFSuccess(newValue == VARIANT_TRUE);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::get_BypassGreylistingOnMailFromMX(VARIANT_BOOL *pVal)
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      *pVal = config_->GetAntiSpamConfiguration().GetBypassGreyListingOnMailFromMX() ? VARIANT_TRUE : VARIANT_FALSE;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::put_BypassGreylistingOnMailFromMX(VARIANT_BOOL newValue)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return GetAccessDenied();

      config_->GetAntiSpamConfiguration().SetBypassGreyListingOnMailFromMX(newValue == VARIANT_TRUE);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::get_GreyListingBypassEntries(IInterfaceGreyListingBypassEntries **pVal)
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      CComObject<InterfaceGreyListingBypassEntries>* pEntries = new CComObject<InterfaceGreyListingBypassEntries>();
      pEntries->SetAuthentication(authentication_);
      pEntries->Attach(HM::Configuration::Instance()->GetAntiSpamConfiguration().GetGreyListingBypassEntries());
      pEntries->AddRef();

      *pVal = pEntries;

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::get_GreyListingSlideOnEarlyRetry(VARIANT_BOOL *pVal)
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      *pVal = config_->GetAntiSpamConfiguration().GetGreyListingSlideOnEarlyRetry() ? VARIANT_TRUE : VARIANT_FALSE;
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::put_GreyListingSlideOnEarlyRetry(VARIANT_BOOL newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return GetAccessDenied();

      config_->GetAntiSpamConfiguration().SetGreyListingSlideOnEarlyRetry(newVal == VARIANT_TRUE);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::get_GreyListingAbuseThreshold(long *pVal)
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      *pVal = config_->GetAntiSpamConfiguration().GetGreyListingAbuseThreshold();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::put_GreyListingAbuseThreshold(long newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return GetAccessDenied();

      // The same sentence RestApiSettings refuses with, so the two surfaces
      // say one thing.
      if (newVal < 0 || newVal > 100000)
         return COMError::GenerateError("GreyListingAbuseThreshold is between 0 and 100000. 0 never refuses: a triplet retried too soon is deferred again, as it always was.");

      config_->GetAntiSpamConfiguration().SetGreyListingAbuseThreshold((int) newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::get_GreyListingAbuseLockMinutes(long *pVal)
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      *pVal = config_->GetAntiSpamConfiguration().GetGreyListingAbuseLockMinutes();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::put_GreyListingAbuseLockMinutes(long newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return GetAccessDenied();

      if (newVal < 1 || newVal > 10080)
         return COMError::GenerateError("GreyListingAbuseLockMinutes is between 1 and 10080 (a week): how long the 550 of GreyListingAbuseThreshold lasts.");

      config_->GetAntiSpamConfiguration().SetGreyListingAbuseLockMinutes((int) newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}


// ---------------------------------------------------------------------------
// SMTP policy the score cannot express (discussions #283, #285, #286). The
// seven scalars are hm_settings rows through AntiSpamConfiguration, as the
// SPF score above is; the list is Common/AntiSpam/DomainPolicyList.h.
// ---------------------------------------------------------------------------

STDMETHODIMP InterfaceAntiSpam::get_UseSPFSoftFailScore(long *pVal)
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      *pVal = config_->GetAntiSpamConfiguration().GetUseSPFSoftFailScore();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::put_UseSPFSoftFailScore(long newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return GetAccessDenied();

      config_->GetAntiSpamConfiguration().SetUseSPFSoftFailScore((int) newVal);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::get_UseSPFTempErrorScore(long *pVal)
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      *pVal = config_->GetAntiSpamConfiguration().GetUseSPFTempErrorScore();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::put_UseSPFTempErrorScore(long newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return GetAccessDenied();

      config_->GetAntiSpamConfiguration().SetUseSPFTempErrorScore((int) newVal);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::get_UseSPFPermErrorScore(long *pVal)
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      *pVal = config_->GetAntiSpamConfiguration().GetUseSPFPermErrorScore();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::put_UseSPFPermErrorScore(long newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return GetAccessDenied();

      config_->GetAntiSpamConfiguration().SetUseSPFPermErrorScore((int) newVal);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::get_SPFTempErrorDefer(VARIANT_BOOL *pVal)
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      *pVal = config_->GetAntiSpamConfiguration().GetSPFTempErrorDefer() ? VARIANT_TRUE : VARIANT_FALSE;

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::put_SPFTempErrorDefer(VARIANT_BOOL newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return GetAccessDenied();

      config_->GetAntiSpamConfiguration().SetSPFTempErrorDefer(newVal == VARIANT_TRUE);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::get_RejectMultiRcptNullSender(VARIANT_BOOL *pVal)
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      *pVal = config_->GetAntiSpamConfiguration().GetRejectMultiRcptNullSender() ? VARIANT_TRUE : VARIANT_FALSE;

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::put_RejectMultiRcptNullSender(VARIANT_BOOL newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return GetAccessDenied();

      config_->GetAntiSpamConfiguration().SetRejectMultiRcptNullSender(newVal == VARIANT_TRUE);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::get_BanMultiRcptNullSender(VARIANT_BOOL *pVal)
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      *pVal = config_->GetAntiSpamConfiguration().GetBanMultiRcptNullSender() ? VARIANT_TRUE : VARIANT_FALSE;

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::put_BanMultiRcptNullSender(VARIANT_BOOL newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return GetAccessDenied();

      config_->GetAntiSpamConfiguration().SetBanMultiRcptNullSender(newVal == VARIANT_TRUE);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::get_BanMultiRcptNullSenderHours(long *pVal)
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      *pVal = config_->GetAntiSpamConfiguration().GetBanMultiRcptNullSenderHours();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::put_BanMultiRcptNullSenderHours(long newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return GetAccessDenied();

      if (newVal < 1 || newVal > 8760)
         return COMError::GenerateError("BanMultiRcptNullSenderHours is between 1 and 8760 (a year): how long the address of a refused multi-recipient null sender is banned.");

      config_->GetAntiSpamConfiguration().SetBanMultiRcptNullSenderHours((int) newVal);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::get_RefuseHeloNotFqdn(VARIANT_BOOL *pVal)
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      *pVal = config_->GetAntiSpamConfiguration().GetRefuseHeloNotFqdn() ? VARIANT_TRUE : VARIANT_FALSE;

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::put_RefuseHeloNotFqdn(VARIANT_BOOL newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return GetAccessDenied();

      config_->GetAntiSpamConfiguration().SetRefuseHeloNotFqdn(newVal == VARIANT_TRUE);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::get_RefuseMailFromDomainNotFqdn(VARIANT_BOOL *pVal)
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      *pVal = config_->GetAntiSpamConfiguration().GetRefuseMailFromDomainNotFqdn() ? VARIANT_TRUE : VARIANT_FALSE;

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::put_RefuseMailFromDomainNotFqdn(VARIANT_BOOL newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return GetAccessDenied();

      config_->GetAntiSpamConfiguration().SetRefuseMailFromDomainNotFqdn(newVal == VARIANT_TRUE);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::get_SurblTooManyDomainsScore(long *pVal)
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      *pVal = config_->GetAntiSpamConfiguration().GetSurblTooManyDomainsScore();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::put_SurblTooManyDomainsScore(long newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return GetAccessDenied();

      config_->GetAntiSpamConfiguration().SetSurblTooManyDomainsScore(newVal);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::get_DomainPolicyList(BSTR *pVal)
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      bool readable = false;
      HM::AnsiString document = HM::DomainPolicyList::Document(readable);

      if (!readable)
         return COMError::GenerateError("The domain policy list could not be read; see the error log.");

      HM::String wide;
      HM::Unicode::MultiByteToWide(document, wide);

      *pVal = wide.AllocSysString();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::AddDomainPolicyListEntry(BSTR Pattern, BSTR MatchOn, BSTR Action, BSTR Note, long *ID)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return GetAccessDenied();

      // The audit trail records the row the persister writes; the whole list is
      // server-wide, so the whole list is the server administrator's, as every
      // other setter of this object is.
      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();

      __int64 id = 0;
      HM::String refusal;

      if (!HM::DomainPolicyList::Add(Pattern, MatchOn, Action, Note, id, refusal))
         return COMError::GenerateError(refusal);

      *ID = (long) id;

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAntiSpam::RemoveDomainPolicyListEntry(long ID, VARIANT_BOOL *Removed)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return GetAccessDenied();

      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();

      bool found = false;

      if (!HM::DomainPolicyList::Remove(ID, found))
         return COMError::GenerateError("The domain policy list entry could not be removed; see the error log.");

      *Removed = found ? VARIANT_TRUE : VARIANT_FALSE;

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}
