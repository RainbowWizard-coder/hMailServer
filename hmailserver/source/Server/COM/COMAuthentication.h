// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "../Common/Application/RoleManager.h"
#include "../Common/Util/AuditTrail.h"

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

      // Who is acting, as RoleManager holds it. Installed beside the audit
      // trail's actor below, at the same moments and from the same grant, so
      // that the two cannot describe two different people.
      RoleActor GetActor() const;

      // The audit trail's identity chokepoint for COM. GetIsServerAdmin,
      // GetIsDomainAdmin and the three role questions above are the
      // authorisation questions every COM write asks before it writes - each
      // of them installs this actor, and RoleManager's, through InstallActor_ -
      // and they are asked ON THE THREAD that is about to do the writing -
      // which matters, because hMailServer's COM server is an out-of-process
      // MTA and RPC dispatches each call on whichever pool thread is free. An
      // actor installed when the client authenticated would be on a thread
      // that never writes anything.
      //
      // The COM methods that write without asking - the settings property
      // setters, which are reachable only once LoadSettings has asked once -
      // carry an AuditScope of their own built from this; see
      // COMAuthenticator::GetAuditActor and build/check-audit-choke-point.py.
      //
      // Its kind and its role come from the grant, never from AdminLevel alone:
      // an account whose AdminLevel says domain administrator but which has
      // been given the help desk role is recorded as the account it is,
      // holding the role it holds - not as an administrator.
      AuditTrail::Actor GetAuditActor() const;

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

      // The one-time code of an owner - an account's address, or
      // "administrator" - whose password has just been accepted: refused while
      // the owner's wrong-code budget is spent, refused and counted when it is
      // wrong, refused and not counted when it has been taken once already, and
      // taken otherwise (SecondFactorGuard, Totp.h, which the REST API shares).
      // Logs each refusal.
      static bool AcceptCode_(const String &owner, const String &secret, const String &sCode);

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