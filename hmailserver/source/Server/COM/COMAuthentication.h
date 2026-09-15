// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "../Common/Application/RoleManager.h"

namespace HM
{
   class Account;

   class COMAuthentication
   {
   public:
      COMAuthentication(void);
      ~COMAuthentication(void);

      std::shared_ptr<const Account> Authenticate(const String &sUsername, const String &sPassword);

      // As above, with a TOTP code. This is the ONLY path that can satisfy a second
      // factor: a mail client has nowhere to type one, which is precisely why an
      // account with a factor enrolled authenticates there through an app password
      // instead. An empty code is not a shortcut - an enrolled account still needs a
      // valid one.
      std::shared_ptr<const HM::Account> Authenticate(const String &sUsername, const String &sPassword, const String &sCode);

      void AttempAnonymousAuthentication();

      bool GetIsAuthenticated() const;

      // The two questions this API has asked since 2007, and the ~315 places
      // in COM/Interface*.cpp that ask them, are now questions for
      // RoleManager: AdministerServer and AdministerDomain are permissions
      // like any other, held by serveradmin and domainadmin and by nothing
      // else. An account with no role assignment resolves to the grant its
      // AdminLevel has always given it, so every one of those call sites
      // answers exactly what it answered before - and a help desk or an
      // auditor, which hold neither permission, is refused every write in COM
      // without a single one of them being edited.
      bool GetIsDomainAdmin() const;
      bool GetIsServerAdmin() const;

      // The question the new roles need asked: may this credential do this
      // particular thing, here. Used at the few COM entry points a narrow role
      // reaches - a password reset, an unlock, a message trace - and refused
      // by build/check-role-choke-point.py anywhere that is not one of them.
      bool Allows(RolePermission permission) const;
      bool AllowsForDomain(RolePermission permission, const String &domainName) const;

      // Whether this credential reaches every domain rather than a list of them.
      bool ReachesEveryDomain() const;

      // What this credential holds, for the actor the audit trail records and
      // for the Control Panel, which hides a page a role may not use.
      const RoleGrant &GetGrant() const { return grant_; }

      // Installs the actor for the life of the COM session, so that a change
      // made through this API is recorded against the person who made it.
      RoleActor GetActor() const;

      __int64 GetAccountID() const;
      __int64 GetDomainID() const;

      int GetAccessDenied() const;

   private:

      // The administrator credential: the ini password, and - when one is enrolled
      // (IniFileSettings::GetAdministratorTotpSecret) - a TOTP code. codePresented
      // says which overload the caller came through: without a code, an enrolled
      // administrator is refused outright, so a stolen password is not a
      // credential on its own. Sets account_ on success.
      void AuthenticateAdministrator_(const String &sPassword, const String &sCode, bool codePresented);

      void ResolveGrant_();
      void InstallActor_() const;

      std::shared_ptr<const Account> account_;

      // Resolved once, when the credential was accepted, and read from there.
      // A role edited half way through a COM session does not change what the
      // session may do until it authenticates again - the same rule the REST
      // caller follows, for the same reason.
      RoleGrant grant_;
   };
}