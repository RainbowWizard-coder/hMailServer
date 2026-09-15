// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"

#include "InterfaceApplication.h"
#include "InterfaceAccount.h"
#include "InterfaceDomains.h"
#include "InterfaceDatabase.h"
#include "InterfaceDiagnostics.h"
#include "InterfaceStatus.h"
#include "InterfaceUtilities.h"
#include "InterfaceRules.h"
#include "InterfaceSettings.h"
#include "InterfaceBackupManager.h"
#include "InterfaceGlobalObjects.h"
#include "InterfaceLinks.h"

#include "../Common/Persistence/PersistentAccount.h"
#include "../Common/BO/Account.h"
#include "../Common/Application/RoleManager.h"
#include "../Common/Application/IniFileSettings.h"
#include "../Common/Util/AccountLockout.h"
#include "../Common/Util/Crypt.h"
#include "../Common/Util/PasswordHistory.h"
#include "../Common/Util/PasswordPolicy.h"
#include "../Common/Util/Time.h"
#include "../Common/Util/Parsing/StringParser.h"
#include "../POP3/POP3Sessions.h"
#include "COMError.h"

#include "../Common/Application/IniFileSettings.h"

#include "../Common/Util/ServiceManager.h"
#include "../Common/Util/ServerStatus.h"

#include "../Common/BO/Rules.h"
#include "../Common/BO/Domains.h"

#include "COMAuthentication.h"

STDMETHODIMP InterfaceApplication::InterfaceSupportsErrorInfo(REFIID riid)
{
   static const IID* arr[] = 
   {
      &IID_IInterfaceApplication,
   };

   for (int i=0;i<sizeof(arr)/sizeof(arr[0]);i++)
   {
      if (InlineIsEqualGUID(*arr[i],riid))
         return S_OK;
   }

   return S_FALSE;
}

InterfaceApplication::InterfaceApplication()
{
   authentication_ = std::shared_ptr<HM::COMAuthentication>(new HM::COMAuthentication);

   authentication_->AttempAnonymousAuthentication();
}

