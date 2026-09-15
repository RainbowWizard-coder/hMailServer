// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"

#include "./COMAuthentication.h"
#include "../Common/Util/UpdateApplyToken.h"
#include "../Common/BO/Account.h"
#include "../Common/Util/PasswordValidator.h"
#include "../Common/Util/Totp.h"
#include "../Common/Util/Crypt.h"
#include "../Common/Application/IniFileSettings.h"
#include "../Common/Application/RoleManager.h"
#include "../Common/Util/Parsing/StringParser.h"
#include "../Common/Util/AuditTrail.h"

#include "COMError.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   COMAuthentication::COMAuthentication(void)
   {
      
   }

   COMAuthentication::~COMAuthentication(void)
   {

   }

   std::shared_ptr<const Account>
   COMAuthentication::Authenticate(const String &sUsername, const String &sPassword)
   {
      // Try to fetch this account
      account_.reset();

      if (sUsername.CompareNoCase(_T("administrator")) == 0)
      {
         // No code on this overload: satisfiable only while no second factor is
         // enrolled on the administrator credential.
         AuthenticateAdministrator_(sPassword, String(), false);
      }
      else
      {
         // Deliberately outside the per-name authentication lockout (see
         // AccountLockout.h) and, as it always has been, outside the per-IP
         // auto-ban. This is the administration API: the Control Panel logs on
         // through it, so a lockout reachable from here would mean that guessing
         // at an administrator's mailbox name from the internet - over IMAP, POP3
         // or SMTP, where the lockout DOES apply - could also lock them out of the
         // tool they need in order to respond. Administration stays available on
         // purpose; DCOM access is itself an authenticated Windows privilege, not
         // an anonymous mail port.
         account_ = HM::PasswordValidator::ValidatePassword(sUsername, sPassword);
      }

      ResolveGrant_();

      return account_;
   }

   std::shared_ptr<const HM::Account>
   COMAuthentication::Authenticate(const String &sUsername, const String &sPassword, const String &sCode)
   {
      if (sUsername.CompareNoCase(_T("administrator")) == 0)
      {
         account_.reset();
         AuthenticateAdministrator_(sPassword, sCode, true);
         ResolveGrant_();
         return account_;
      }

      account_.reset();

      std::shared_ptr<const HM::Account> pAccount = HM::PasswordValidator::LookupAccount(sUsername);

      if (!pAccount)
      {
         // No account, so nothing to verify a code against. Deliberately still runs
         // the ordinary path rather than returning early, so that an unknown name and
         // a wrong password remain indistinguishable from out here.
         account_ = HM::PasswordValidator::ValidatePassword(sUsername, sPassword);
         ResolveGrant_();
         return account_;
      }

      const bool secondFactorSatisfied =
         !pAccount->GetTotpSecret().IsEmpty() &&
         HM::Totp::VerifyCode(AnsiString(pAccount->GetTotpSecret()), AnsiString(sCode));

      if (HM::PasswordValidator::ValidatePassword(pAccount, sPassword, secondFactorSatisfied))
         account_ = pAccount;

      ResolveGrant_();

      return account_;
   }

   void
   COMAuthentication::AuthenticateAdministrator_(const String &sPassword, const String &sCode, bool codePresented)
   {
      // The update helper's database upgrade: the single-use token the server
      // issued for it (UpdateApplyToken), in place of a password nobody typed. It
      // stands in for the second factor as well, being the server's own proof of
      // its own action.
      if (sPassword.StartsWith(_T("token:")))
      {
         if (HM::UpdateApplyToken::Redeem(sPassword))
            account_ = std::shared_ptr<Account>(new Account("Administrator", Account::ServerAdmin));
         return;
      }

      String sPasswordCorrect = HM::IniFileSettings::Instance()->GetAdministratorPassword();

      if (sPasswordCorrect.IsEmpty())
      {
         // The administrators password has not been set yet. It's likely
         // that we have just installed or upgraded hMailServer.

         // Has the empty password, so we can compare. The upgrade tool first
         // tries to authenticate with an empty password.
         sPasswordCorrect = HM::Crypt::Instance()->EnCrypt(sPasswordCorrect, HM::Crypt::ETSHA256);
      }

      Crypt::EncryptionType type = HM::Crypt::Instance()->GetHashType(sPasswordCorrect);

      // Validate the password.
      if (!HM::Crypt::Instance()->Validate(sPassword, sPasswordCorrect, type))
         return;

      // The second factor, checked only once the password has been accepted, so
      // that a wrong password and a missing code are answered identically from
      // outside (both: no account) and the log line below is written only for
      // somebody who holds the password. That is exactly when the administrator
      // needs to be told, and never in response to a guess.
      const String secret = HM::IniFileSettings::Instance()->GetAdministratorTotpSecret();
      if (HM::IniFileSettings::Instance()->IsAdministratorTotpEnrolled() && secret.IsEmpty())
      {
         // Enrolled, and the secret cannot be opened - a configuration moved to a
         // machine without its key, or a store that has failed. Refusing is the
         // only safe answer: accepting would turn a lost key into a switched-off
         // second factor without anyone choosing that.
         HM::ErrorManager::Instance()->ReportError(HM::ErrorManager::High, 6417, "COMAuthentication",
            "Administrator logon refused: a second factor is enrolled but its secret cannot be opened on this machine (see the preceding secret-store error). Restore the key the secret was protected with, or clear AdministratorTotpSecret in hMailServer.ini and enrol again.");
         return;
      }

      if (!secret.IsEmpty())
      {
         if (!codePresented)
         {
            LOG_APPLICATION("Administrator logon refused: the password was correct but a second factor is enrolled and no code was presented. Authenticate with a code (Application.AuthenticateWithCode).");
            return;
         }

         if (!HM::Totp::VerifyCode(AnsiString(secret), AnsiString(sCode)))
         {
            LOG_APPLICATION("Administrator logon refused: the password was correct but the one-time code was not.");
            return;
         }
      }

      // Create a dummy account since the administrator
      // does not have a real email account.
      account_ = std::shared_ptr<Account>(new Account("Administrator", Account::ServerAdmin));
   }

   void
   COMAuthentication::AttempAnonymousAuthentication()
   {
      // No authentication is required if the administration password is empty -
      // and, since a second factor guards a password, only while there is none of
      // those either.
      String sAdminPassword = HM::IniFileSettings::Instance()->GetAdministratorPassword();
      if (sAdminPassword.IsEmpty() && HM::IniFileSettings::Instance()->GetAdministratorTotpSecret().IsEmpty())
      {
         // Create a dummy account since the administrator
         // does not have a real email account.

         account_ = std::shared_ptr<Account> (new Account("Administrator", Account::ServerAdmin));
      }

      ResolveGrant_();
   }

   bool 
   COMAuthentication::GetIsAuthenticated() const
   {
      return account_ != 0;
   }

   __int64 
   COMAuthentication::GetAccountID() const
   {
      // Nothing authenticated is not an account, and answering 0 is what every
      // caller of this already treats as "not that account" - where throwing
      // through a COM boundary would be a crash with no message.
      if (!account_)
         return 0;

      return account_->GetID();
   }

   __int64 
   COMAuthentication::GetDomainID() const
   {
      return account_->GetDomainID();
   }

   void
   COMAuthentication::ResolveGrant_()
   {
      grant_ = RoleGrant();

      if (!account_)
         return;

      // The administrator credential has no account row behind it - the object
      // above is a stand-in with the name "Administrator" and no id - so it is
      // recognised here rather than looked up, and is the server
      // administrator, as it always has been.
      if (account_->GetID() == 0 && account_->GetAdminLevel() == Account::ServerAdmin)
      {
         grant_ = RoleManager::ServerAdministrator();
         return;
      }

      grant_ = RoleManager::GrantForAccount(account_);
   }

   void
   COMAuthentication::InstallActor_() const
   {
      // Installed here, on whichever thread is answering this call, rather
      // than once when the credential was authenticated - because DCOM does
      // not promise that the thread which authenticated is the thread that
      // later sets a property. Every administrative entry point in COM asks
      // one of the authority questions below before it changes anything,
      // so the actor is in place on the right thread for the write that
      // follows, and is replaced by the next question rather than left to go
      // stale. It is the credential of the object being used, which is what
      // "who did this" means.
      //
      // Two actors, installed together from the one grant: RoleManager's, and
      // the audit trail's, which is what AuditTrail::RecordStatement and
      // RecordSettingChange read when the write that follows lands. This is
      // the COM half of the audit trail's identity chokepoint (see
      // AuditTrail.h and build/check-audit-choke-point.py), and it is reached
      // from every question - GetIsServerAdmin and GetIsDomainAdmin as before,
      // and the three a narrow role's entry points ask - so a password a help
      // desk resets over COM is on the record as the help desk's.
      RoleManager::SetCurrentActor(GetActor());
      AuditTrail::SetActor(GetAuditActor());
   }

   bool
   COMAuthentication::GetIsDomainAdmin() const
   {
      InstallActor_();

      // Reads exactly as it did: serveradmin holds every permission, so
      // AdministerDomain is among them, and an account with AdminLevel =
      // DomainAdmin and no assignment resolves to the domainadmin grant, which
      // holds it. A help desk and an auditor hold neither, which is what
      // refuses them every COM write without one of the ~200 call sites that
      // ask this question being touched.
      return RoleManager::Allows(grant_, RolePermission::AdministerDomain);
   }

   bool
   COMAuthentication::GetIsServerAdmin() const
   {
      InstallActor_();

      return RoleManager::Allows(grant_, RolePermission::AdministerServer);
   }

   bool
   COMAuthentication::Allows(RolePermission permission) const
   {
      InstallActor_();

      return RoleManager::Allows(grant_, permission);
   }

   bool
   COMAuthentication::AllowsForDomain(RolePermission permission, const String &domainName) const
   {
      InstallActor_();

      String reason;
      return RoleManager::Allows(grant_, permission, domainName, reason);
   }

   bool
   COMAuthentication::ReachesEveryDomain() const
   {
      InstallActor_();

      return RoleManager::ReachesEveryDomain(grant_);
   }

   RoleActor
   COMAuthentication::GetActor() const
   {
      RoleActor actor;

      if (!account_)
         return actor;

      if (account_->GetID() == 0)
         actor.identity = "administrator";
      else
         actor.identity = AnsiString("account:") + AnsiString(account_->GetAddress());

      actor.account_id = account_->GetID();
      actor.domain_id = account_->GetDomainID();
      actor.grant = grant_;

      return actor;
   }

   /*
      Who this COM call is being made by, for the audit trail. Empty (present ==
      false) for an unauthenticated caller, which cannot write anything anyway.

      The address is deliberately blank: a COM call arrives through DCOM with no
      peer address this process can see, and inventing one would be worse than
      saying nothing. What the row does say is the interface - COM - which is the
      Control Panel, hmconfig.ps1, DBSetup, a script, or anything else holding the
      administrator password.

      The kind and the role are read from the grant this credential was given
      when it authenticated, the grant every decision above is made from - not
      from AdminLevel. Before roles the two were the same thing; now an account
      whose AdminLevel is DomainAdmin can hold the help desk role, and recording
      it as an administrator because of the column would describe authority it
      does not have. "administrator" is the administrator password and the two
      grants that have always meant administrator (serveradmin, domainadmin);
      every other account is an "account", and the role it acted under, when it
      holds one, is recorded beside the change.
   */
   AuditTrail::Actor
   COMAuthentication::GetAuditActor() const
   {
      AuditTrail::Actor actor;

      if (!account_)
         return actor;

      actor.present = true;
      actor.name = account_->GetAddress();
      actor.interface_name = _T("COM");
      actor.kind = RoleManager::IsAdministratorGrant(grant_) ? _T("administrator") : _T("account");

      if (grant_.administrative)
         actor.role = grant_.name;

      return actor;
   }

   int 
   COMAuthentication::GetAccessDenied() const
   {
      return COMError::GenerateError("You do not have access to this property / method. Ensure that hMailServer.Application.Authenticate() is called with proper login credentials.");
   }

}