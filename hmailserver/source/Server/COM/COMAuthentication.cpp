// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"

#include "./COMAuthentication.h"
#include "../Common/Util/UpdateApplyToken.h"
#include "../Common/BO/Account.h"
#include "../Common/Persistence/PersistentAccount.h"
#include "../Common/Util/PasswordValidator.h"
#include "../Common/Util/Totp.h"
#include "../Common/Util/Crypt.h"
#include "../Common/Application/IniFileSettings.h"
#include "../Common/Application/RoleManager.h"
#include "../Common/Util/Parsing/StringParser.h"
#include "../Common/Util/AuditTrail.h"
#include "../Common/Scripting/ScriptServer.h"

#include "COMError.h"
#include "../Common/Application/Application.h"
#include "../Common/SQL/SQLCommand.h"
#include "../Common/SQL/DALRecordset.h"
#include "../Common/SQL/DatabaseConnectionManager.h"

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
         bool viaAppPassword = false;
         account_ = HM::PasswordValidator::ValidatePassword(_T(""), sUsername, sPassword, &viaAppPassword);
         RefuseAppPassword_(viaAppPassword);
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
         bool viaAppPassword = false;
         account_ = HM::PasswordValidator::ValidatePassword(_T(""), sUsername, sPassword, &viaAppPassword);
         RefuseAppPassword_(viaAppPassword);
         ResolveGrant_();
         return account_;
      }

      const String secret = pAccount->GetTotpSecret();
      if (secret.IsEmpty() || sCode.IsEmpty())
      {
         // No second factor, or no code: the password as it always was - which,
         // with a factor enrolled, accepts only an app password (PasswordValidator),
         // and an app password is no COM logon (RefuseAppPassword_), so then
         // nothing signs in here. Nothing to count: no code was offered.
         HM::SecondFactorOffer noCode;
         bool viaAppPassword = false;
         if (HM::PasswordValidator::ValidatePassword(pAccount, sPassword, &noCode, &viaAppPassword))
            account_ = pAccount;

         RefuseAppPassword_(viaAppPassword);
         ResolveGrant_();
         return account_;
      }

      // The password first, and the code only once it is known to be right, so
      // that what is said and counted about the code is said and counted only
      // for somebody who holds the password - the order the administrator's
      // sign-in and every REST route use. The offer is marked satisfied for the
      // comparison, and the code is judged below.
      HM::SecondFactorOffer secondFactor;
      secondFactor.satisfied = true;
      bool viaAppPassword = false;

      if (!HM::PasswordValidator::ValidatePassword(pAccount, sPassword, &secondFactor, &viaAppPassword))
      {
         ResolveGrant_();
         return account_;
      }

      // An app password is not a COM logon (RefuseAppPassword_), with a code
      // beside it or not, so the code is neither judged nor counted: it is
      // refused as the overloads without a code refuse it.
      if (viaAppPassword)
      {
         account_ = pAccount;
         RefuseAppPassword_(viaAppPassword);
         ResolveGrant_();
         return account_;
      }

      if (!AcceptCode_(pAccount->GetAddress(), secret, sCode))
      {
         ResolveGrant_();
         return account_;
      }

      account_ = pAccount;
      ResolveGrant_();

      return account_;
   }

   bool
   COMAuthentication::AcceptCode_(const String &owner, const String &secret, const String &sCode)
   {
      // Counted, where the password on this path is not. The per-name lockout
      // and the auto-ban stay out of COM for the reason Authenticate gives -
      // administration has to stay reachable - but a wrong code is a guess at
      // six digits by somebody who already has the password, and the only
      // thing between them and the account. So it is counted against the
      // owner's own wrong-code budget, the one the REST API counts against
      // too (SecondFactorGuard): five wrong codes in fifteen minutes, and no
      // code is taken for fifteen more, whichever door they were tried at. The
      // budget is asked, the code judged and a wrong one counted under one
      // lock (TakeCode), so that calls racing each other cannot all be judged
      // before the fifth is counted.
      HM::SecondFactorGuard::Outcome outcome;
      const HM::SecondFactorGuard::Taken taken = HM::SecondFactorGuard::TakeCode(owner, AnsiString(secret), AnsiString(sCode), &outcome);
      if (taken == HM::SecondFactorGuard::CodeTaken)
         return true;

      if (taken == HM::SecondFactorGuard::CodeLocked)
      {
         LOG_APPLICATION("COM logon of " + owner + " refused: the password was correct but no one-time code is taken for it now - too many wrong codes have been tried for it. Try again in " +
            HM::StringParser::IntToString((outcome.retry_after + 59) / 60) + " minute(s).");
         return false;
      }

      // Taken once (RFC 6238 section 5.2): a code already accepted for this
      // owner - here or over the REST API - is refused, and not counted: it is
      // no guess, and the one most likely to send it again is the owner.
      if (taken == HM::SecondFactorGuard::CodeReused)
      {
         LOG_APPLICATION("COM logon of " + owner + " refused: the password was correct but the one-time code had been used already. A code is taken once; the next one the authenticator shows is. Not counted as a wrong code.");
         return false;
      }

      const bool lockedNow = outcome.locked_now;
      LOG_APPLICATION("COM logon of " + owner + " refused: the password was correct but the one-time code was not (" +
         HM::StringParser::IntToString(outcome.wrong) + " in fifteen minutes).");

      if (lockedNow)
      {
         LOG_APPLICATION("COM logon: " + owner + " has had five wrong one-time codes in fifteen minutes; no code for it is taken for the next fifteen.");
      }

      return false;
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

      // The hash as [Security] AdministratorPassword holds it, kept apart from the
      // stand-in below: it is what a successful sign-in re-hashes.
      const String stored = HM::IniFileSettings::Instance()->GetAdministratorPassword();
      String sPasswordCorrect = stored;

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
      HM::Crypt::PasswordMatch match;
      if (!HM::Crypt::Instance()->Validate(sPassword, sPasswordCorrect, type, &match))
      {
         if (match.refused_old_conversion)
            LOG_APPLICATION(_T("Administrator logon refused: the stored administrator password hash was made before 6.3.4 from a password with a character above U+00FF, of which that version kept only the length, so any password of the same form would match it. Set the administrator password again (hMailServer.exe --set-admin-password)."));
         return;
      }

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
         // An empty code is no code: refused, and not counted as a guess, as the
         // REST API does not count a request with no X-hMailServer-OTP.
         if (!codePresented || sCode.IsEmpty())
         {
            LOG_APPLICATION("Administrator logon refused: the password was correct but a second factor is enrolled and no code was presented. Authenticate with a code (Application.AuthenticateWithCode).");
            return;
         }

         // Counted and taken once, against the same budget and the same record
         // of steps the REST API's administrator sign-in uses.
         if (!AcceptCode_(_T("administrator"), secret, sCode))
            return;
      }

      // Signed in. A password that verified against the installer's old unsalted
      // MD5, or against a hash cheaper than the one the server makes now, is written
      // again as PBKDF2, as an account's is at its next logon.
      if (!stored.IsEmpty())
         HM::IniFileSettings::Instance()->UpgradeAdministratorPasswordHash(match.text, stored, match.legacy_bytes);

      // Create a dummy account since the administrator
      // does not have a real email account.
      account_ = std::shared_ptr<Account>(new Account("Administrator", Account::ServerAdmin));
   }

   void
   COMAuthentication::RefuseAppPassword_(bool viaAppPassword)
   {
      // An app password is a credential for a program that reads and sends
      // mail: it is what a second factor leaves working in a client that cannot
      // present a code, and whoever administers the account's domain may issue
      // one. Accepted here, it was a COM logon with the account's whole role
      // and no second factor - so an app password a domain administrator
      // issued on the server administrator's mailbox in its domain logged on
      // as the server administrator. The REST listener has always taken the
      // role away from one (ApplyAccountGrant_); COM, which is administration
      // and nothing else, does not accept one at all, and from outside an app
      // password reads as a wrong password does.
      if (!viaAppPassword || !account_)
         return;

      LOG_APPLICATION("COM logon refused for " + account_->GetAddress() +
         ": the password given is one of the account's app passwords, which open the mailbox in a mail client and never the administration interface.");

      account_.reset();
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

   bool
   COMAuthentication::CallerIsEventScript()
   {
      return ScriptServer::IsScriptRunningOnThisThread();
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
   COMAuthentication::TakesInAccount(__int64 accountID) const
   {
      InstallActor_();

      std::shared_ptr<Account> stored = std::shared_ptr<Account>(new Account());

      if (accountID <= 0 || !PersistentAccount::ReadObject(stored, accountID))
         return false;

      return RoleManager::Encompasses(grant_, RoleManager::GrantForAccount(stored));
   }

   const wchar_t *const COMAuthentication::NotTakenIn =
      L"You do not have access to change this. The account it belongs to is not one this credential administers, "
      L"or holds administrative authority that this credential does not.";

   bool
   COMAuthentication::MayChangeAccount(__int64 accountID) const
   {
      InstallActor_();

      if (!account_)
         return CallerIsEventScript();

      if (accountID <= 0)
         return true;

      std::shared_ptr<Account> stored = std::shared_ptr<Account>(new Account());
      if (!PersistentAccount::ReadObject(stored, accountID))
         return GetIsServerAdmin();

      const bool itself = account_->GetID() == stored->GetID();
      const bool domainAdministrator = GetIsDomainAdmin() && GetDomainID() == stored->GetDomainID();

      if (!GetIsServerAdmin() && !domainAdministrator && !itself)
         return false;

      return RoleManager::Encompasses(grant_, RoleManager::GrantForAccount(stored));
   }

   bool
   COMAuthentication::MayChangeRuleOwnedBy(__int64 accountID, __int64 domainID) const
   {
      InstallActor_();

      if (!account_)
         return CallerIsEventScript();

      if (accountID > 0)
         return MayChangeAccount(accountID);

      if (domainID > 0)
         return GetIsServerAdmin() || (GetIsDomainAdmin() && GetDomainID() == domainID);

      return GetIsServerAdmin();
   }

   bool
   COMAuthentication::MayChangeRule(__int64 ruleID) const
   {
      if (!account_)
         return CallerIsEventScript();

      if (ruleID <= 0)
         return true;

      SQLCommand command("select ruleaccountid, ruledomainid from hm_rules where ruleid = @RULEID");
      command.AddParameter("@RULEID", ruleID);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset || recordset->IsEOF())
         return GetIsServerAdmin();

      return MayChangeRuleOwnedBy(recordset->GetInt64Value("ruleaccountid"), recordset->GetInt64Value("ruledomainid"));
   }

   bool
   COMAuthentication::MayChangeFolder(__int64 folderID) const
   {
      if (!account_)
         return CallerIsEventScript();

      if (folderID <= 0)
         return true;

      SQLCommand command("select folderaccountid from hm_imapfolders where folderid = @FOLDERID");
      command.AddParameter("@FOLDERID", folderID);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset || recordset->IsEOF())
         return GetIsServerAdmin();

      return MayChangeAccount(recordset->GetInt64Value("folderaccountid"));
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