STDMETHODIMP InterfaceApplication::Start()
{
   try
   {
      // Start the server threads.
      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();
   
      if (!HM::Application::Instance()->StartServers())
         return COMError::GenerateError("Server start failed. Please check the hMailServer error log.");
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceApplication::Stop()
{
   try
   {
      // Stop the server threads.
      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();
       
      HM::Application::Instance()->StopServers();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceApplication::Reinitialize()
{
   try
   {
      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();
   
      HM::String sErrorMessage = HM::Application::Instance()->Reinitialize();
      if (!sErrorMessage.IsEmpty())
         return COMError::GenerateError(sErrorMessage);
      
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceApplication::get_Settings(IInterfaceSettings **pVal)
{
   try
   {
      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();
   
      // A connection is needed; a loaded configuration is not - the settings
      // object refuses its own getters with the reason when there is none, and
      // its directories answer from hMailServer.ini, which is what DBUpdater
      // reads on a database the server refused (issue #263).
      HRESULT hResult = EnsureDatabaseConnection_();
      if (hResult != S_OK)
         return hResult;
   
      CComObject<InterfaceSettings>* pInterfaceSettings = new CComObject<InterfaceSettings>;
      pInterfaceSettings->SetAuthentication(authentication_);
      pInterfaceSettings->LoadSettings();
   
      pInterfaceSettings->AddRef();
      *pVal = pInterfaceSettings;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceApplication::get_BackupManager(IInterfaceBackupManager **pVal)
{
   try
   {
      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();
   
      HRESULT hResult = EnsureDatabaseConnectivity_();
      if (hResult != S_OK)
         return hResult;
   
      CComObject<InterfaceBackupManager>* pInterfaceBackupManager = new CComObject<InterfaceBackupManager>;
      pInterfaceBackupManager->SetAuthentication(authentication_);
      if (!pInterfaceBackupManager->LoadSettings())
         return COMError::GenerateError("Backup manager not available");
        
      pInterfaceBackupManager->AddRef();
      *pVal = pInterfaceBackupManager;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceApplication::get_GlobalObjects(IInterfaceGlobalObjects **pVal)
{
   try
   {
      HRESULT hResult = EnsureDatabaseConnectivity_();
      if (hResult != S_OK)
         return hResult;

      CComObject<InterfaceGlobalObjects>* pInterfaceGlobalObjects = new CComObject<InterfaceGlobalObjects>;
      
      pInterfaceGlobalObjects->SetAuthentication(authentication_);
      pInterfaceGlobalObjects->AddRef();
      *pVal = pInterfaceGlobalObjects;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceApplication::get_Domains(IInterfaceDomains **pVal)
{
   try
   {
      if (!authentication_->GetIsAuthenticated())
         return authentication_->GetAccessDenied();
   
      HRESULT hResult = EnsureDatabaseConnectivity_();
      if (hResult != S_OK)
         return hResult;
   
   
      CComObject<InterfaceDomains>* pInterfaceDomains = new CComObject<InterfaceDomains>;
   
      pInterfaceDomains->SetAuthentication(authentication_);
      pInterfaceDomains->Refresh();
   
      pInterfaceDomains->AddRef();
      *pVal = pInterfaceDomains;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceApplication::get_ServerState(eServerState *pVal)
{
   try
   {
      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();
   
      HM::ServerStatus::ServerState iState = (HM::ServerStatus::ServerState) HM::ServerStatus::Instance()->GetState();
   
      switch (iState)
      {
      case HM::ServerStatus::StateStarting:
         *pVal = hStateStarting;
         break;
      case HM::ServerStatus::StateRunning:
         *pVal = hStateRunning;
         break;
      case HM::ServerStatus::StateStopping:
         *pVal = hStateStopping;
         break;
      case HM::ServerStatus::StateStopped:
         *pVal = hStateStopped;
         break;
      default:
         *pVal = hStateUnknown;
         break;
      }
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceApplication::get_Database(IInterfaceDatabase **pVal)
{
   try
   {
      CComObject<InterfaceDatabase>* pInterfaceDatabase = new CComObject<InterfaceDatabase>;
   
      pInterfaceDatabase->SetAuthentication(authentication_);
      pInterfaceDatabase->LoadSettings();
   
      pInterfaceDatabase->AddRef();
      *pVal = pInterfaceDatabase;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP 
InterfaceApplication::get_Status(IInterfaceStatus **pVal)
{
   try
   {
      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();
   
      CComObject<InterfaceStatus>* pStatus = new CComObject<InterfaceStatus>;
   
      pStatus->SetAuthentication(authentication_);
      pStatus->LoadSettings();
   
      pStatus->AddRef();
      *pVal = pStatus;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceApplication::SubmitEMail()
{
   try
   {
      DWORD SubmitEmailOpCode = 200;
   
      HM::ServiceManager pServiceManager;
      pServiceManager.UserControlService("hMailServer", SubmitEmailOpCode);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceApplication::Connect()
{
   try
   {
      HM::String sErrorMessage = HM::Application::Instance()->GetLastErrorMessage();
      if (!sErrorMessage.IsEmpty())
         return COMError::GenerateError(sErrorMessage);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceApplication::AuthenticateWithCode(BSTR sUsername, BSTR sPassword, BSTR sCode, IInterfaceAccount **pVal)
{
   try
   {
      // The one entry point that can satisfy a second factor. An account with one
      // enrolled is refused by Authenticate above, and by every mail protocol, because
      // none of them has anywhere to present a code - which is what app passwords are
      // for. See PasswordValidator::ValidatePassword.
      std::shared_ptr<const HM::Account> pAccount = authentication_->Authenticate(sUsername, sPassword, sCode);

      if (pAccount)
      {
         std::shared_ptr<HM::Account> accountCopy = std::shared_ptr<HM::Account>(new HM::Account(*pAccount.get()));

         CComObject<InterfaceAccount>* pAccountInt = new CComObject<InterfaceAccount>();

         pAccountInt->AttachItem(accountCopy);
         pAccountInt->SetAuthentication(authentication_);
         pAccountInt->AddRef();

         *pVal = pAccountInt;
      }

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceApplication::Authenticate(BSTR sUsername, BSTR sPassword, IInterfaceAccount **pVal)
{
   try
   {
      // Authenticates the user and returns the account object.
      std::shared_ptr<const HM::Account> pAccount = authentication_->Authenticate(sUsername, sPassword);
   
      if (pAccount)
      {
         std::shared_ptr<HM::Account> accountCopy = std::shared_ptr<HM::Account>(new HM::Account(*pAccount.get()));
   
         // Return the account.
         CComObject<InterfaceAccount>* pAccountInt = new CComObject<InterfaceAccount>();
   
         pAccountInt->AttachItem(accountCopy);
         pAccountInt->SetAuthentication(authentication_);
         pAccountInt->AddRef();
   
         *pVal = pAccountInt;
   
      }
      
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceApplication::get_AdministratorTOTPEnabled(VARIANT_BOOL *pVal)
{
   try
   {
      // Deliberately readable without authenticating: a client has to know
      // whether to ask for a code before it can present the credential, and
      // "this account has a second factor" is what every logon form reveals the
      // moment the password is accepted anyway. The secret itself is never
      // readable from anywhere.
      *pVal = HM::IniFileSettings::Instance()->GetAdministratorTotpSecret().IsEmpty() ? VARIANT_FALSE : VARIANT_TRUE;

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceApplication::get_Version(BSTR *pVal)
{
   try
   {
      *pVal = HM::Application::Instance()->GetVersionNumber().AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceApplication::get_VersionArchitecture(BSTR *pVal)
{
   try
   {
      *pVal = HM::Application::Instance()->GetVersionArchitecture().AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceApplication::get_Utilities(IInterfaceUtilities **pVal)
{
   try
   {
      CComObject<InterfaceUtilities>* pUtilities = new CComObject<InterfaceUtilities>;
   
      pUtilities->SetAuthentication(authentication_);
      pUtilities->AddRef();
      *pVal = pUtilities;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceApplication::get_InitializationFile(BSTR *pVal)
{
   try
   {
      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();
   
      *pVal = HM::IniFileSettings::Instance()->GetInitializationFile().AllocSysString();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceApplication::get_Rules(IInterfaceRules **pVal)
{
   try
   {
      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();
   
      HRESULT hResult = EnsureDatabaseConnectivity_();
      if (hResult != S_OK)
         return hResult;
   
      CComObject<InterfaceRules >* pItem = new CComObject<InterfaceRules >();
      pItem->SetAuthentication(authentication_);
   
      std::shared_ptr<HM::Rules> pRules = std::shared_ptr<HM::Rules>(new HM::Rules(0));
   
      if (pRules)
      {
         pRules->Refresh();
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

HRESULT
InterfaceApplication::EnsureDatabaseConnection_()
{
   std::shared_ptr<HM::DatabaseConnectionManager> pConnectionManager = HM::Application::Instance()->GetDBManager();
   if (!pConnectionManager || !pConnectionManager->GetIsConnected())
   {
      return COMError::GenerateError("The connection to the database is not available. Please check the hMailServer error log for details.");
   }
   return S_OK;
}

HRESULT
InterfaceApplication::EnsureDatabaseConnectivity_()
{
   HRESULT connected = EnsureDatabaseConnection_();
   if (connected != S_OK)
      return connected;

   // Connected is not enough. A database this server opened and then refused -
   // its schema older or newer than this build requires (HM5011), or its
   // version unreadable (HM5010) - stays open for hMailServer.Database, which
   // is how DBUpdater brings it up to date, and nothing else was loaded:
   // Configuration::Load never ran. The objects the getters hand out read the
   // configuration's property set on every call, and until 12 September 2026
   // that was an access violation inside the call, reported to the client as
   // "an error occurred processing the request" and to the crash oracle as a
   // memory fault. The refusal's own message says what to do instead.
   if (!HM::Application::Instance()->IsInitialized())
   {
      HM::String reason = HM::Application::Instance()->GetLastErrorMessage();
      if (reason.IsEmpty())
         reason = "The server has not finished starting.";

      return COMError::GenerateError(HM::String("The server has not loaded its configuration. ") + reason);
   }

   return S_OK;
}

STDMETHODIMP InterfaceApplication::get_Links(IInterfaceLinks **pVal)
{
   try
   {
      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();
   
      HRESULT hResult = EnsureDatabaseConnectivity_();
      if (hResult != S_OK)
         return hResult;
   
      CComObject<InterfaceLinks>* pItem = new CComObject<InterfaceLinks >();
      pItem->SetAuthentication(authentication_);
      pItem->AddRef();
      *pVal = pItem;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceApplication::get_Diagnostics(IInterfaceDiagnostics **pVal)
{
   try
   {
      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();
   
      HRESULT hResult = EnsureDatabaseConnectivity_();
      if (hResult != S_OK)
         return hResult;

      CComObject<InterfaceDiagnostics>* pInterfaceDiagnostics = new CComObject<InterfaceDiagnostics>;
   
      pInterfaceDiagnostics->SetAuthentication(authentication_);
      pInterfaceDiagnostics->AddRef();
   
      *pVal = pInterfaceDiagnostics;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceApplication::get_Roles(BSTR *pVal)
{
   try
   {
      if (!authentication_->GetIsAuthenticated())
         return authentication_->GetAccessDenied();

      if (!authentication_->Allows(HM::RolePermission::ViewConfiguration))
         return authentication_->GetAccessDenied();

      // One line per role: name, built-in, description, permissions, tab
      // separated. A string and not a collection deliberately - a new COM
      // interface needs an elevated registration on every machine that runs
      // the suite, and a listing does not earn that.
      HM::String result;

      for (const HM::RoleRecord &record : HM::RoleManager::ListRoles())
      {
         HM::String line;
         line.Format(_T("%s\t%d\t%s\t%s\r\n"),
                     record.name.c_str(), record.builtin ? 1 : 0,
                     record.description.c_str(), record.permissions.c_str());
         result += line;
      }

      *pVal = result.AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceApplication::get_CurrentRole(BSTR *pVal)
{
   try
   {
      if (!authentication_->GetIsAuthenticated())
         return authentication_->GetAccessDenied();

      *pVal = HM::String(authentication_->GetGrant().name).AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceApplication::get_CurrentPermissions(BSTR *pVal)
{
   try
   {
      if (!authentication_->GetIsAuthenticated())
         return authentication_->GetAccessDenied();

      // What the signed-in credential may do, so that a Control Panel can
      // leave out a page rather than show one every button of which refuses.
      *pVal = HM::String(HM::RoleManager::DescribeGrant(authentication_->GetGrant())).AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceApplication::get_RoleAssignments(BSTR *pVal)
{
   try
   {
      if (!authentication_->GetIsAuthenticated())
         return authentication_->GetAccessDenied();

      if (!authentication_->Allows(HM::RolePermission::ViewConfiguration))
         return authentication_->GetAccessDenied();

      HM::String result;

      for (const HM::RoleAssignment &assignment : HM::RoleManager::ListAssignments())
      {
         std::shared_ptr<HM::Account> account = std::shared_ptr<HM::Account>(new HM::Account());

         // An assignment whose account has gone is not listed; the row follows
         // the account through the foreign key, and this is the moment between
         // the delete and the reload.
         if (!HM::PersistentAccount::ReadObject(account, assignment.account_id) || account->GetID() == 0)
            continue;

         // Narrowed to the domains a confined credential reaches, as the REST
         // listing is, so that a domain administrator learns nothing about who
         // administers any other domain.
         if (!authentication_->ReachesEveryDomain() &&
             !authentication_->AllowsForDomain(HM::RolePermission::ViewConfiguration, HM::StringParser::ExtractDomain(account->GetAddress())))
         {
            continue;
         }

         // A fourth field says whether an empty list means every domain, so that
         // the Control Panel shows what the assignment reaches without knowing
         // the rule itself.
         const bool everyDomain = assignment.domains.IsEmpty() && HM::RoleManager::ReachesEveryDomainWhenUnlisted(assignment.role_name);

         HM::String line;
         line.Format(_T("%s\t%s\t%s\t%d\r\n"),
                     account->GetAddress().c_str(),
                     assignment.role_name.c_str(),
                     assignment.domains.c_str(),
                     everyDomain ? 1 : 0);
         result += line;
      }

      *pVal = result.AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceApplication::AssignRole(BSTR Address, BSTR Role, BSTR Domains)
{
   try
   {
      if (!authentication_->GetIsAuthenticated())
         return authentication_->GetAccessDenied();

      if (!authentication_->Allows(HM::RolePermission::ManageRoles))
         return authentication_->GetAccessDenied();

      std::shared_ptr<HM::Account> account = std::shared_ptr<HM::Account>(new HM::Account());

      // The same answer for an address that does not exist and for one this
      // credential may not see, because ManageRoles is only ever held with
      // AdministerServer today - and when it is delegated, the day it is, this
      // must already read the same either way.
      if (!HM::PersistentAccount::ReadObject(account, HM::String(Address)) || account->GetID() == 0)
         return COMError::GenerateError("No such account.");

      const HM::String wanted = Role;
      HM::String error;

      if (wanted.IsEmpty())
      {
         if (!HM::RoleManager::Unassign(account->GetID(), error))
            return COMError::GenerateError(error);

         return S_OK;
      }

      // A credential may not hand out authority it has not got, nor take away
      // authority it has not got.
      if (!HM::RoleManager::Encompasses(authentication_->GetGrant(), HM::RoleManager::GrantForAssignment(account, wanted, HM::String(Domains))) ||
          !HM::RoleManager::Encompasses(authentication_->GetGrant(), HM::RoleManager::GrantForAccount(account)))
      {
         return COMError::GenerateError("That role holds permissions this credential does not, and a credential may not hand out authority it has not got.");
      }

      if (!HM::RoleManager::Assign(account->GetID(), wanted, HM::String(Domains), error))
         return COMError::GenerateError(error);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

namespace
{
   // This unit sits outside namespace HM, where the logging macros do not
   // resolve; the two role actions log through here.
   void LogRoleAction_(const HM::String &message)
   {
      using namespace HM;
      LOG_APPLICATION(message);
   }

   // The account an address names, if this credential may act on it with this
   // permission - or nothing, with ONE answer for both "there is no such
   // account" and "there is, and it is not yours". The domain is judged from
   // the address alone, before the database is asked, so that the time the
   // answer takes does not tell the two apart either.
   std::shared_ptr<HM::Account> ReachableAccount_(std::shared_ptr<HM::COMAuthentication> authentication,
                                                  HM::RolePermission permission, const HM::String &address)
   {
      const HM::String domainName = HM::StringParser::ExtractDomain(address);

      if (domainName.IsEmpty() || !authentication->AllowsForDomain(permission, domainName))
         return std::shared_ptr<HM::Account>();

      std::shared_ptr<HM::Account> account = std::shared_ptr<HM::Account>(new HM::Account());

      if (!HM::PersistentAccount::ReadObject(account, address) || account->GetID() == 0)
         return std::shared_ptr<HM::Account>();

      return account;
   }
}

STDMETHODIMP InterfaceApplication::ResetPassword(BSTR Address, BSTR Password)
{
   try
   {
      if (!authentication_->GetIsAuthenticated())
         return authentication_->GetAccessDenied();

      // The permission first, with no account named: a credential that holds
      // no reset_password anywhere is refused as every COM refusal is, and
      // learns nothing about any address.
      if (!authentication_->Allows(HM::RolePermission::ResetPassword))
         return authentication_->GetAccessDenied();

      std::shared_ptr<HM::Account> account = ReachableAccount_(authentication_, HM::RolePermission::ResetPassword, HM::String(Address));

      if (!account)
         return COMError::GenerateError("No such account.");

      // Resetting a password is taking the account over: refused for an account
      // whose authority this credential does not take in, which is how a help
      // desk is kept from resetting a domain administrator's password in its
      // domain and signing in as them.
      if (!HM::RoleManager::Encompasses(authentication_->GetGrant(), HM::RoleManager::GrantForAccount(account)))
         return COMError::GenerateError("That account holds authority this credential does not, so its password is not this credential's to reset.");

      if (account->GetIsAD())
         return COMError::GenerateError("This account authenticates against the directory; its password is not this server's to set.");

      const HM::String password = Password;

      // The policy, the reuse history, the old hash recorded before the new
      // one replaces it, the configured hash and the age clock: exactly what
      // InterfaceAccount::put_Password does, because a password set by a help
      // desk must be no weaker than one set anywhere else.
      HM::String policyFailure;

      if (!HM::PasswordPolicy::IsAcceptable(account->GetAddress(), password, policyFailure))
         return COMError::GenerateError(policyFailure);

      if (HM::PasswordHistory::IsReuse(account, password))
         return COMError::GenerateError("This password has been used recently on this account. Choose one that has not.");

      HM::PasswordHistory::Record(account);

      const int preferredHashAlgorithm = HM::IniFileSettings::Instance()->GetPreferredHashAlgorithm();
      account->SetPassword(HM::Crypt::Instance()->EnCrypt(password, (HM::Crypt::EncryptionType) preferredHashAlgorithm));
      account->SetPasswordEncryption(preferredHashAlgorithm);
      account->SetPasswordChanged(HM::Time::GetCurrentDateTime());

      HM::String errorMessage;

      if (!HM::PersistentAccount::SaveObject(account, errorMessage, false, HM::PersistenceModeNormal))
         return COMError::GenerateError("Failed to save object. " + errorMessage);

      // Somebody whose password has just been reset because they could not get
      // in must be able to get in.
      HM::AccountLockout::Instance()->RecordSuccess(account->GetAddress());

      HM::String logged;
      logged.Format(_T("COM: the password of %s was reset by %s."),
                    account->GetAddress().c_str(), HM::String(authentication_->GetActor().identity).c_str());
      LogRoleAction_(logged);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceApplication::UnlockAccount(BSTR Address)
{
   try
   {
      if (!authentication_->GetIsAuthenticated())
         return authentication_->GetAccessDenied();

      if (!authentication_->Allows(HM::RolePermission::UnlockAccount))
         return authentication_->GetAccessDenied();

      std::shared_ptr<HM::Account> account = ReachableAccount_(authentication_, HM::RolePermission::UnlockAccount, HM::String(Address));

      if (!account)
         return COMError::GenerateError("No such account.");

      // Both locks, as the REST route clears both: somebody who cannot get to
      // their mail does not know which one they are behind.
      HM::AccountLockout::Instance()->RecordSuccess(account->GetAddress());
      HM::POP3Sessions::Instance()->Unlock(account->GetID());

      HM::String logged;
      logged.Format(_T("COM: %s was unlocked by %s."),
                    account->GetAddress().c_str(), HM::String(authentication_->GetActor().identity).c_str());
      LogRoleAction_(logged);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}
