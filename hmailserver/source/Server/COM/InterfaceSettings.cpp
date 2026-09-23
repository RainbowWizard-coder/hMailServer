// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"

#include "COMError.h"
#include "../Common/Util/AuditTrail.h"

#include "InterfaceSettings.h"
#include "../Common/Util/CountryBlocking.h"
#include "InterfaceLogging.h"
#include "InterfaceAntiVirus.h"
#include "InterfaceCache.h"

#include "../Common/Application/Application.h"
#include "../Common/Application/ACLManager.h"
#include "../Common/Application/SettingsDiff.h"
#include "../Common/Application/Profiles.h"
#include "../Common/Util/JsonDocument.h"
#include "../Common/Util/Unicode.h"
#include "../Common/Util/ReputationWatch.h"
#include "../Common/Application/IniSettingStore.h"
#include "../Common/Application/EnvironmentSettings.h"
#include "../Common/Application/Logger.h"
#include "../Common/Util/MessageOrigin.h"
#include "../Common/Util/Parsing/StringParser.h"
#include "../Common/Util/Totp.h"
#include "../Common/Util/PasswordBreachCheck.h"
#include "../Common/Util/SessionPolicies.h"
#include "../Common/LDAP/LdapClient.h"
#include "../Common/LDAP/LdapSettings.h"
#include "../Common/LDAP/DirectorySync.h"
#include "../Common/BO/TCPIPPorts.h"
#include "../Common/BO/SSLCertificates.h"
#include "../Common/BO/IncomingRelays.h"
#include "../Common/AntiSpam/AntiSpamConfiguration.h"

#include "../Common/Persistence/PersistentServerMessage.h"

#include "../POP3/POP3Configuration.h"
#include "../SMTP/SMTPConfiguration.h"
#include "../IMAP/IMAPConfiguration.h"


#include "InterfaceSecurityRanges.h"
#include "InterfaceRoutes.h"
#include "InterfaceRemoteDomainPolicies.h"
#include "InterfaceIMAPFolders.h"
#include "InterfaceScripting.h"
#include "InterfaceBackupSettings.h"
#include "InterfaceAntiSpam.h"
#include "InterfaceServerMessages.h"
#include "InterfaceTCPIPPorts.h"
#include "InterfaceSSLCertificates.h"
#include "InterfaceDirectories.h"
#include "InterfaceGroups.h"
#include "InterfaceIncomingRelays.h"
#include "InterfaceMessageIndexing.h"

#include "../Common/BO/Groups.h"

InterfaceSettings::InterfaceSettings() :
   config_(nullptr),
   ini_file_settings_(nullptr)
{

}

STDMETHODIMP InterfaceSettings::InterfaceSupportsErrorInfo(REFIID riid)
{
   static const IID* arr[] = 
   {
      &IID_IInterfaceSettings,
   };

   for (int i=0;i<sizeof(arr)/sizeof(arr[0]);i++)
   {
      if (InlineIsEqualGUID(*arr[i],riid))
         return S_OK;
   }
   return S_FALSE;
}
   
bool 
InterfaceSettings::LoadSettings()
{
   if (!GetIsServerAdmin())
      return false;

   // The configuration's objects exist once Configuration::Load has run. Before
   // that - a database the server opened and refused (HM5011) and left open
   // for DBUpdater - they do not, and every getter that read them was an
   // access violation until the guard of 12 September 2026, which then refused
   // the whole settings object; DBUpdater reads Settings.Directories before it
   // can run a script, so every upgrade from an older schema stopped there
   // (issue #263). Now: the directories, which come from hMailServer.ini,
   // answer; everything else is refused with the reason.
   config_ = HM::Application::Instance()->IsInitialized() ? HM::Configuration::Instance() : nullptr;
   ini_file_settings_ = HM::IniFileSettings::Instance();

   return true;
}

HRESULT
InterfaceSettings::NotLoaded_() const
{
   HM::String reason = HM::Application::Instance()->GetLastErrorMessage();
   if (reason.IsEmpty())
      reason = "The server has not finished starting.";
   return COMError::GenerateError(HM::String("The server has not loaded its configuration. ") + reason);
}

