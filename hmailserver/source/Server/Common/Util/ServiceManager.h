// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#ifndef HM_PLATFORM_POSIX

// Windows only, in its entirety. Every member of this class is a call into the
// Service Control Manager - OpenSCManager, CreateService, ChangeServiceConfig,
// ControlService - and one of them hands back the SCM's own SERVICE_STATUS. There
// is nothing here to translate: on a POSIX machine the job this class does belongs
// to the init system, and a program does not register itself with systemd - a unit
// file does, written by whoever installs the package. That is the roadmap section
// "Linux and AArch64", and it is packaging work rather than a compilation fix.
//
// Guarded away rather than given stubs for the same reason ExceptionLogger is: a
// StartServiceOnLocalComputer that answered anything at all would be answering
// about a service that does not exist. Every caller is already Windows-only -
// Server/COM, which is the ATL administration API, and hMailServer.exe - and
// BackupExecuter.cpp includes this header under the same guard without using
// anything from it. A POSIX build that acquired a caller would fail to link and
// say so, which is the honest failure.

#include <Winsvc.h>

namespace HM
{
   class ServiceManager  
   {
   public:
      ServiceManager();
      virtual ~ServiceManager();

      // Which account RegisterService gives the service, and it comes from the
      // command line and nowhere else: hMailServer.exe --service-account
      // virtual|localsystem|keep|<DOMAIN\name$> /Register (docs/ServiceAccount.md),
      // which the installer and the Control Panel pass. AccountKeep - no
      // --service-account at all - leaves the account of a service that exists
      // as it is, and makes a new one LocalSystem, as /Register always has.
      //
      // Never from hMailServer.INI any more, where ServiceAccountName used to be
      // read. /Register runs elevated, and that value is written by others: it
      // is a row of the settings store, which hMailServer administrators - who
      // need not be Windows administrators - set over REST and COM, and which
      // the service writes; and the file itself is written by whichever account
      // last could. A registration that took its account from there would let
      // any of them make the next elevated /Register give the service
      // LocalSystem back. Asking whether the file is writable only by
      // administrators would answer for one of the two places the value comes
      // from, so the value is not read at all.
      enum AccountChoice
      {
         AccountKeep = 0,
         AccountVirtual = 1,
         AccountLocalSystem = 2,
         AccountManaged = 3
      };

      // AccountManaged takes the account's name: a group managed service
      // account (DOMAIN\name$), which has no password for anybody to pass.
      void SetAccountChoice(AccountChoice choice, const String &accountName = String()) { account_choice_ = choice; account_name_ = accountName; }

      // The logon account for CreateService and ChangeServiceConfig, from a
      // choice: change false for "no change" (LocalSystem on create). The
      // password is never taken from anywhere - an account that has one is set
      // in services.msc, and an upgrade keeps it - so it is NULL for the
      // virtual and managed accounts, which have none, and an empty string, not
      // NULL, for LocalSystem, as CreateService and ChangeServiceConfig ask.
      // False, with a sentence in refusal, for a name that is not a managed
      // service account. Public and static for ServiceAccountTester.
      struct Logon
      {
         bool change;
         String name;
         bool emptyPassword;
      };

      static bool LogonFor(AccountChoice choice, const String &serviceName, const String &accountName, Logon &logon, String &refusal);

      bool RegisterService(const String &ServiceName, const String &ServiceCaption);
      bool UnregisterService(const String &ServiceName);

      // What RegisterService reported, each sentence on a line of its own:
      // why it failed, and what it could not do that did not stop it (the
      // SID type). hMailServer.exe /Register writes these on its own output,
      // since an administrator's verb writes nothing to the error log
      // (hMailServer.cpp, ForbidServiceFolderWrites).
      const String &GetReported() const { return reported_; }

      // False, with a sentence in error, when the dependency could not be set -
      // which is always the case for a service that is not LocalSystem asking to
      // change its own configuration: that right is deliberately not granted to
      // it, since a service that may rewrite its own entry may also make itself
      // LocalSystem again.
      bool MakeDependentOn(const String &ServiceName, String &error);

      bool StartServiceOnLocalComputer(const String &ServiceName);
      bool StopServiceOnLocalComputer(const String &ServiceName);

      SERVICE_STATUS GetServiceStatus(const String &ServiceName);

      bool UserControlService(const String &ServiceName, DWORD OpCode);
      bool DoesServiceExist(const String &ServiceName);

   private:

      // SERVICE_SID_TYPE_UNRESTRICTED, so that the service's token carries
      // NT SERVICE\<name> whatever account it logs on as, and the grants
      // hMailServer.exe /GrantServiceAccess makes to that SID reach it. Set only
      // when the service ends up on an account other than LocalSystem, which
      // gains nothing from it - so a registration that keeps LocalSystem, the
      // bench's and every upgrade's that keeps it, changes nothing it did not
      // change before. Reported and not fatal when it cannot be set.
      void SetUnrestrictedSid_(SC_HANDLE hService);

      // Whether the service's entry names LocalSystem now, read back after a
      // change. True when it cannot be read: then the SID type is left alone.
      static bool RunsAsLocalSystem_(SC_HANDLE hService);

      bool ReconfigureService_(SC_HANDLE hSCMManager, const String &ServiceName);

      // An error ErrorManager is given, kept for GetReported as well.
      void Report_(int errorId, const String &source, const String &sentence);

      AccountChoice account_choice_;
      String account_name_;
      String reported_;
   };
}

#endif
