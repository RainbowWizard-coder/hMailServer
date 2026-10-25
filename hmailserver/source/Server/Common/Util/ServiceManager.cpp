// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#include "stdafx.h"

#ifndef HM_PLATFORM_POSIX

// The whole of this file is the Windows Service Control Manager; see the note at
// the top of ServiceManager.h for why there is no POSIX arm rather than a set of
// stubs. On this platform the translation unit is deliberately empty.

#include "ServiceManager.h"

#include "../Application/IniFileSettings.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   ServiceManager::ServiceManager() :
      account_choice_(AccountKeep)
   {

   }

   ServiceManager::~ServiceManager()
   {

   }

   void
   ServiceManager::Report_(int errorId, const String &source, const String &sentence)
   {
      reported_ += sentence + _T("\r\n");
      ErrorManager::Instance()->ReportError(ErrorManager::Medium, errorId, source, sentence);
   }

   bool
   ServiceManager::RegisterService(const String &ServiceName, const String &ServiceCaption)
   //---------------------------------------------------------------------------//
   // DESCRIPTION:
   // Registres a service with the SCM. (adds the application to the list of services)
   //---------------------------------------------------------------------------//
   {
      // Retrieve the handle for the local service manager.
      SC_HANDLE hSCManager = OpenSCManager( NULL, NULL, SC_MANAGER_CREATE_SERVICE | SC_MANAGER_LOCK);

      if (hSCManager == NULL)
      {
         String sErrorMessage;
         sErrorMessage.Format(_T("The service control manager could not be opened to create or change the service (%d); run this from an elevated prompt."), (int) GetLastError());
         Report_(5055, "ServiceManager::RegisterService", sErrorMessage);
         return false;
      }

      // Check wether we already exists.
      if (DoesServiceExist(ServiceName))
      {
         if (!ReconfigureService_(hSCManager, ServiceName))
            return false;
      }
      else
      {
         // Get the path to the currently running executable
         String sPath = "\"" + Application::GetExecutableName() + "\" RunAsService";

         // Check wether we should set the service dependent on MySQL.
         LPCTSTR szServiceDependencies = _T("RPCSS\0\0");

         // The account the choice names (LogonFor): no name is LocalSystem here.
         Logon logon;
         String refusal;
         if (!LogonFor(account_choice_, ServiceName, account_name_, logon, refusal))
         {
            Report_(7464, "ServiceManager::RegisterService", refusal);
            CloseServiceHandle(hSCManager);
            return false;
         }
         LPCTSTR pServiceStartName = logon.change ? logon.name.c_str() : NULL;
         LPCTSTR pServicePassword = logon.change && logon.emptyPassword ? _T("") : NULL;

         SC_HANDLE hService = CreateService( hSCManager,
                                   ServiceName,
                                   ServiceCaption,
                                   GENERIC_EXECUTE | GENERIC_WRITE | GENERIC_READ,
                                   SERVICE_WIN32_OWN_PROCESS,
                                   SERVICE_AUTO_START,
                                   SERVICE_ERROR_NORMAL,
                                   sPath,
                                   NULL,
                                   NULL,
                                   szServiceDependencies,
                                   pServiceStartName,
                                   pServicePassword
                                   );  


         if (hService == NULL)
         {
            String sErrorMessage;
            sErrorMessage.Format(_T("Description: CreateService failed. (%d)"), GetLastError());
            Report_(5055, "ServiceManager::RegisterService", sErrorMessage);

            return false;
         }

         if (logon.change && !RunsAsLocalSystem_(hService))
            SetUnrestrictedSid_(hService);

         CloseServiceHandle (hService);

      }

      
      CloseServiceHandle (hSCManager);

      return true;
   }

   bool
   ServiceManager::ReconfigureService_(SC_HANDLE hSCManager, const String &ServiceName)
   //---------------------------------------------------------------------------//
   // DESCRIPTION:
   // Updates an existing hMailServer service.
   //---------------------------------------------------------------------------//
   {
      // Retrieve the handle for the service.
      SC_HANDLE hService = OpenService( hSCManager, ServiceName,  SERVICE_QUERY_CONFIG | SERVICE_CHANGE_CONFIG);

      
      SC_LOCK sclLock = LockServiceDatabase(hSCManager); 

      if (sclLock == NULL)
      {
         Report_(5056, "ServiceManager::ReconfigureService_", "Failed to obtain lock on service database.");

         CloseServiceHandle(hService);
         return false;
      }

      // Update the path to the executable
      String sPath = "\"" + Application::GetExecutableName() + "\" RunAsService";

      // The account the choice names (LogonFor): no name leaves the account as
      // it is, which is what an upgrade that keeps it asks for.
      Logon logon;
      String refusal;
      if (!LogonFor(account_choice_, ServiceName, account_name_, logon, refusal))
      {
         Report_(7464, "ServiceManager::ReconfigureService_", refusal);
         UnlockServiceDatabase(sclLock);
         CloseServiceHandle(hService);
         return false;
      }
      LPCTSTR pServiceStartName = logon.change ? logon.name.c_str() : NULL;
      LPCTSTR pServicePassword = logon.change && logon.emptyPassword ? _T("") : NULL;

      if (ChangeServiceConfig(
               hService,        // handle of service 
               SERVICE_NO_CHANGE , // service type: no change 
               SERVICE_NO_CHANGE, // service start type no change
               SERVICE_NO_CHANGE, // error control: no change 
               sPath,            // binary path changed
               NULL,              // load order group: no change 
               NULL,              // tag ID: no change 
               NULL,              // dependencies: no change 
               pServiceStartName, // account name: change only when configured 
               pServicePassword,  // password: change only when configured 
               NULL) == 0)             // display name: no change
      {
         String sErrorMessage;
         sErrorMessage.Format(_T("ChangeServiceConfig failed. (%d)"), GetLastError());

         Report_(5057, "ServiceManager::ReconfigureService_", sErrorMessage);
         return false;
      }

      // Read back rather than inferred from the choice: keep leaves whatever
      // the service had, which may be any account.
      if (!RunsAsLocalSystem_(hService))
         SetUnrestrictedSid_(hService);

      // Unlock and release.
      CloseServiceHandle(hService); 

      UnlockServiceDatabase(sclLock); 

      CloseServiceHandle (hSCManager);

      return true;
   }

   bool
   ServiceManager::LogonFor(AccountChoice choice, const String &serviceName, const String &accountName, Logon &logon, String &refusal)
   //---------------------------------------------------------------------------//
   // DESCRIPTION:
   // The account the service is to log on as, and the password to pass with it.
   //
   // The password is where the accounts differ, and CreateService and
   // ChangeServiceConfig are precise about it: NULL for a virtual account
   // (NT SERVICE\<name>) and for a managed service account (a name ending in
   // $), which have none; an empty string - not NULL - for LocalSystem. No
   // other account is set here, because every other account has a password,
   // and the only places one could come from are a command line, which other
   // users of the machine can read, and a file or a database other people
   // write (ServiceManager.h).
   //---------------------------------------------------------------------------//
   {
      logon.change = false;
      logon.name.Empty();
      logon.emptyPassword = false;
      refusal.Empty();

      switch (choice)
      {
      case AccountVirtual:
         logon.change = true;
         logon.name = _T("NT SERVICE\\") + serviceName;
         return true;
      case AccountLocalSystem:
         logon.change = true;
         logon.name = _T("LocalSystem");
         logon.emptyPassword = true;
         return true;
      case AccountKeep:
         return true;
      case AccountManaged:
         break;
      }

      String name = accountName;
      name.Trim();

      // A group managed service account: DOMAIN\name$, a domain part and a name
      // of its own, with no space - the command line was split on them - and no
      // character a SAM account name cannot have.
      const int separator = name.Find(_T("\\"));
      const bool shaped = separator > 0 && separator < name.GetLength() - 2 && name.Right(1) == _T("$") &&
         name.Find(_T("\\"), separator + 1) < 0 && name.FindOneOf(_T(" \t\"/[]:;|=,+*?<>@")) < 0;
      if (!shaped)
      {
         refusal = Formatter::Format(_T("--service-account takes virtual (NT SERVICE\\{0}), localsystem, keep, or a group managed service account as DOMAIN\\name$; \"{1}\" is none of them. An account with a password is set in services.msc (the service's Log On tab) instead, and every upgrade keeps it. Nothing was registered."),
            serviceName, name);
         return false;
      }

      logon.change = true;
      logon.name = name;
      return true;
   }

   bool
   ServiceManager::RunsAsLocalSystem_(SC_HANDLE hService)
   {
      // A configuration that cannot be read answers "not LocalSystem": the SID
      // type is then set, which a LocalSystem service does not need but is not
      // harmed by, where the other answer would leave a virtual or managed
      // account without the SID every grant names.
      DWORD needed = 0;
      QueryServiceConfig(hService, NULL, 0, &needed);
      if (needed == 0)
         return false;

      std::vector<BYTE> buffer(needed);
      QUERY_SERVICE_CONFIG *config = (QUERY_SERVICE_CONFIG *) buffer.data();
      if (!QueryServiceConfig(hService, config, needed, &needed))
         return false;

      const String account = config->lpServiceStartName ? String(config->lpServiceStartName) : String(_T("LocalSystem"));
      return account.IsEmpty() || account.CompareNoCase(_T("LocalSystem")) == 0 || account.CompareNoCase(_T(".\\LocalSystem")) == 0 ||
         account.CompareNoCase(_T("NT AUTHORITY\\SYSTEM")) == 0;
   }

   void
   ServiceManager::SetUnrestrictedSid_(SC_HANDLE hService)
   {
      SERVICE_SID_INFO sidInfo;
      sidInfo.dwServiceSidType = SERVICE_SID_TYPE_UNRESTRICTED;

      if (!ChangeServiceConfig2(hService, SERVICE_CONFIG_SERVICE_SID_INFO, &sidInfo))
      {
         String sErrorMessage;
         sErrorMessage.Format(_T("The service's SID type could not be set to unrestricted (%d), so the grants hMailServer.exe /GrantServiceAccess makes to NT SERVICE\\hMailServer reach the service only while it runs as that virtual account."), (int) GetLastError());
         Report_(7462, "ServiceManager::SetUnrestrictedSid_", sErrorMessage);
      }
   }

   bool
   ServiceManager::MakeDependentOn(const String &ServiceName, String &error)
   //---------------------------------------------------------------------------//
   // DESCRIPTION:
   // Makes hMailServer dependent on RPCSS and ServiceName
   //
   // This runs in the service's own process, as COM's Utilities.MakeDependent:
   // the service changing its own entry. LocalSystem may; any other account may
   // not, and is not given the right (ServiceManager.h says why), so it is told
   // so with the command an administrator runs instead - where it used to do
   // nothing at all and answer as though it had.
   //---------------------------------------------------------------------------//
   {
      const String command = _T("sc.exe config hMailServer depend= RPCSS/") + ServiceName;

      // Retrieve the handle for the local service manager.
      // SC_MANAGER_LOCK is an administrator's, so an account that is not one is
      // refused here already, before the service is asked.
      SC_HANDLE hSCManager = OpenSCManager( NULL, NULL, SC_MANAGER_CONNECT | SC_MANAGER_LOCK);
      if (hSCManager == NULL)
      {
         const int windowsError = (int) GetLastError();
         if (windowsError == ERROR_ACCESS_DENIED)
            error = Formatter::Format(_T("The hMailServer service runs as an account that may not change its own configuration, which is as it should be. From an elevated prompt: {0}"), command);
         else
            error = Formatter::Format(_T("The service control manager could not be opened to change the hMailServer service (Windows error {0}). From an elevated prompt: {1}"), windowsError, command);
         return false;
      }

      // Retrieve the handle for the service.
      SC_HANDLE hService = OpenService( hSCManager, _T("hMailServer"), SERVICE_CHANGE_CONFIG );
      if (hService == NULL)
      {
         const int windowsError = (int) GetLastError();
         CloseServiceHandle(hSCManager);
         if (windowsError == ERROR_ACCESS_DENIED)
            error = Formatter::Format(_T("The hMailServer service runs as an account that may not change its own configuration, which is as it should be. From an elevated prompt: {0}"), command);
         else
            error = Formatter::Format(_T("The hMailServer service could not be opened to change its configuration (Windows error {0}). From an elevated prompt: {1}"), windowsError, command);
         return false;
      }

      SC_LOCK sclLock = LockServiceDatabase(hSCManager);

      // Create null-terminated list of services hMailServer depends on.
      int iLength = ServiceName.GetLength() + 8;

      TCHAR * lpDependent = new TCHAR[iLength];
      int memSize = iLength * sizeof(TCHAR);

      memset(lpDependent, 0, memSize);

      // _tcscpy_s counts characters, not bytes: sized by memSize the second copy
      // believed it had twice the room it had.
      _tcscpy_s(lpDependent, iLength, _T("RPCSS"));
      _tcscpy_s(lpDependent + 6, iLength - 6, ServiceName);

      BOOL changed = ChangeServiceConfig(
         hService,        // handle of service
         SERVICE_NO_CHANGE, // service type: no change
         SERVICE_NO_CHANGE, // service start type no change
         SERVICE_NO_CHANGE, // error control: no change
         NULL,            // binary path changed
         NULL,              // load order group: no change
         NULL,              // tag ID: no change
         lpDependent,      // dependencies: no change
         NULL,              // account name: no change
         NULL,              // password: no change
         NULL);             // display name: no change

      const int windowsError = changed ? 0 : (int) GetLastError();

      if (sclLock != NULL)
         UnlockServiceDatabase(sclLock);
      CloseServiceHandle(hService);
      CloseServiceHandle(hSCManager);

      delete [] lpDependent;

      if (!changed)
      {
         error = Formatter::Format(_T("The hMailServer service's dependencies could not be changed (Windows error {0}). From an elevated prompt: {1}"), windowsError, command);
         return false;
      }

      return true;
   }

   bool
   ServiceManager::UnregisterService(const String &ServiceName)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Deregisters a service. (removes the service from the list of services)
   //---------------------------------------------------------------------------()
   {
      // Retrieve the handle for the local service manager.
      SC_HANDLE hSCManager;
      hSCManager = OpenSCManager( NULL, NULL, SC_MANAGER_CREATE_SERVICE );

      // Retrieve the handle for the service.
      SC_HANDLE hService = OpenService( hSCManager, ServiceName, SERVICE_ALL_ACCESS );

      if (!hService)
      {
         CloseServiceHandle (hSCManager);
         return false;
      }

      DeleteService(hService);

      CloseServiceHandle (hSCManager);
      CloseServiceHandle (hService);

      return 0;
   }

   bool
   ServiceManager::StartServiceOnLocalComputer(const String &ServiceName)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Starts the service with name ServiceName on the local computer.
   //---------------------------------------------------------------------------()
   {
      // Retrieve the handle for the local service manager.
      SC_HANDLE hSCManager;
      hSCManager = OpenSCManager( NULL, NULL, SC_MANAGER_CONNECT );
 
      // Retrieve the handle for the service.
      SC_HANDLE hService = OpenService( hSCManager, ServiceName, SERVICE_START );
   
      BOOL bResult = StartService(hService, 0, NULL);

      CloseServiceHandle (hSCManager);
      CloseServiceHandle (hService);

      if (bResult)
         return true;
      else
         return false;

   }

   bool
   ServiceManager::StopServiceOnLocalComputer(const String &ServiceName)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Stops the service with name ServiceName on the local computer.
   //---------------------------------------------------------------------------()
   {
      // Retrieve the handle for the local service manager.
      SC_HANDLE hSCManager;
      hSCManager = OpenSCManager( NULL, NULL, SC_MANAGER_CONNECT );
 
      // Retrieve the handle for the service.
      SC_HANDLE hService = OpenService( hSCManager, ServiceName, SERVICE_STOP );
   
      SERVICE_STATUS ssStatus;
      BOOL bResult = ::ControlService(hService, SERVICE_CONTROL_STOP, &ssStatus);

      CloseServiceHandle (hSCManager);
      CloseServiceHandle (hService);

      if (bResult)
         return true;
      else
         return false;

   }

   bool
   ServiceManager::UserControlService(const String &ServiceName, DWORD OpCode)
   {
      // Retrieve the handle for the local service manager.
      SC_HANDLE hSCManager;
      hSCManager = OpenSCManager( NULL, NULL, SC_MANAGER_CONNECT );

      if (hSCManager == NULL)
      {
         int err = GetLastError();
         switch (err)
         {
            case ERROR_ACCESS_DENIED:
               ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5059, "ServiceManager::UserControlService", "OpenSCManager failed. The requested access was denied.");
               return false;
            case ERROR_DATABASE_DOES_NOT_EXIST:
               ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5096, "ServiceManager::UserControlService", "OpenSCManager failed. The specified database does not exist.");
               return false;
            case ERROR_INVALID_PARAMETER:
               ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5061, "ServiceManager::UserControlService", "OpenSCManager failed. A specified parameter is invalid.");
               return false;
            default:
               ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5061, "ServiceManager::UserControlService", "OpenSCManager failed. Windows error code: " + StringParser::IntToString(err));
               return false;
         }

      }
 


      // Retrieve the handle for the service.
      SC_HANDLE hService = OpenService( hSCManager, ServiceName, SERVICE_USER_DEFINED_CONTROL );

      if (hService == NULL)
      {
         int err = GetLastError();

         switch (err)
         {
            case ERROR_ACCESS_DENIED:
               ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5062, "ServiceManager::UserControlService", "OpenService failed. The specified service control manager database handle does not have access to the service.");
               return false;
            case ERROR_INVALID_HANDLE:
               ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5063, "ServiceManager::UserControlService", "OpenService failed. The specified handle is invalid.");
               return false;
            case ERROR_INVALID_NAME:
               ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5064, "ServiceManager::UserControlService", "OpenService failed. The specified service name is invalid.");
               return false;
            case ERROR_SERVICE_DOES_NOT_EXIST:
               ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5065, "ServiceManager::UserControlService", "OpenService failed. The specified service does not exist.");
               return false;
            default:
               CloseServiceHandle(hSCManager);
               ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5065, "ServiceManager::UserControlService", "OpenService failed. Windows error code: " + StringParser::IntToString(err));
               return false;
         }
      }

   

      // Thell the thserver what to to
      SERVICE_STATUS ssStatus;
      BOOL bResult = ControlService(hService, OpCode, &ssStatus);


      if (bResult != 0)
      {
         int err = GetLastError();

         switch (err)
         {
            case ERROR_ACCESS_DENIED:
               ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5066, "ServiceManager::ControlService", "ControlService failed. The specified handle was not opened with the necessary access.");
               return false;
            case ERROR_DEPENDENT_SERVICES_RUNNING:
               ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5067, "ServiceManager::ControlService", "ControlService failed. The service cannot be stopped because other running services are dependent on it.");
               return false;
            case ERROR_INVALID_HANDLE:
               ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5068, "ServiceManager::ControlService", "ControlService failed. The specified handle was not obtained using CreateService or OpenService, or the handle is no longer valid.");
               return false;
            case ERROR_INVALID_PARAMETER:
               ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5069, "ServiceManager::ControlService", "ControlService failed. The requested control code is undefined.");
               return false;
            case ERROR_INVALID_SERVICE_CONTROL:
               ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5070, "ServiceManager::ControlService", "ControlService failed. The requested control code is not valid, or it is unacceptable to the service.");
               return false;
            case ERROR_SERVICE_CANNOT_ACCEPT_CTRL:
               ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5071, "ServiceManager::ControlService", "ControlService failed. The requested control code cannot be sent to the service because the state of the service is SERVICE_STOPPED, SERVICE_START_PENDING, or SERVICE_STOP_PENDING.");
               return false;
            case ERROR_SERVICE_NOT_ACTIVE:
               ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5072, "ServiceManager::ControlService", "ControlService failed. The service has not been started.");
               return false;
            case ERROR_SERVICE_REQUEST_TIMEOUT:
               ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5073, "ServiceManager::ControlService", "ControlService failed. The process for the service was started, but it did not call StartServiceCtrlDispatcher, or the thread that called StartServiceCtrlDispatcher may be blocked in a control handler function.");
               return false;
            case ERROR_SHUTDOWN_IN_PROGRESS:
               ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5074, "ServiceManager::ControlService", "ControlService failed. The system is shutting down.");
               return false;
         }
      }


      CloseServiceHandle (hSCManager);
      CloseServiceHandle (hService);

      if (bResult)
         return true;
      else
         return false;
   }

   SERVICE_STATUS
   ServiceManager::GetServiceStatus(const String &ServiceName)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Stops the service with name ServiceName on the local computer.
   //---------------------------------------------------------------------------()
   {
      // Retrieve the handle for the local service manager.
      SC_HANDLE hSCManager;
      hSCManager = OpenSCManager( NULL, NULL, GENERIC_READ );
 
      // Retrieve the handle for the service.
      SC_HANDLE hService = OpenService( hSCManager, ServiceName, SERVICE_QUERY_STATUS );
   
      SERVICE_STATUS Status;
      BOOL bResult = QueryServiceStatus(hService, &Status);

      int e = GetLastError();

      CloseServiceHandle (hSCManager);
      CloseServiceHandle (hService);

      return Status;

   }

   bool
   ServiceManager::DoesServiceExist(const String &ServiceName)
   {
      // Retrieve the handle for the local service manager.
      SC_HANDLE hSCManager;
      hSCManager = OpenSCManager( NULL, NULL, GENERIC_READ );
 
      // Retrieve the handle for the service.
      SC_HANDLE hService = OpenService( hSCManager, ServiceName, SERVICE_QUERY_STATUS );
      
      CloseServiceHandle (hSCManager);
      CloseServiceHandle (hService);

      if (hService == NULL)
         return false;
      else
         return true;
   }

}

#endif