STDMETHODIMP InterfaceSettings::get_MaxMessageSize(long *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetSMTPConfiguration()->GetMaxMessageSize();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_MaxMessageSize(long newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      config_->GetSMTPConfiguration()->SetMaxMessageSize(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_MaxSMTPConnections(long *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetSMTPConfiguration()->GetMaxSMTPConnections();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_MaxSMTPConnections(long newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      config_->GetSMTPConfiguration()->SetMaxSMTPConnections(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_MaxIMAPConnections(long *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetIMAPConfiguration()->GetMaxIMAPConnections();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_MaxIMAPConnections(long newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      config_->GetIMAPConfiguration()->SetMaxIMAPConnections(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_MaxPOP3Connections(long *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

   
      *pVal = config_->GetPOP3Configuration()->GetMaxPOP3Connections();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_MaxPOP3Connections(long newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      config_->GetPOP3Configuration()->SetMaxPOP3Connections(newVal);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_MirrorEMailAddress(BSTR *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetMirrorAddress().AllocSysString();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_MirrorEMailAddress(BSTR newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

   
      config_->SetMirrorAddress(newVal);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_AllowSMTPAuthPlain(VARIANT_BOOL *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

   
      if (config_->GetSMTPConfiguration()->GetAuthAllowPlainText())
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

STDMETHODIMP InterfaceSettings::put_AllowSMTPAuthPlain(VARIANT_BOOL newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

   
      if (newVal == VARIANT_TRUE)
         config_->GetSMTPConfiguration()->SetAuthAllowPlainText(true);
      else
         config_->GetSMTPConfiguration()->SetAuthAllowPlainText(false);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_DenyMailFromNull(VARIANT_BOOL *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      if (config_->GetSMTPConfiguration()->GetAllowMailFromNull())
         *pVal = VARIANT_FALSE;
      else
         *pVal = VARIANT_TRUE;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_DenyMailFromNull(VARIANT_BOOL newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

   
   
      if (newVal == VARIANT_TRUE)
         config_->GetSMTPConfiguration()->SetAllowMailFromNull (false);
      else
         config_->GetSMTPConfiguration()->SetAllowMailFromNull (true);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_AllowIncorrectLineEndings(VARIANT_BOOL *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetSMTPConfiguration()->GetAllowIncorrectLineEndings() ? VARIANT_TRUE : VARIANT_FALSE;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_AllowIncorrectLineEndings(VARIANT_BOOL newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      config_->GetSMTPConfiguration()->SetAllowIncorrectLineEndings(newVal == VARIANT_TRUE);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_Logging(IInterfaceLogging **pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      if (!GetIsServerAdmin())
         return authentication_->GetAccessDenied();
   
      CComObject<InterfaceLogging>* pInterfaceLogging = new CComObject<InterfaceLogging>;
      
      pInterfaceLogging->SetAuthentication(authentication_);
      pInterfaceLogging->LoadSettings();
   
      pInterfaceLogging->AddRef();
   
      *pVal = pInterfaceLogging;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_AntiVirus(IInterfaceAntiVirus **pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      if (!GetIsServerAdmin())
         return authentication_->GetAccessDenied();
   
      CComObject<InterfaceAntiVirus>* pInterfaceAntiVirus = new CComObject<InterfaceAntiVirus>;
   
      pInterfaceAntiVirus->SetAuthentication(authentication_);
   
      pInterfaceAntiVirus->AddRef();
   
      *pVal = pInterfaceAntiVirus;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_Cache(IInterfaceCache **pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      if (!GetIsServerAdmin())
         return authentication_->GetAccessDenied();
   
      CComObject<InterfaceCache>* pInterfaceCache = new CComObject<InterfaceCache>;
      
      pInterfaceCache->SetAuthentication(authentication_);
      pInterfaceCache->LoadSettings();
   
      pInterfaceCache->AddRef();
   
      *pVal = pInterfaceCache;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_SecurityRanges(IInterfaceSecurityRanges **pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      if (!GetIsServerAdmin())
         return authentication_->GetAccessDenied();
   
      CComObject<InterfaceSecurityRanges>* pRangesInt = new CComObject<InterfaceSecurityRanges>();
      pRangesInt->SetAuthentication(authentication_);
      pRangesInt->LoadSettings();
   
      pRangesInt->AddRef();
      *pVal = pRangesInt;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_SMTPNoOfTries(long *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetSMTPConfiguration()->GetNoOfRetries();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_SMTPNoOfTries(long newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

   
      config_->GetSMTPConfiguration()->SetNoOfRetries(newVal);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_SMTPMinutesBetweenTry(long *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

   
   
      *pVal = config_->GetSMTPConfiguration()->GetMinutesBetweenTry();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_SMTPMinutesBetweenTry(long newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

   
      config_->GetSMTPConfiguration()->SetMinutesBetweenTry(newVal);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_MaxDeliveryThreads(long *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

   
   
      *pVal = config_->GetSMTPConfiguration()->GetMaxNoOfDeliveryThreads();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_MaxDeliveryThreads(long newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

   
      config_->GetSMTPConfiguration()->SetMaxNoOfDeliveryThreads(newVal);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_SMTPRelayer(BSTR *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

   
      *pVal = config_->GetSMTPConfiguration()->GetSMTPRelayer().AllocSysString();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_SMTPRelayer(BSTR newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

   
      config_->GetSMTPConfiguration()->SetSMTPRelayer(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_SMTPRelayerPort(long *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

   
      *pVal = config_->GetSMTPConfiguration()->GetSMTPRelayerPort();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_SMTPRelayerPort(long newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

   
      config_->GetSMTPConfiguration()->SetSMTPRelayerPort(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_HostName(BSTR *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

   
      *pVal = config_->GetHostName().AllocSysString();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_HostName(BSTR newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      HM::String sNewVal = newVal;
      config_->SetHostName(sNewVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_WelcomeSMTP(BSTR *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetSMTPConfiguration()->GetWelcomeMessage().AllocSysString();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_WelcomeSMTP(BSTR newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      config_->GetSMTPConfiguration()->SetWelcomeMessage(newVal);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_WelcomePOP3(BSTR *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetPOP3Configuration()->GetWelcomeMessage().AllocSysString();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_WelcomePOP3(BSTR newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      config_->GetPOP3Configuration()->SetWelcomeMessage(newVal);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_WelcomeIMAP(BSTR *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetIMAPConfiguration()->GetWelcomeMessage().AllocSysString();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_WelcomeIMAP(BSTR newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      config_->GetIMAPConfiguration()->SetWelcomeMessage(newVal);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_ServiceSMTP(VARIANT_BOOL *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      if (config_->GetUseSMTP())
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

STDMETHODIMP InterfaceSettings::put_ServiceSMTP(VARIANT_BOOL newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      if (newVal == VARIANT_TRUE)
         config_->SetUseSMTP(true);
      else
         config_->SetUseSMTP(false);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_ServicePOP3(VARIANT_BOOL *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      if (config_->GetUsePOP3())
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

STDMETHODIMP InterfaceSettings::put_ServicePOP3(VARIANT_BOOL newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      if (newVal == VARIANT_TRUE)
         config_->SetUsePOP3(true);
      else
         config_->SetUsePOP3(false);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_ServiceIMAP(VARIANT_BOOL *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetUseIMAP() ? VARIANT_TRUE : VARIANT_FALSE;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_ServiceIMAP(VARIANT_BOOL newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      config_->SetUseIMAP(newVal == VARIANT_TRUE ? true : false);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_SMTPRelayerRequiresAuthentication(VARIANT_BOOL *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetSMTPConfiguration()->GetSMTPRelayerRequiresAuthentication() ? VARIANT_TRUE : VARIANT_FALSE;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_SMTPRelayerRequiresAuthentication(VARIANT_BOOL newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      config_->GetSMTPConfiguration()->SetSMTPRelayerRequiresAuthentication(newVal == VARIANT_TRUE);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_SMTPRelayerUsername(BSTR *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

   
      *pVal = config_->GetSMTPConfiguration()->GetSMTPRelayerUsername().AllocSysString();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_SMTPRelayerUsername(BSTR newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      config_->GetSMTPConfiguration()->SetSMTPRelayerUsername(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_UserInterfaceLanguage(BSTR *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

   
      *pVal = ini_file_settings_->GetUserInterfaceLanguage().AllocSysString();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_UserInterfaceLanguage(BSTR newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      ini_file_settings_->SetUserInterfaceLanguage(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::SetSMTPRelayerPassword(BSTR newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      config_->GetSMTPConfiguration()->SetSMTPRelayerPassword(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::SetAdministratorPassword(BSTR newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      if (!GetIsServerAdmin())
         return GetAccessDenied();
      
   
      // The administrator credential is held to the breach check as a mailbox
      // password is (the mailbox policy has never applied to it). What cannot be
      // reached from here is hmailserver --set-admin-password, which runs with no
      // database open and so no settings to read; that is the way back in when the
      // check is set to refuse and its endpoint is down.
      HM::String breachRefusal;
      HM::AnsiString breachReason;
      int breachStatus = 0;

      if (!HM::PasswordBreachCheck::IsAcceptable(_T("the administrator"), HM::String(newVal), breachRefusal, breachReason, breachStatus))
         return COMError::GenerateError(breachRefusal);

      ini_file_settings_->SetAdministratorPassword(newVal);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::EnrolAdministratorTOTP(BSTR *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      if (!GetIsServerAdmin())
         return GetAccessDenied();

      // A second factor guards a password. With no password set, anyone
      // authenticates anonymously (COMAuthentication::AttempAnonymousAuthentication)
      // and a code would guard nothing.
      if (ini_file_settings_->GetAdministratorPassword().IsEmpty())
         return COMError::GenerateError("Set the administrator password first. A second factor protects a password, and there is none to protect.");

      HM::AnsiString secret = HM::Totp::GenerateSecret();

      if (secret.IsEmpty())
         return COMError::GenerateError("The random number generator failed, so no second factor was enrolled. Nothing has been stored. See the hMailServer error log.");

      if (!ini_file_settings_->SetAdministratorTotpSecret(HM::String(secret)))
         return COMError::GenerateError("The secret could not be written to hMailServer.INI. The account the server runs as needs write access to that file. Nothing has been changed.");

      // The URI carries the secret, and this is the only moment it leaves the
      // server. Enforced from this moment: the session that enrolled stays
      // authenticated, and the next logon needs the code.
      *pVal = HM::String(HM::Totp::BuildOtpAuthUri("Administrator", secret)).AllocSysString();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::DisableAdministratorTOTP()
{
   try
   {
      if (!config_)
         return NotLoaded_();

      if (!GetIsServerAdmin())
         return GetAccessDenied();

      if (!ini_file_settings_->SetAdministratorTotpSecret(HM::String()))
         return COMError::GenerateError("The secret could not be removed from hMailServer.INI. The account the server runs as needs write access to that file. Nothing has been changed.");

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

// What this server will do for a named remote domain: the TLS it demands of it
// outbound and inbound, the size and concurrency it will attempt, and whether a
// recipient is verified with the domain's own server before mail for it is
// accepted. Server administrators only, like the routes beside it.
STDMETHODIMP InterfaceSettings::get_RemoteDomainPolicies(IInterfaceRemoteDomainPolicies **pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      if (!GetIsServerAdmin())
         return authentication_->GetAccessDenied();

      CComObject<InterfaceRemoteDomainPolicies>* interfacePolicies = new CComObject<InterfaceRemoteDomainPolicies>();
      interfacePolicies->SetAuthentication(authentication_);
      interfacePolicies->LoadSettings();

      interfacePolicies->AddRef();
      *pVal = interfacePolicies;

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_Routes(IInterfaceRoutes **pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      if (!GetIsServerAdmin())
         return authentication_->GetAccessDenied();
   
      CComObject<InterfaceRoutes>* pRoutesInterface = new CComObject<InterfaceRoutes>();
      pRoutesInterface->SetAuthentication(authentication_);
      pRoutesInterface->LoadSettings();
   
      pRoutesInterface->AddRef();
      *pVal = pRoutesInterface;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_Scripting(IInterfaceScripting **pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      if (!GetIsServerAdmin())
         return authentication_->GetAccessDenied();
   
      CComObject<InterfaceScripting>* pItem = new CComObject<InterfaceScripting >();
      pItem->SetAuthentication(authentication_);
      pItem->LoadSettings();
   
      pItem->AddRef();
      *pVal = pItem;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_Backup(IInterfaceBackupSettings **pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      if (!GetIsServerAdmin())
         return authentication_->GetAccessDenied();
   
      CComObject<InterfaceBackupSettings>* pItem = new CComObject<InterfaceBackupSettings>();
     
      pItem->SetAuthentication(authentication_);
      pItem->LoadSettings();
   
      pItem->AddRef();
      *pVal = pItem;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_Directories(IInterfaceDirectories **pVal)
{
   try
   {
      // The directories come from hMailServer.ini, not the configuration, and
      // DBUpdater needs them on a database the server refused.
      if (!GetIsServerAdmin())
         return authentication_->GetAccessDenied();
   
      CComObject<InterfaceDirectories>* pItem = new CComObject<InterfaceDirectories>();
   
      ini_file_settings_->LoadSettings();
   
      pItem->SetAuthentication(authentication_);
      pItem->LoadSettings(ini_file_settings_);
      pItem->AddRef();
      *pVal = pItem;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_AntiSpam(IInterfaceAntiSpam **pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      CComObject<InterfaceAntiSpam>* pItem = new CComObject<InterfaceAntiSpam>();
      
      pItem->SetAuthentication(authentication_);
      pItem->LoadSettings();
   
      pItem->AddRef();
      *pVal = pItem;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_ServerMessages(IInterfaceServerMessages **pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      if (!GetIsServerAdmin())
         return authentication_->GetAccessDenied();
   
      CComObject<InterfaceServerMessages>* pItem = new CComObject<InterfaceServerMessages>();
      
      pItem->SetAuthentication(authentication_);
      pItem->LoadSettings();
   
      pItem->AddRef();
      *pVal = pItem;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_TCPIPPorts(IInterfaceTCPIPPorts **pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      if (!GetIsServerAdmin())
         return authentication_->GetAccessDenied();
   
      CComObject<InterfaceTCPIPPorts>* pItem = new CComObject<InterfaceTCPIPPorts>();
      pItem->SetAuthentication(authentication_);
      pItem->Attach(HM::Configuration::Instance()->GetTCPIPPorts());
   
      pItem->AddRef();
      *pVal = pItem;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_SSLCertificates(IInterfaceSSLCertificates **pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      if (!GetIsServerAdmin())
         return GetAccessDenied();
   
      std::shared_ptr<HM::SSLCertificates> pSSLCertificates = HM::Configuration::Instance()->GetSSLCertificates();
   
      CComObject<InterfaceSSLCertificates>* pItem = new CComObject<InterfaceSSLCertificates>();
      pItem->SetAuthentication(authentication_);
      pItem->Attach(pSSLCertificates);
   
      pItem->AddRef();
      *pVal = pItem;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_RuleLoopLimit(long *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetSMTPConfiguration()->GetRuleLoopLimit();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_RuleLoopLimit(long newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      config_->GetSMTPConfiguration()->SetRuleLoopLimit(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_DefaultDomain(BSTR *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetDefaultDomain().AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_DefaultDomain(BSTR newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      config_->SetDefaultDomain(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_IMAPPublicFolderName(BSTR *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetIMAPConfiguration()->GetIMAPPublicFolderName().AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_IMAPPublicFolderName(BSTR newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      config_->GetIMAPConfiguration()->SetIMAPPublicFolderName(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_SMTPDeliveryBindToIP(BSTR *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetSMTPConfiguration()->GetSMTPDeliveryBindToIP().AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_SMTPDeliveryBindToIP(BSTR newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      config_->GetSMTPConfiguration()->SetSMTPDeliveryBindToIP(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_IMAPSortEnabled(VARIANT_BOOL *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetIMAPConfiguration()->GetUseIMAPSort() ? VARIANT_TRUE : VARIANT_FALSE;
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_IMAPSortEnabled(VARIANT_BOOL newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      config_->GetIMAPConfiguration()->SetUseIMAPSort(newVal == VARIANT_TRUE);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_IMAPQuotaEnabled(VARIANT_BOOL *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetIMAPConfiguration()->GetUseIMAPQuota() ? VARIANT_TRUE : VARIANT_FALSE;
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_IMAPQuotaEnabled(VARIANT_BOOL newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      config_->GetIMAPConfiguration()->SetUseIMAPQuota(newVal == VARIANT_TRUE);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_IMAPIdleEnabled(VARIANT_BOOL *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetIMAPConfiguration()->GetUseIMAPIdle() ? VARIANT_TRUE : VARIANT_FALSE;
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_IMAPIdleEnabled(VARIANT_BOOL newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      config_->GetIMAPConfiguration()->SetUseIMAPIdle(newVal == VARIANT_TRUE);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_IMAPACLEnabled(VARIANT_BOOL *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetIMAPConfiguration()->GetUseIMAPACL() ? VARIANT_TRUE : VARIANT_FALSE;
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_IMAPACLEnabled(VARIANT_BOOL newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      config_->GetIMAPConfiguration()->SetUseIMAPACL(newVal == VARIANT_TRUE);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_WorkerThreadPriority(long *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetWorkerThreadPriority();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_WorkerThreadPriority(long newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      config_->SetWorkerThreadPriority(newVal);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_TCPIPThreads(long *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetTCPIPThreads();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_TCPIPThreads(long newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      config_->SetTCPIPThreads(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_MaxAsynchronousThreads(long *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetAsynchronousThreads();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_MaxAsynchronousThreads(long newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      config_->SetAsynchronousThreads(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_CrashSimulationMode(long *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetCrashSimulationMode();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_CrashSimulationMode(long newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      config_->SetCrashSimulationMode(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}


STDMETHODIMP InterfaceSettings::get_MaxSMTPRecipientsInBatch(long *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetSMTPConfiguration()->GetMaxSMTPRecipientsInBatch();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_MaxSMTPRecipientsInBatch(long newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      config_->GetSMTPConfiguration()->SetMaxSMTPRecipientsInBatch(newVal);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_DisconnectInvalidClients(VARIANT_BOOL *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetDisconnectInvalidClients() ? VARIANT_TRUE : VARIANT_FALSE;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_DisconnectInvalidClients(VARIANT_BOOL newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      config_->SetDisconnectInvalidClients(newVal == VARIANT_TRUE);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_MaxNumberOfInvalidCommands(long *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetMaximumIncorrectCommands();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_MaxNumberOfInvalidCommands(long newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      config_->SetMaximumIncorrectCommands(newVal);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_SMTPRelayerUseSSL(VARIANT_BOOL *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetSMTPConfiguration()->GetSMTPRelayerConnectionSecurity() == HM::CSSSL? VARIANT_TRUE : VARIANT_FALSE;
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_SMTPRelayerUseSSL(VARIANT_BOOL newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      if (newVal == VARIANT_TRUE)
         config_->GetSMTPConfiguration()->SetSMTPRelayerConnectionSecurity(HM::CSSSL);
      else
         config_->GetSMTPConfiguration()->SetSMTPRelayerConnectionSecurity(HM::CSNone);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}


STDMETHODIMP InterfaceSettings::put_SMTPRelayerConnectionSecurity(eConnectionSecurity newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      config_->GetSMTPConfiguration()->SetSMTPRelayerConnectionSecurity((HM::ConnectionSecurity) newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_SMTPRelayerConnectionSecurity(eConnectionSecurity *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = (eConnectionSecurity) config_->GetSMTPConfiguration()->GetSMTPRelayerConnectionSecurity();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_SMTPConnectionSecurity(eConnectionSecurity newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      config_->GetSMTPConfiguration()->SetSMTPConnectionSecurity((HM::ConnectionSecurity) newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_SMTPConnectionSecurity(eConnectionSecurity *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = (eConnectionSecurity) config_->GetSMTPConfiguration()->GetSMTPConnectionSecurity();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}


STDMETHODIMP InterfaceSettings::get_AddDeliveredToHeader(VARIANT_BOOL *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetSMTPConfiguration()->GetAddDeliveredToHeader() ? VARIANT_TRUE : VARIANT_FALSE;
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_AddDeliveredToHeader(VARIANT_BOOL newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      config_->GetSMTPConfiguration()->SetAddDeliveredToHeader(newVal== VARIANT_TRUE);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_PublicFolders(IInterfaceIMAPFolders **pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      if (!GetIsServerAdmin())
         return GetAccessDenied();
   
      std::shared_ptr<HM::IMAPFolders> pIMAPFolders = HM::Configuration::Instance()->GetIMAPConfiguration()->GetPublicFolders();
   
      CComObject<InterfaceIMAPFolders>* pItem = new CComObject<InterfaceIMAPFolders>();
      pItem->SetAuthentication(authentication_);
      pItem->Attach(pIMAPFolders);
      pItem->AddRef();
   
      *pVal = pItem;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_PublicFolderDiskName(BSTR *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      if (!GetIsServerAdmin())
         return GetAccessDenied();
   
      *pVal = config_->GetIMAPConfiguration()->GetPublicFolderDiskName().AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_Groups(IInterfaceGroups **pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      if (!GetIsServerAdmin())
         return GetAccessDenied();
   
      CComObject<InterfaceGroups>* pItem = new CComObject<InterfaceGroups>();
      pItem->SetAuthentication(authentication_);
   
      std::shared_ptr<HM::Groups> pGroups = config_->GetIMAPConfiguration()->GetGroups();
      pGroups->Refresh();
   
      if (pGroups)
      {
         pItem->Attach(pGroups);
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

STDMETHODIMP InterfaceSettings::get_IncomingRelays(IInterfaceIncomingRelays **pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      if (!GetIsServerAdmin())
         return authentication_->GetAccessDenied();
   
      CComObject<InterfaceIncomingRelays>* pItem = new CComObject<InterfaceIncomingRelays>();
      pItem->SetAuthentication(authentication_);
   
      std::shared_ptr<HM::IncomingRelays> incomingRelays = config_->GetSMTPConfiguration()->GetIncomingRelays();
      
      if (incomingRelays)
      {
         pItem->Attach(incomingRelays);
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

STDMETHODIMP InterfaceSettings::get_MessageIndexing(IInterfaceMessageIndexing **pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      if (!GetIsServerAdmin())
         return authentication_->GetAccessDenied();
   
      CComObject<InterfaceMessageIndexing>* pItem = new CComObject<InterfaceMessageIndexing>();
      pItem->SetAuthentication(authentication_);
      pItem->LoadSettings();
      pItem->AddRef();
   
      *pVal = pItem;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_AutoBanOnLogonFailure(VARIANT_BOOL *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetAutoBanLogonEnabled() ? VARIANT_TRUE : VARIANT_FALSE;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_AutoBanOnLogonFailure(VARIANT_BOOL newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      config_->SetAutoBanLogonEnabled(newVal == VARIANT_TRUE);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_MaxInvalidLogonAttempts(long *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetMaxInvalidLogonAttempts();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_MaxInvalidLogonAttempts(long newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      config_->SetMaxInvalidLogonAttempts(newVal);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_MaxInvalidLogonAttemptsWithin(long *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetMaxLogonAttemptsWithin();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_MaxInvalidLogonAttemptsWithin(long newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      config_->SetMaxLogonAttemptsWithin(newVal);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_AutoBanMinutes(long *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetAutoBanMinutes();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_AutoBanMinutes(long newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      config_->SetAutoBanMinutes(newVal);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::ClearLogonFailureList()
{
   try
   {
      if (!config_)
         return NotLoaded_();

      if (!GetIsServerAdmin())
         return GetAccessDenied();
   
      // hm_logon_failures is traffic, so the statement chokepoint does not
      // record the delete; clearing it is an unlock all the same, and is
      // recorded here under the actor GetIsServerAdmin installed, as the REST
      // route records it.
      if (config_->ClearOldLogonFailures())
      {
         HM::AuditTrail::Instance()->RecordEvent(_T("logon failures"), _T("every recorded failure"), _T("cleared"),
            _T("the failures the auto-ban counts were deleted, so every address starts again at none"));
      }
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_IMAPHierarchyDelimiter(BSTR *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetIMAPConfiguration()->GetHierarchyDelimiter().AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_IMAPHierarchyDelimiter(BSTR newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      if (!config_->GetIMAPConfiguration()->SetHierarchyDelimiter(newVal))
         return COMError::GenerateError("It was not possible to change the IMAP hierarchy delimiter. It has probably failed because there exists one or more IMAP folders containing the new character.");
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_MaxNumberOfMXHosts(long *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetSMTPConfiguration()->GetMaxNumberOfMXHosts();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_MaxNumberOfMXHosts(long newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      config_->GetSMTPConfiguration()->SetMaxNumberOfMXHosts(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}


STDMETHODIMP InterfaceSettings::get_VerifyRemoteSslCertificate(VARIANT_BOOL *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();


      if (config_->GetVerifyRemoteSslCertificate())
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

STDMETHODIMP InterfaceSettings::put_VerifyRemoteSslCertificate(VARIANT_BOOL newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      config_->SetVerifyRemoteSslCertificate(newVal == VARIANT_TRUE);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_SslCipherList(BSTR *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetSslCipherList().AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_SslCipherList(BSTR newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      config_->SetSslCipherList(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_TlsVersion10Enabled(VARIANT_BOOL newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      config_->SetSslVersionEnabled(HM::TlsVersion10, newVal == VARIANT_TRUE);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}


STDMETHODIMP InterfaceSettings::get_TlsVersion10Enabled(VARIANT_BOOL *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetSslVersionEnabled(HM::TlsVersion10) ? VARIANT_TRUE : VARIANT_FALSE;
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_TlsVersion11Enabled(VARIANT_BOOL newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      config_->SetSslVersionEnabled(HM::TlsVersion11, newVal == VARIANT_TRUE);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}


STDMETHODIMP InterfaceSettings::get_TlsVersion11Enabled(VARIANT_BOOL *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetSslVersionEnabled(HM::TlsVersion11) ? VARIANT_TRUE : VARIANT_FALSE;
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}


STDMETHODIMP InterfaceSettings::put_TlsVersion12Enabled(VARIANT_BOOL newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      config_->SetSslVersionEnabled(HM::TlsVersion12, newVal == VARIANT_TRUE);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}


STDMETHODIMP InterfaceSettings::get_TlsVersion12Enabled(VARIANT_BOOL *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetSslVersionEnabled(HM::TlsVersion12) ? VARIANT_TRUE : VARIANT_FALSE;
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}


STDMETHODIMP InterfaceSettings::put_TlsVersion13Enabled(VARIANT_BOOL newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      config_->SetSslVersionEnabled(HM::TlsVersion13, newVal == VARIANT_TRUE);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}


STDMETHODIMP InterfaceSettings::get_TlsVersion13Enabled(VARIANT_BOOL *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetSslVersionEnabled(HM::TlsVersion13) ? VARIANT_TRUE : VARIANT_FALSE;
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_TlsOptionPreferServerCiphersEnabled(VARIANT_BOOL newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      config_->SetTlsOptionEnabled(HM::TlsOptionPreferServerCiphers, newVal == VARIANT_TRUE);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_TlsOptionPreferServerCiphersEnabled(VARIANT_BOOL *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetTlsOptionEnabled(HM::TlsOptionPreferServerCiphers) ? VARIANT_TRUE : VARIANT_FALSE;
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_TlsOptionPrioritizeChaChaEnabled(VARIANT_BOOL newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      config_->SetTlsOptionEnabled(HM::TlsOptionPrioritizeChaCha, newVal == VARIANT_TRUE);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_TlsOptionPrioritizeChaChaEnabled(VARIANT_BOOL *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetTlsOptionEnabled(HM::TlsOptionPrioritizeChaCha) ? VARIANT_TRUE : VARIANT_FALSE;
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_IMAPMasterUser(BSTR *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();


      *pVal = config_->GetIMAPConfiguration()->GetIMAPMasterUser().AllocSysString();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_IMAPMasterUser(BSTR newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      HM::String sNewVal = newVal;
      config_->GetIMAPConfiguration()->SetIMAPMasterUser(sNewVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_IMAPSASLPlainEnabled(VARIANT_BOOL newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      config_->GetIMAPConfiguration()->SetUseIMAPSASLPlain(newVal == VARIANT_TRUE);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_IMAPSASLPlainEnabled(VARIANT_BOOL *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetIMAPConfiguration()->GetUseIMAPSASLPlain() ? VARIANT_TRUE : VARIANT_FALSE;
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_IMAPSASLInitialResponseEnabled(VARIANT_BOOL newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      config_->GetIMAPConfiguration()->SetUseIMAPSASLInitialResponse(newVal == VARIANT_TRUE);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}


STDMETHODIMP InterfaceSettings::get_IMAPSASLInitialResponseEnabled(VARIANT_BOOL *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetIMAPConfiguration()->GetUseIMAPSASLInitialResponse() ? VARIANT_TRUE : VARIANT_FALSE;
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_IPv6PreferredEnabled(VARIANT_BOOL *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetIPv6Preferred() ? VARIANT_TRUE : VARIANT_FALSE;

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_IPv6PreferredEnabled(VARIANT_BOOL newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      config_->SetIPv6Preferred(newVal == VARIANT_TRUE);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_RewriteEnvelopeFromWhenForwarding(VARIANT_BOOL *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = ini_file_settings_->GetRewriteEnvelopeFromWhenForwarding() ? VARIANT_TRUE : VARIANT_FALSE;

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_RewriteEnvelopeFromWhenForwarding(VARIANT_BOOL newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      ini_file_settings_->SetRewriteEnvelopeFromWhenForwarding(newVal == VARIANT_TRUE);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}
STDMETHODIMP InterfaceSettings::GetIniSetting(BSTR Name, BSTR *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      if (!pVal)
         return COMError::GenerateGenericMessage();

      HM::String name = Name;

      // Refused rather than answered with an empty string. A name this store could
      // never have written is a caller mistake, and "" is indistinguishable from a
      // setting that is genuinely absent - which is exactly the ambiguity that makes
      // a misspelled key look like a working one.
      if (!HM::IniSettingStore::IsStorableName(name))
         return COMError::GenerateError("The setting name is empty, longer than 100 characters, surrounded by whitespace, or contains one of = [ ] or a line break.");

      HM::String value = HM::IniFileSettings::Instance()->GetSettingsValue(name);

      // A value a file of the server's environment supplied - a Secret's - is
      // answered as GET /api/v1/settings/ini/{name} answers it: (set), or empty
      // when it is empty, until an administrator writes another
      // (docs/EnvironmentConfiguration.md).
      if (HM::EnvironmentSettings::IsWithheld(HM::EnvironmentSettings::StoreIni, name) && !value.IsEmpty())
         value = _T("(set)");

      *pVal = value.AllocSysString();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::SetIniSetting(BSTR Name, BSTR Value)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      HM::String name = Name;
      HM::String value = Value;

      if (!HM::IniSettingStore::IsStorableName(name))
         return COMError::GenerateError("The setting name is empty, longer than 100 characters, surrounded by whitespace, or contains one of = [ ] or a line break.");

      if (!HM::IniSettingStore::IsStorableValue(value))
         return COMError::GenerateError("The value is longer than 4000 characters or contains a line break, so it could not be stored or read back as one setting.");

      if (!HM::IniFileSettings::Instance()->WriteSettingsValue(name, value))
         return COMError::GenerateError("The setting could not be stored. Settings are held in the database, so this means the server could not write to it. Nothing has been changed.");

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::DeleteIniSetting(BSTR Name)
{
   try
   {
      // Returning a setting to its default is a change like any other, and it is
      // recorded with the value it had - which needs the actor on THIS thread, not
      // on whichever one asked GetIsServerAdmin when the object was loaded.
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      HM::String name = Name;

      if (!HM::IniSettingStore::IsStorableName(name))
         return COMError::GenerateError("The setting name is empty, longer than 100 characters, surrounded by whitespace, or contains one of = [ ] or a line break.");

      if (!HM::IniFileSettings::Instance()->RemoveSettingsValue(name))
         return COMError::GenerateError("The setting could not be removed. Settings are held in the database, so this means the server could not write to it. Nothing has been changed.");

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_IniSettingNames(BSTR *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      if (!pVal)
         return COMError::GenerateGenericMessage();

      std::vector<HM::String> names;
      HM::IniFileSettings::Instance()->GetSettingsNames(names);

      // One per line, because a name cannot contain a line break - IsStorableName
      // refuses one - so the separator can never appear inside a value of this list.
      HM::String result = HM::StringParser::JoinVector(names, _T("\r\n"));

      *pVal = result.AllocSysString();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::IniSettingApplies(BSTR Name, BSTR *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      if (!pVal)
         return COMError::GenerateGenericMessage();

      // As spelled, not trimmed: GetIniSetting refuses a name with whitespace round
      // it, and the REST route takes the name from the path as it is - so a name
      // with a space round it is no key here either, and is refused below.
      HM::String name = Name;

      // Refused rather than answered "now" for a name the loader does not read: a
      // misspelled key answered as though it applied would be the misspelling
      // looking like a working setting, which is what GetIniSetting refuses for too.
      // The same sentence the REST answers carry as applies_refused.
      HM::String applies;
      HM::String spelling;
      if (!HM::IniFileSettings::GetApplies(name, applies, spelling))
         return COMError::GenerateError(HM::IniFileSettings::NoApplyMarkSentence());

      *pVal = applies.AllocSysString();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_PendingIniSettings(BSTR *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      if (!pVal)
         return COMError::GenerateGenericMessage();

      std::vector<HM::IniFileSettings::PendingSetting> pending = HM::IniFileSettings::Instance()->GetPendingSettings();

      // One per line, as IniSettingNames: a name cannot contain a line break.
      std::vector<HM::String> names;
      for (size_t i = 0; i < pending.size(); i++)
         names.push_back(pending[i].name);

      *pVal = HM::StringParser::JoinVector(names, _T("\r\n")).AllocSysString();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_IniSettingAppliesAll(BSTR *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      if (!pVal)
         return COMError::GenerateGenericMessage();

      // One JSON object, name -> mark, for every key the loader reads: the applies
      // map GET /api/v1/settings/ini answers. The names are the baseline's own - a
      // letter, digit or underscore each - and the marks three plain words, so
      // nothing in it needs escaping.
      std::vector<std::pair<HM::String, HM::String> > marks;
      HM::IniFileSettings::GetAllApplies(marks);

      HM::String json = _T("{");
      for (size_t i = 0; i < marks.size(); i++)
      {
         if (i > 0)
            json += _T(",");
         json += _T("\"") + marks[i].first + _T("\":\"") + marks[i].second + _T("\"");
      }
      json += _T("}");

      *pVal = json.AllocSysString();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_QuarantineDigestBaseUrl(BSTR *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetQuarantineDigestBaseUrl().AllocSysString();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_QuarantineDigestBaseUrl(BSTR newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      HM::String url = newVal;
      url.Trim();

      // Refused rather than stored and ignored. A value that is not an absolute
      // http or https URL silently switches the whole digest off - the sender
      // checks the same thing and sends nothing - and an administrator who typed
      // "mail.example.com" would be left with a feature that is on in every
      // domain and never does anything.
      if (!url.IsEmpty())
      {
         HM::String lowered = url;
         lowered.MakeLower();

         if (lowered.Find(_T("http://")) != 0 && lowered.Find(_T("https://")) != 0)
            return COMError::GenerateError("The base URL is absolute and begins with http:// or https://, such as https://mail.example.com.");
      }

      config_->SetQuarantineDigestBaseUrl(url);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

// COUNTRY BLOCKING. The grammar and every refusal sentence are
// CountryBlocking's, so that this property, the REST settings group, hmctl and
// the Control Deck all refuse a bad policy in the same words - and so that a
// policy is stored in one spelling however it was typed.

STDMETHODIMP InterfaceSettings::get_GeoIpDatabaseFile(BSTR *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetGeoIpDatabaseFile().AllocSysString();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_GeoIpDatabaseFile(BSTR newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      HM::String path = newVal;
      path.Trim();

      // Not refused for being missing: an administrator may name the file before
      // the download lands, and a server that refused the name would make that
      // order impossible. Whether it could be read is what
      // Utilities.GeoIpDatabaseStatus answers, and the application log says so
      // the first time the file is wanted.
      config_->SetGeoIpDatabaseFile(path);

      // The next question loads the new file rather than the old one.
      HM::CountryBlocking::Instance()->Invalidate();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_GeoIpDatabaseMaxAgeDays(long *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = (long) config_->GetGeoIpDatabaseMaxAgeDays();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_GeoIpDatabaseMaxAgeDays(long newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      if (newVal < 0 || newVal > 3650)
         return COMError::GenerateError("GeoIpDatabaseMaxAgeDays is between 0 and 3650 days. 0 never calls the database stale.");

      config_->SetGeoIpDatabaseMaxAgeDays((int) newVal);
      HM::CountryBlocking::Instance()->Invalidate();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_CountryBlockingExemptRanges(BSTR *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetCountryBlockingExemptRanges().AllocSysString();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_CountryBlockingExemptRanges(BSTR newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      HM::String ranges = newVal;
      ranges.Trim();

      config_->SetCountryBlockingExemptRanges(ranges);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_CountryPolicy(BSTR Protocol, BSTR *pVal)
{
   try
   {
      if (!pVal)
         return COMError::GenerateGenericMessage();

      if (!config_)
         return NotLoaded_();

      HM::CountryBlocking::Surface surface;

      if (!HM::CountryBlocking::SurfaceFromName(HM::String(Protocol), surface))
         return COMError::GenerateError("The protocol is smtp, imap, pop3 or http.");

      *pVal = HM::CountryBlocking::FormatPolicy(HM::CountryBlocking::Instance()->PolicyFor(surface)).AllocSysString();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_CountryPolicy(BSTR Protocol, BSTR newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      HM::CountryBlocking::Surface surface;

      if (!HM::CountryBlocking::SurfaceFromName(HM::String(Protocol), surface))
         return COMError::GenerateError("The protocol is smtp, imap, pop3 or http.");

      HM::String canonical;
      HM::String error;

      if (!HM::CountryBlocking::Canonicalise(HM::String(newVal), canonical, error))
         return COMError::GenerateError(error);

      switch (surface)
      {
      case HM::CountryBlocking::SurfaceSmtp:
         config_->SetCountryPolicySMTP(canonical);
         break;
      case HM::CountryBlocking::SurfaceImap:
         config_->SetCountryPolicyIMAP(canonical);
         break;
      case HM::CountryBlocking::SurfacePop3:
         config_->SetCountryPolicyPOP3(canonical);
         break;
      default:
         config_->SetCountryPolicyHTTP(canonical);
         break;
      }

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_QuarantineDigestLinkHours(long *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = (long) config_->GetQuarantineDigestLinkHours();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_QuarantineDigestLinkHours(long newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      if (newVal < 1 || newVal > 720)
         return COMError::GenerateError("A release link lives between 1 and 720 hours.");

      config_->SetQuarantineDigestLinkHours((int) newVal);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_ReputationWatchEnabled(VARIANT_BOOL *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetReputationWatchEnabled() ? VARIANT_TRUE : VARIANT_FALSE;

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_ReputationWatchEnabled(VARIANT_BOOL newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      config_->SetReputationWatchEnabled(newVal == VARIANT_TRUE);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_ReputationWatchLists(BSTR *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetReputationWatchLists().AllocSysString();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_ReputationWatchLists(BSTR newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      // The same sentence the REST settings and the Deck show: a list that
      // cannot be used is refused where it is typed, rather than stored and
      // skipped every night with a line in a log nobody reads.
      const HM::String refusal = HM::ReputationWatch::ValidateLists(newVal);

      if (!refusal.IsEmpty())
         return COMError::GenerateError(refusal);

      config_->SetReputationWatchLists(newVal);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_ReputationWatchAddresses(BSTR *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetReputationWatchAddresses().AllocSysString();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_ReputationWatchAddresses(BSTR newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      const HM::String refusal = HM::ReputationWatch::ValidateAddresses(newVal);

      if (!refusal.IsEmpty())
         return COMError::GenerateError(refusal);

      config_->SetReputationWatchAddresses(newVal);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_HelpWikiBaseUrl(BSTR *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetHelpWikiBaseUrl().AllocSysString();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_HelpWikiBaseUrl(BSTR newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      HM::String url = newVal;
      url.Trim();

      // Refused rather than stored and shown as a link that goes nowhere: the
      // same sentence the REST row uses, so the Control Panel and the Deck
      // refuse a value in the same words.
      if (!url.IsEmpty())
      {
         HM::String lowered = url;
         lowered.MakeLower();

         if (lowered.Find(_T("http://")) != 0 && lowered.Find(_T("https://")) != 0)
            return COMError::GenerateError("The wiki base URL is absolute and begins with http:// or https://, such as https://gitlab.com/Progressiverobot/hmailserver/-/wikis/; empty hides the link.");
      }

      config_->SetHelpWikiBaseUrl(url);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::TestLdapDirectory(long MaxUsers, BSTR *ResultText, VARIANT_BOOL *pResult)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      if (!ResultText || !pResult)
         return COMError::GenerateGenericMessage();

      *pResult = VARIANT_FALSE;

      const HM::LdapConfiguration configuration = HM::LdapSettings::Instance()->GetConfiguration();

      if (!configuration.enabled)
      {
         *ResultText = HM::String("[LDAP] Enabled is 0, so directory access is switched off. Nothing was contacted.").AllocSysString();
         return S_OK;
      }

      HM::String missing;

      if (!configuration.IsComplete(missing))
      {
         *ResultText = HM::String(HM::Formatter::Format("The [LDAP] configuration is incomplete: {0}. Nothing was contacted.", missing)).AllocSysString();
         return S_OK;
      }

      if (configuration.search_base.IsEmpty())
      {
         // Enumeration needs a base to search from. Authentication can get by with a
         // UserDnTemplate and never search at all, so this is not covered by
         // IsComplete - but an account source with no search base has nothing to read.
         //
         // Reached only when UserDnTemplate is set or BindMethod is Negotiate; in the
         // plain search configuration IsComplete above already refuses, naming both
         // SearchBase and UserDnTemplate as the two ways out.
         *ResultText = HM::String("[LDAP] SearchBase is empty. Authentication can work without it when UserDnTemplate is set, but reading the directory as an account source cannot: there is nowhere to search. Nothing was contacted.").AllocSysString();
         return S_OK;
      }

      // Checked here, before connecting, so that hMailServer's OWN refusal to send a
      // credential is never reported as the directory refusing it.
      //
      // BindSimple and BindNegotiate both refuse an empty password locally, and
      // BindSimple also refuses to put a password on an unprotected connection - all
      // three return an LDAP result code without a packet leaving the machine. Left to
      // fall through, those arrive at the "Connected, but the service credential was
      // refused" message below, which asserts that the directory answered and points
      // the administrator at the domain controller for a decision this server made.
      if (!configuration.service_username.IsEmpty() && configuration.service_password.IsEmpty())
      {
         *ResultText = HM::String("[LDAP] ServiceUsername is set but ServicePassword is empty. hMailServer refuses to bind with an empty password rather than sending one, so the directory was not contacted.").AllocSysString();
         return S_OK;
      }

      // Mirrors BindSimple's own guard exactly - `!transport_protected_ &&
      // !unprotected_password_allowed_` - rather than PasswordIsProtected(), which
      // does not account for AllowUnprotectedPassword and would refuse a
      // configuration the bind itself would have accepted.
      if (configuration.bind_method == HM::LdapBindMethod::BindSimple &&
          !configuration.service_username.IsEmpty() &&
          configuration.security == HM::LdapTransportSecurity::TransportPlain &&
          !configuration.allow_unprotected_password)
      {
         *ResultText = HM::String("[LDAP] the service credential would be sent as a simple bind over an unprotected connection, which hMailServer refuses. Use Security=2 (LDAPS), or BindMethod=1 (Negotiate, which never puts the password on the wire), or set AllowUnprotectedPassword=1 if the network genuinely is trusted. The directory was not contacted.").AllocSysString();
         return S_OK;
      }

      HM::LdapClient client;

      if (client.Connect(configuration) != HM::LdapOperationOutcome::OutcomeSuccess)
      {
         *ResultText = HM::String(HM::Formatter::Format("Could not connect to the directory: {0}", client.DescribeLastError())).AllocSysString();
         return S_OK;
      }

      if (client.BindService(configuration) != HM::LdapOperationOutcome::OutcomeSuccess)
      {
         HM::String diagnostic = client.GetLastDiagnostic();
         HM::String substatus = HM::LdapClient::DescribeActiveDirectorySubStatus(diagnostic);

         *ResultText = HM::String(HM::Formatter::Format("Connected, but the service credential was refused: {0}{1}",
            client.DescribeLastError(), substatus.IsEmpty() ? HM::String() : HM::String(" - ") + substatus)).AllocSysString();
         return S_OK;
      }

      // A ceiling the caller can lower for a quick look, but never raise past the
      // configured one - that bound exists so a misconfigured search base cannot be
      // answered by allocating until the process dies.
      int limit = MaxUsers > 0 && MaxUsers < configuration.sync_max_users
         ? (int) MaxUsers
         : configuration.sync_max_users;

      std::vector<HM::String> attributes;
      attributes.push_back(configuration.sync_mail_attribute);
      attributes.push_back(configuration.sync_username_attribute);
      attributes.push_back(configuration.sync_display_name_attribute);

      std::vector<HM::LdapDirectoryEntry> entries;
      bool truncated = false;

      if (client.SearchEntries(configuration.search_base, configuration.sync_filter,
             attributes, limit, entries, truncated) != HM::LdapOperationOutcome::OutcomeSuccess)
      {
         *ResultText = HM::String(HM::Formatter::Format("The search failed: {0}. Base: {1}. Filter: {2}",
            client.DescribeLastError(), configuration.search_base, configuration.sync_filter)).AllocSysString();
         return S_OK;
      }

      HM::String report = HM::Formatter::Format("Read {0} directory entr{1} from {2}.\r\nFilter: {3}\r\n",
         HM::StringParser::IntToString((int) entries.size()),
         entries.size() == 1 ? HM::String("y") : HM::String("ies"),
         configuration.search_base, configuration.sync_filter);

      if (truncated)
      {
         report += HM::Formatter::Format("\r\nSTOPPED AT {0} ENTRIES - the directory holds more. This is a partial view, and an account source run against it would not see the rest.\r\n",
            HM::StringParser::IntToString(limit));
      }

      int withoutAddress = 0;

      report += _T("\r\n");

      for (const HM::LdapDirectoryEntry &entry : entries)
      {
         // Preferred, not first, and for a reason beyond determinism: this card exists to
         // show an administrator which addresses provisioning would use, so it has to
         // choose the value by the same rule DirectorySync does. Reading the first value
         // here while the sync reads the preferred one would make the card advertise one
         // address and the apply create another.
         const HM::String address = entry.PreferredAddress(configuration.sync_mail_attribute);
         const HM::String username = entry.Preferred(configuration.sync_username_attribute);
         const HM::String display = entry.Preferred(configuration.sync_display_name_attribute);

         if (address.IsEmpty())
            withoutAddress++;

         report += HM::Formatter::Format("{0}\t{1}\t{2}\r\n",
            address.IsEmpty() ? HM::String("(no address)") : address,
            username.IsEmpty() ? HM::String("(no user name)") : username,
            display);
      }

      if (withoutAddress > 0)
      {
         // Named rather than filtered out silently. An entry the filter selected but
         // which carries no address cannot become a mailbox, and an administrator
         // seeing "12 users" and getting 9 mailboxes deserves to know why here rather
         // than afterwards.
         report += HM::Formatter::Format("\r\n{0} of these carry no {1} attribute, so an account source could not provision them.\r\n",
            HM::StringParser::IntToString(withoutAddress), configuration.sync_mail_attribute);
      }

      *ResultText = report.AllocSysString();
      *pResult = VARIANT_TRUE;

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

HRESULT InterfaceSettings::RunDirectorySync_(BSTR DomainName, bool apply, bool disableMissing,
   BSTR *ResultText, VARIANT_BOOL *pResult)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      if (!ResultText || !pResult)
         return COMError::GenerateGenericMessage();

      *pResult = VARIANT_FALSE;

      HM::DirectorySyncOptions options;
      options.mode = apply ? HM::DirectorySyncMode::ModeApply : HM::DirectorySyncMode::ModePreview;
      // NOT gated on apply. A preview asked to show a disabling run must show it,
      // otherwise the preview and the apply are answering different questions - which
      // is the one thing the shared decider is there to prevent.
      options.disable_missing = disableMissing;
      options.domain_name = HM::String(DomainName);
      options.domain_name.Trim();

      const HM::DirectorySyncResult result = HM::DirectorySync::Run(options);

      // The summary first, because it is the sentence an administrator reads, and
      // then a line per entry - which is the part that turns "22 skipped" into
      // something anyone can act on.
      HM::String report = result.Summary();

      if (!result.entries.empty())
      {
         report += _T("\r\n\r\n");

         for (const HM::DirectorySyncEntry &entry : result.entries)
         {
            // A FOURTH field, carrying what became of this row, and it is not
            // decoration. Without it the action word is all a caller has, and after an
            // apply "create" means both "this account was created" and "this account
            // was going to be created and the database refused" - which are the two
            // outcomes an administrator most needs to tell apart. The Control Panel
            // counted the second as a success and could print "nothing here needs a
            // decision" over a run in which nothing was created at all.
            //
            // "planned" for a preview, where applied is false for every row and means
            // nothing. Written for both modes rather than only for apply, so a reader -
            // human or machine - never has to know which call produced the text in
            // order to know what the fourth column means.
            //
            // Only the three ACTED-ON actions can be "done" or "failed". Apply_ has no
            // case for unchanged, skip or no-longer-in-the-directory: it does nothing
            // to them by design, so their applied flag stays false and reading it as
            // failure would report every correctly-skipped entry as an error. On a
            // directory where most accounts already match, that is almost every row.
            const bool actedOn =
               entry.action == HM::DirectorySyncAction::ActionCreate ||
               entry.action == HM::DirectorySyncAction::ActionUpdate ||
               entry.action == HM::DirectorySyncAction::ActionDisable;

            HM::String state = _T("planned");

            if (options.mode == HM::DirectorySyncMode::ModeApply)
            {
               if (!actedOn)
                  state = _T("noop");
               else
                  state = entry.applied ? HM::String(_T("done")) : HM::String(_T("failed"));
            }

            report += HM::Formatter::Format("{0}\t{1}\t{2}\t{3}\r\n",
               entry.address.IsEmpty() ? HM::String("(no address)") : entry.address,
               HM::DirectorySync::DescribeAction(entry.action),
               entry.error.IsEmpty() ? entry.detail : entry.error,
               state);
         }
      }

      // The breakdown of WHY things were skipped. Reported separately from the entry
      // list because on a large directory the list is long and the counts are what
      // gets read - and "22 in a domain that is not on this server" is the difference
      // between a puzzle and a fix.
      if (result.skipped > 0)
      {
         report += _T("\r\nSkipped, by reason:\r\n");

         for (auto iter = result.skip_reasons.begin(); iter != result.skip_reasons.end(); iter++)
         {
            if ((*iter).second <= 0)
               continue;

            report += HM::Formatter::Format("  {0}\t{1}\r\n",
               (*iter).second, HM::DirectorySync::DescribeSkipReason((*iter).first));
         }
      }

      *ResultText = report.AllocSysString();
      *pResult = result.succeeded ? VARIANT_TRUE : VARIANT_FALSE;

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::PreviewDirectorySync(BSTR DomainName, VARIANT_BOOL DisableMissing,
   BSTR *ResultText, VARIANT_BOOL *pResult)
{
   return RunDirectorySync_(DomainName, false, DisableMissing == VARIANT_TRUE, ResultText, pResult);
}

STDMETHODIMP InterfaceSettings::ApplyDirectorySync(BSTR DomainName, VARIANT_BOOL DisableMissing,
   BSTR *ResultText, VARIANT_BOOL *pResult)
{
   return RunDirectorySync_(DomainName, true, DisableMissing == VARIANT_TRUE, ResultText, pResult);
}

STDMETHODIMP InterfaceSettings::get_CreateDefaultSpecialUseFoldersEnabled(VARIANT_BOOL *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetCreateDefaultSpecialUseFolders() ? VARIANT_TRUE : VARIANT_FALSE;

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_CreateDefaultSpecialUseFoldersEnabled(VARIANT_BOOL newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      config_->SetCreateDefaultSpecialUseFolders(newVal == VARIANT_TRUE);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

// The password breach check. The puts refuse what the check could not use, in the
// sentence the Control Panel shows, before anything is stored - so the value
// readable afterwards is always one the check accepts.

STDMETHODIMP InterfaceSettings::get_BreachCheckEnabled(VARIANT_BOOL *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetBreachCheckEnabled() ? VARIANT_TRUE : VARIANT_FALSE;

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_BreachCheckEnabled(VARIANT_BOOL newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      config_->SetBreachCheckEnabled(newVal == VARIANT_TRUE);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_BreachCheckUrl(BSTR *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetBreachCheckUrl().AllocSysString();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_BreachCheckUrl(BSTR newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      HM::String url = newVal;
      HM::String reason;
      if (!HM::PasswordBreachCheck::ValidateEndpoint(url, reason))
         return COMError::GenerateError(reason);

      config_->SetBreachCheckUrl(url);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_BreachCheckMinimumCount(long *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetBreachCheckMinimumCount();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_BreachCheckMinimumCount(long newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      if (newVal < 1)
         return COMError::GenerateError("BreachCheckMinimumCount must be 1 or more: a password is refused when it has appeared in the corpus at least this many times.");

      config_->SetBreachCheckMinimumCount(newVal);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_BreachCheckUnreachableAction(long *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetBreachCheckUnreachableAction();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_BreachCheckUnreachableAction(long newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      if (newVal != HM::PasswordBreachCheck::UnreachableAllow && newVal != HM::PasswordBreachCheck::UnreachableRefuse)
         return COMError::GenerateError("BreachCheckUnreachableAction must be 0 (let the change through) or 1 (refuse it).");

      config_->SetBreachCheckUnreachableAction(newVal);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::TestBreachCheckEndpoint(BSTR Url, long *Entries)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      if (!GetIsServerAdmin())
         return GetAccessDenied();

      int status = 0;
      int entries = 0;
      bool padded = false;
      int milliseconds = 0;
      HM::String error;

      // Changes nothing: a request for the prefix 00000, which is no password's.
      if (!HM::PasswordBreachCheck::Probe(HM::String(Url), status, entries, padded, milliseconds, error))
         return COMError::GenerateError(error);

      *Entries = entries;

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_BounceProcessingEnabled(VARIANT_BOOL *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetBounceProcessingEnabled() ? VARIANT_TRUE : VARIANT_FALSE;

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_BounceProcessingEnabled(VARIANT_BOOL newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      config_->SetBounceProcessingEnabled(newVal == VARIANT_TRUE);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_BounceHardBounceThreshold(long *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetBounceHardBounceThreshold();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_BounceHardBounceThreshold(long newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      // Refused rather than clamped, so a caller that asked for a value outside the
      // range learns that it did not get it.
      if (newVal < 1 || newVal > 100)
         return COMError::GenerateError("BounceHardBounceThreshold must be between 1 and 100.");

      config_->SetBounceHardBounceThreshold((int) newVal);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_BounceMailboxFullIsHard(VARIANT_BOOL *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetBounceMailboxFullIsHard() ? VARIANT_TRUE : VARIANT_FALSE;

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_BounceMailboxFullIsHard(VARIANT_BOOL newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      config_->SetBounceMailboxFullIsHard(newVal == VARIANT_TRUE);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_BounceSuppressionAction(long *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetBounceSuppressionAction();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_BounceSuppressionAction(long newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      // Refused rather than clamped, so a caller that asked for a value outside the
      // range learns that it did not get it.
      if (newVal < 0 || newVal > 2)
         return COMError::GenerateError("BounceSuppressionAction must be 0 (refuse), 1 (drop) or 2 (observe).");

      config_->SetBounceSuppressionAction((int) newVal);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_BounceSuppressionExpiryDays(long *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetBounceSuppressionExpiryDays();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_BounceSuppressionExpiryDays(long newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      // Refused rather than clamped, so a caller that asked for a value outside the
      // range learns that it did not get it.
      if (newVal < 0 || newVal > 3650)
         return COMError::GenerateError("BounceSuppressionExpiryDays must be between 0 and 3650.");

      config_->SetBounceSuppressionExpiryDays((int) newVal);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_BounceReadDeliveryReports(VARIANT_BOOL *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetBounceReadDeliveryReports() ? VARIANT_TRUE : VARIANT_FALSE;

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_BounceReadDeliveryReports(VARIANT_BOOL newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      config_->SetBounceReadDeliveryReports(newVal == VARIANT_TRUE);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_BounceCountRetentionDays(long *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetBounceCountRetentionDays();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_BounceCountRetentionDays(long newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      // Refused rather than clamped, so a caller that asked for a value outside the
      // range learns that it did not get it.
      if (newVal < 0 || newVal > 3650)
         return COMError::GenerateError("BounceCountRetentionDays must be between 0 and 3650.");

      config_->SetBounceCountRetentionDays((int) newVal);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_BounceReportWindowDays(long *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetBounceReportWindowDays();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_BounceReportWindowDays(long newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      // Refused rather than clamped, so a caller that asked for a value outside the
      // range learns that it did not get it.
      if (newVal < 1 || newVal > 30)
         return COMError::GenerateError("BounceReportWindowDays must be between 1 and 30.");

      config_->SetBounceReportWindowDays((int) newVal);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

// The Web Key Directory (docs/OpenPGP.md). The switch is off on a server that has
// never been told otherwise, and a domain and an account each have to say yes as
// well before one key is served. The three numbers below are refused rather than
// clamped for the reason above.

STDMETHODIMP InterfaceSettings::get_OpenPGPWebKeyDirectoryEnabled(VARIANT_BOOL *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetOpenPGPWebKeyDirectoryEnabled() ? VARIANT_TRUE : VARIANT_FALSE;

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_OpenPGPWebKeyDirectoryEnabled(VARIANT_BOOL newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      config_->SetOpenPGPWebKeyDirectoryEnabled(newVal == VARIANT_TRUE);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_OpenPGPKeyMaximumBytes(long *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetOpenPGPKeyMaximumBytes();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_OpenPGPKeyMaximumBytes(long newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      if (newVal < 1024 || newVal > 1048576)
         return COMError::GenerateError("OpenPGPKeyMaximumBytes must be between 1024 and 1048576.");

      config_->SetOpenPGPKeyMaximumBytes((int) newVal);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_OpenPGPKeysPerAccount(long *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetOpenPGPKeysPerAccount();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_OpenPGPKeysPerAccount(long newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      if (newVal < 1 || newVal > 64)
         return COMError::GenerateError("OpenPGPKeysPerAccount must be between 1 and 64.");

      config_->SetOpenPGPKeysPerAccount((int) newVal);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_OpenPGPRequireAddressInUserId(VARIANT_BOOL *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetOpenPGPRequireAddressInUserId() ? VARIANT_TRUE : VARIANT_FALSE;

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_OpenPGPRequireAddressInUserId(VARIANT_BOOL newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      config_->SetOpenPGPRequireAddressInUserId(newVal == VARIANT_TRUE);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_WebhooksEnabled(VARIANT_BOOL *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetWebhooksEnabled() ? VARIANT_TRUE : VARIANT_FALSE;

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_WebhooksEnabled(VARIANT_BOOL newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      config_->SetWebhooksEnabled(newVal == VARIANT_TRUE);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_WebhookMaxAttempts(long *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetWebhookMaxAttempts();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_WebhookMaxAttempts(long newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      // Refused rather than clamped, so a caller that asked for a value outside the
      // range learns that it did not get it.
      if (newVal < 1 || newVal > 20)
         return COMError::GenerateError("WebhookMaxAttempts must be between 1 and 20.");

      config_->SetWebhookMaxAttempts((int) newVal);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_WebhookMaxPerMinute(long *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetWebhookMaxPerMinute();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_WebhookMaxPerMinute(long newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      // Refused rather than clamped, so a caller that asked for a value outside the
      // range learns that it did not get it.
      if (newVal < 1 || newVal > 100000)
         return COMError::GenerateError("WebhookMaxPerMinute must be between 1 and 100000.");

      config_->SetWebhookMaxPerMinute((int) newVal);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_WebhookRetentionDays(long *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetWebhookRetentionDays();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_WebhookRetentionDays(long newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      // Refused rather than clamped, so a caller that asked for a value outside the
      // range learns that it did not get it.
      if (newVal < 0 || newVal > 3650)
         return COMError::GenerateError("WebhookRetentionDays must be between 0 (keep everything) and 3650.");

      config_->SetWebhookRetentionDays((int) newVal);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_SessionIdleSeconds(long *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetSessionIdleSeconds();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_SessionIdleSeconds(long newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      // Refused rather than clamped, in SessionPolicies' own words, so that REST,
      // COM and the command line refuse the same numbers for the same reason.
      const HM::String refusal = HM::SessionPolicies::CheckSeconds((int) newVal);
      if (!refusal.IsEmpty())
         return COMError::GenerateError(refusal);

      config_->SetSessionIdleSeconds((int) newVal);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_SessionLifetimeSeconds(long *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetSessionLifetimeSeconds();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_SessionLifetimeSeconds(long newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      const HM::String refusal = HM::SessionPolicies::CheckSeconds((int) newVal);
      if (!refusal.IsEmpty())
         return COMError::GenerateError(refusal);

      config_->SetSessionLifetimeSeconds((int) newVal);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_SessionOnePerDevice(VARIANT_BOOL *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetSessionOnePerDevice() ? VARIANT_TRUE : VARIANT_FALSE;

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_SessionOnePerDevice(VARIANT_BOOL newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      config_->SetSessionOnePerDevice(newVal == VARIANT_TRUE);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_SessionBindToAddress(VARIANT_BOOL *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetSessionBindToAddress() ? VARIANT_TRUE : VARIANT_FALSE;

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_SessionBindToAddress(VARIANT_BOOL newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      config_->SetSessionBindToAddress(newVal == VARIANT_TRUE);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_WebPushEnabled(VARIANT_BOOL *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetWebPushEnabled() ? VARIANT_TRUE : VARIANT_FALSE;

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_WebPushEnabled(VARIANT_BOOL newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      config_->SetWebPushEnabled(newVal == VARIANT_TRUE);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_WebPushSubject(BSTR *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetWebPushSubject().AllocSysString();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_WebPushSubject(BSTR newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      HM::String subject = newVal;
      subject.Trim();

      HM::String lower = subject;
      lower.MakeLower();

      // A push service is told this so that it can reach a person when this
      // server's subscriptions are failing. Anything that is not an address a
      // person can be reached at would be worse than leaving it empty, which is
      // itself allowed and means postmaster at this server's host name.
      if (!subject.IsEmpty() && !lower.StartsWith(_T("mailto:")) && !lower.StartsWith(_T("https://")))
         return COMError::GenerateError("WebPushSubject is a mailto: or https: address, or empty.");

      config_->SetWebPushSubject(subject);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_WebPushVapidPublicKey(BSTR *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetWebPushVapidPublicKey().AllocSysString();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::GenerateWebPushKeys(BSTR *PublicKey)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      HM::String refusal;
      if (!config_->GenerateWebPushVapidKeys(refusal))
         return COMError::GenerateError(refusal);

      {
         // This unit sits outside namespace HM, where the logging macro does not resolve.
         using namespace HM;
         LOG_APPLICATION("COM: a new Web Push VAPID key pair was generated. Every existing subscription is now invalid and each device must subscribe again.");
      }

      *PublicKey = config_->GetWebPushVapidPublicKey().AllocSysString();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_ComplaintProcessingEnabled(VARIANT_BOOL *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetComplaintProcessingEnabled() ? VARIANT_TRUE : VARIANT_FALSE;

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_ComplaintProcessingEnabled(VARIANT_BOOL newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      config_->SetComplaintProcessingEnabled(newVal == VARIANT_TRUE);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_ComplaintMailbox(BSTR *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetComplaintMailbox().AllocSysString();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_ComplaintMailbox(BSTR newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      // Every address in the list has to be one, and one this server hosts: a
      // feedback loop is registered against a mailbox here, and a list naming an
      // address somewhere else would read no report and say nothing about it.
      HM::String value = newVal;
      value.Trim();

      if (value.GetLength() > 255)
         return COMError::GenerateError("ComplaintMailbox must be at most 255 characters.");

      if (!value.IsEmpty())
      {
         HM::String list = value;
         list.Replace(_T(";"), _T(","));
         list.Replace(_T(" "), _T(","));

         std::vector<HM::String> names = HM::StringParser::SplitString(list, _T(","));
         int found = 0;

         for (const HM::String &name : names)
         {
            HM::String address = name;
            address.Trim();

            if (address.IsEmpty())
               continue;

            found++;

            if (!HM::StringParser::IsValidEmailAddress(address, true))
               return COMError::GenerateError("ComplaintMailbox must be one address, or several separated by commas.");

            if (!HM::MessageOrigin::IsHostedAddress(address))
               return COMError::GenerateError("The complaint mailbox must be an address this server hosts, since that is where a provider delivers its reports.");
         }

         if (found == 0)
            return COMError::GenerateError("ComplaintMailbox must be one address, or several separated by commas.");
      }

      config_->SetComplaintMailbox(value);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_ComplaintSuppressionThreshold(long *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetComplaintSuppressionThreshold();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_ComplaintSuppressionThreshold(long newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      // Refused rather than clamped, so a caller that asked for a value outside the
      // range learns that it did not get it.
      if (newVal < 0 || newVal > 100)
         return COMError::GenerateError("ComplaintSuppressionThreshold must be between 0 and 100.");

      config_->SetComplaintSuppressionThreshold((int) newVal);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_ComplaintRetentionDays(long *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetComplaintRetentionDays();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_ComplaintRetentionDays(long newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      // Refused rather than clamped, so a caller that asked for a value outside the
      // range learns that it did not get it.
      if (newVal < 0 || newVal > 3650)
         return COMError::GenerateError("ComplaintRetentionDays must be between 0 and 3650.");

      config_->SetComplaintRetentionDays((int) newVal);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

namespace
{
   HM::String WideFromUtf8(const HM::AnsiString &utf8)
   {
      HM::String wide;
      HM::Unicode::MultiByteToWide(utf8, wide);
      return wide;
   }
}

STDMETHODIMP InterfaceSettings::GetChangedFromDefault(BSTR *Json)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      if (!Json)
         return COMError::GenerateGenericMessage();

      // The same document GET /api/v1/settings/changed answers, built by the same
      // function, so that an administrator reading the Control Panel and one
      // reading the Deck cannot be told different things about the same server.
      *Json = WideFromUtf8(HM::SettingsDiff::Json()).AllocSysString();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

// The setting profiles (Common/Application/Profiles, docs/Profiles.md): the same
// documents GET, POST .../apply, .../revert and .../discard under
// /api/v1/settings/profiles answer, made by the same functions, so that the Control
// Panel and the Deck cannot be told different things about the same profile. A
// refusal - a way back held, nothing to revert, an entry that needs the
// confirmation or would be refused - is raised as an error with the server's
// sentence, which the Control Panel shows as it is.
STDMETHODIMP InterfaceSettings::GetProfiles(BSTR *Json)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      if (!Json)
         return COMError::GenerateGenericMessage();

      if (!GetIsServerAdmin())
         return GetAccessDenied();

      *Json = WideFromUtf8(HM::Profiles::ListJson()).AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::GetProfile(BSTR Name, BSTR *Json)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      if (!Json)
         return COMError::GenerateGenericMessage();

      if (!GetIsServerAdmin())
         return GetAccessDenied();

      HM::AnsiString json;
      if (!HM::Profiles::DiffJson(HM::String(Name), json))
         return COMError::GenerateError(HM::Formatter::Format("There is no profile named {0}. The profiles are office, gateway, relay and sender.", HM::String(Name)));

      *Json = WideFromUtf8(json).AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

namespace
{
   // A write that failed part way: COM has no body beside its error, so the
   // entries that were written - and the one that was not - go into the sentence,
   // which is what REST answers in its 500's body.
   HM::String ProfileReportLines_(const HM::AnsiString &json)
   {
      HM::JsonValue document;
      std::string error;
      if (!HM::JsonValue::Parse(std::string(json.c_str()), document, error) || !document.IsObject())
         return HM::String();

      const HM::JsonValue *entries = document.Get("entries");
      if (!entries || !entries->IsArray())
         return HM::String();

      std::vector<HM::String> lines;
      for (const HM::JsonValue &entry : entries->Items())
      {
         const std::string result = entry.GetString("result");
         if (result == "unchanged")
            continue;

         HM::AnsiString line = HM::AnsiString(entry.GetString("name").c_str()) + " " + HM::AnsiString(result.c_str());
         if (!entry.GetString("reason").empty() && result != "set" && result != "restored")
            line += " (" + HM::AnsiString(entry.GetString("reason").c_str()) + ")";
         lines.push_back(WideFromUtf8(line));
      }

      if (lines.empty())
         return HM::String();
      return _T(" What each setting did: ") + HM::StringParser::JoinVector(lines, _T("; ")) + _T(".");
   }
}

STDMETHODIMP InterfaceSettings::ApplyProfile(BSTR Name, VARIANT_BOOL Confirmed, BSTR DiffToken, BSTR *Json)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      if (!Json)
         return COMError::GenerateGenericMessage();

      // Installs the actor the apply and every setting it writes are recorded under.
      if (!GetIsServerAdmin())
         return GetAccessDenied();

      // A confirmation is of the diff the caller showed, as over REST.
      const HM::String token = DiffToken ? HM::String(DiffToken) : HM::String();
      if (Confirmed == VARIANT_TRUE && token.IsEmpty())
         return COMError::GenerateError(_T("A confirmation confirms a diff: pass the diff_token GetProfile answered as DiffToken beside Confirmed."));

      HM::AnsiString json;
      HM::String sentence;
      const HM::Profiles::Outcome outcome = HM::Profiles::Apply(HM::String(Name), Confirmed == VARIANT_TRUE, token, json, sentence);
      if (outcome == HM::Profiles::OutcomeFailed)
         return COMError::GenerateError(sentence + ProfileReportLines_(json));
      if (outcome != HM::Profiles::OutcomeDone)
         return COMError::GenerateError(sentence);

      *Json = WideFromUtf8(json).AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::RevertProfile(BSTR *Json)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      if (!Json)
         return COMError::GenerateGenericMessage();

      if (!GetIsServerAdmin())
         return GetAccessDenied();

      HM::AnsiString json;
      HM::String sentence;
      const HM::Profiles::Outcome outcome = HM::Profiles::Revert(json, sentence);
      if (outcome == HM::Profiles::OutcomeFailed)
         return COMError::GenerateError(sentence + ProfileReportLines_(json));
      if (outcome != HM::Profiles::OutcomeDone)
         return COMError::GenerateError(sentence);

      *Json = WideFromUtf8(json).AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::DiscardProfileSnapshot()
{
   try
   {
      if (!config_)
         return NotLoaded_();

      if (!GetIsServerAdmin())
         return GetAccessDenied();

      HM::AnsiString json;
      HM::String sentence;
      if (HM::Profiles::Discard(json, sentence) != HM::Profiles::OutcomeDone)
         return COMError::GenerateError(sentence);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::DeclineProfiles(BSTR *Json)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      if (!Json)
         return COMError::GenerateGenericMessage();

      if (!GetIsServerAdmin())
         return GetAccessDenied();

      HM::AnsiString json;
      HM::String sentence;
      if (HM::Profiles::Decline(json, sentence) != HM::Profiles::OutcomeDone)
         return COMError::GenerateError(sentence);

      *Json = WideFromUtf8(json).AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_AppliedProfile(BSTR *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      if (!pVal)
         return COMError::GenerateGenericMessage();

      *pVal = HM::Profiles::AppliedProfile().AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::GetSettingDefault(BSTR Name, BSTR *Value)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      if (!Value)
         return COMError::GenerateGenericMessage();

      HM::String name = Name;
      name.Trim();

      HM::String shipped;
      if (!HM::SettingsDiff::ShippedValue(name, shipped))
         return COMError::GenerateError("This build ships no default for a setting of that name. The [Settings] keys it reads are in IniSettingsBaseline.h and the hm_settings properties in SettingsDefaults.h; a name in neither is one an administrator stored for a script of their own, which has no default because nothing here reads it.");

      *Value = shipped.AllocSysString();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}


STDMETHODIMP InterfaceSettings::get_SpamTrapBanMinutes(long *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = (long) config_->GetSpamTrapBanMinutes();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_SpamTrapBanMinutes(long newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      if (newVal < 0 || newVal > 525600)
         return COMError::GenerateError("SpamTrapBanMinutes is between 0 and 525600 (a year). 0 bans nobody, which leaves the sender block and the record.");

      config_->SetSpamTrapBanMinutes((int) newVal);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_SpamTrapBlockDays(long *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = (long) config_->GetSpamTrapBlockDays();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_SpamTrapBlockDays(long newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      if (newVal < 0 || newVal > 3650)
         return COMError::GenerateError("SpamTrapBlockDays is between 0 and 3650. 0 blocks nobody, which leaves the ban and the record.");

      config_->SetSpamTrapBlockDays((int) newVal);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_SpamTrapHitRetentionDays(long *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = (long) config_->GetSpamTrapHitRetentionDays();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_SpamTrapHitRetentionDays(long newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      if (newVal < 1 || newVal > 3650)
         return COMError::GenerateError("SpamTrapHitRetentionDays is between 1 and 3650: the record of spam trap hits is kept for that long.");

      config_->SetSpamTrapHitRetentionDays((int) newVal);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_SpamTrapTeachSpamAssassin(VARIANT_BOOL *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetSpamTrapTeachSpamAssassin() ? VARIANT_TRUE : VARIANT_FALSE;

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_SpamTrapTeachSpamAssassin(VARIANT_BOOL newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      config_->SetSpamTrapTeachSpamAssassin(newVal == VARIANT_TRUE);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_QuotaGracePercent(long *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetQuotaGracePercent();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_QuotaGracePercent(long newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      if (newVal < 0 || newVal > 1000)
         return COMError::GenerateError("QuotaGracePercent must be between 0 and 1000.");

      config_->SetQuotaGracePercent((int) newVal);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_QuotaSendStopPercent(long *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetQuotaSendStopPercent();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_QuotaSendStopPercent(long newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      if (newVal < 0 || newVal > 1000)
         return COMError::GenerateError("QuotaSendStopPercent must be between 0 and 1000.");

      config_->SetQuotaSendStopPercent((int) newVal);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

// Outbound shaping (SMTP/DestinationBackoff.h). Each setter refuses rather
// than clamps, so a caller that asked for a value outside the range learns
// that it did not get it; the sentence is the one the REST route answers.

STDMETHODIMP InterfaceSettings::get_OutboundBackoffMinutes(long *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetOutboundBackoffMinutes();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_OutboundBackoffMinutes(long newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      if (newVal < 0 || newVal > 10080)
         return COMError::GenerateError("OutboundBackoffMinutes must be between 0 and 10080.");

      if (newVal > config_->GetOutboundBackoffMaxMinutes())
         return COMError::GenerateError("OutboundBackoffMinutes must not be above OutboundBackoffMaxMinutes.");

      config_->SetOutboundBackoffMinutes((int) newVal);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_OutboundBackoffMaxMinutes(long *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetOutboundBackoffMaxMinutes();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_OutboundBackoffMaxMinutes(long newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      if (newVal < 1 || newVal > 10080)
         return COMError::GenerateError("OutboundBackoffMaxMinutes must be between 1 and 10080.");

      if (newVal < config_->GetOutboundBackoffMinutes())
         return COMError::GenerateError("OutboundBackoffMaxMinutes must not be below OutboundBackoffMinutes.");

      config_->SetOutboundBackoffMaxMinutes((int) newVal);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_MaxOutboundConnectionsPerDestination(long *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetMaxOutboundConnectionsPerDestination();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_MaxOutboundConnectionsPerDestination(long newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      if (newVal < 0 || newVal > 1000)
         return COMError::GenerateError("MaxOutboundConnectionsPerDestination must be between 0 and 1000.");

      config_->SetMaxOutboundConnectionsPerDestination((int) newVal);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::get_MaxOutboundConnectionsPerTenant(long *pVal)
{
   try
   {
      if (!config_)
         return NotLoaded_();

      *pVal = config_->GetMaxOutboundConnectionsPerTenant();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSettings::put_MaxOutboundConnectionsPerTenant(long newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!config_)
         return NotLoaded_();

      if (newVal < 0 || newVal > 1000)
         return COMError::GenerateError("MaxOutboundConnectionsPerTenant must be between 0 and 1000.");

      config_->SetMaxOutboundConnectionsPerTenant((int) newVal);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}
