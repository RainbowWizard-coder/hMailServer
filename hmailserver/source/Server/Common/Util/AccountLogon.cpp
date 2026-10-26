// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#include "StdAfx.h"

#include "AccountLogon.h"
#include "AccountLockout.h"
#include "CountryBlocking.h"
#include "AutoBanFirewall.h"
#include "NeverBanNameList.h"
#include "../Application/IniFileSettings.h"
#include "PasswordValidator.h"
#include "PasswordHistory.h"
#include "MailboxKeys.h"
#include "../Persistence/PersistentLogonFailure.h"
#include "../Persistence/PersistentAccount.h"
#include "../Persistence/PersistentSecurityRange.h"

#include "../BO/SecurityRange.h"
#include "../BO/Account.h"
#include "../BO/DomainAliases.h"
#include "../Application/ObjectCache.h"
#include "../Application/DefaultDomain.h"
#include "VariantDateTime.h"
#include "Time.h"
#include "GUIDCreator.h"
#include "PasswordGenerator.h"
#include "ServerStatus.h"
#include "Unicode.h"

#include <set>
#include <stdexcept>
#include <vector>


#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   boost::recursive_mutex AccountLogon::ip_range_creation_mutex_;

   // What every auto-ban's range name begins with: what the firewall reconciliation
   // mirrors and the expiry sweep lifts (AutoBanFirewall, NeverBanNameList,
   // AutoBanWidener), and the Control Panel lists as a ban.
   const TCHAR *const AccountLogon::AutoBanRangePrefix = _T("Auto-ban: ");

   AccountLogon::AccountLogon(void)
   {
   }

   AccountLogon::~AccountLogon(void)
   {
   }

   std::shared_ptr<const Account>
   AccountLogon::Logon(const IPAddress &ipaddress, const String &username, const String &password, bool &disconnect,
                       bool *matchedAppPassword, String *matchedAppPasswordName, SecondFactorOffer *secondFactor,
                       bool chargeAddress)
   {
      return Logon(ipaddress, _T(""), username, password, disconnect, matchedAppPassword, matchedAppPasswordName, secondFactor,
                   chargeAddress);
   }

   std::shared_ptr<const Account>
   AccountLogon::Logon(const IPAddress &ipaddress, const String &masqname, const String &username, const String &password, bool &disconnect,
                       bool *matchedAppPassword, String *matchedAppPasswordName, SecondFactorOffer *secondFactor,
                       bool chargeAddress)
   {
      disconnect = false;
      if (matchedAppPassword)
         *matchedAppPassword = false;
      if (matchedAppPasswordName)
         *matchedAppPasswordName = String();

      // Per-name lockout, checked BEFORE the password: a locked name is refused
      // without spending an Argon2id verification on it, and the refusal is the
      // ordinary invalid-credentials reply (see AccountLockout.h for what timing
      // still reveals).
      //
      // The refusal is deliberately NOT reported to RegisterFailedLogin, so it
      // reaches neither the per-name counters nor the per-IP auto-ban.
      //
      // The per-IP half is the one that matters, and it is a real hazard rather
      // than tidiness. Once a name is locked, the person most likely to keep
      // trying is its owner, with the CORRECT password - and charging those
      // attempts to their address is how a lockout caused by a botnet gets an
      // innocent office NAT, or the loopback address of a co-hosted webmail
      // front end, banned from SMTP, IMAP and POP3 for everybody behind it by a
      // priority-100 deny-everything range. An attack on one mailbox must not
      // become an outage for unrelated users at the victim's address.
      //
      // Nothing is lost in the other direction: an attacker who reaches this
      // branch has already been charged for the failures that caused the lock,
      // and further attempts against a name that can no longer be verified tell
      // them nothing, so declining to count them removes no pressure that was
      // doing any work. The per-connection attempt caps and
      // OnAuthenticationFailed still see every attempt.
      if (AccountLockout::Instance()->IsLockedOut(username))
      {
         ServerStatus::Instance()->OnAuthenticationFailed();

         std::shared_ptr<Account> empty;
         return empty;
      }

      // The country policy of the domain the name belongs to, checked here for
      // the same reason the lockout is: before a password is verified, so that
      // an address a domain will not accept sign-ins from cannot spend an
      // Argon2id verification, and so that every protocol gets the decision -
      // this is the one function IMAP LOGIN and AUTHENTICATE, POP3, SMTP AUTH,
      // ManageSieve, the DAV services and the REST API all pass through.
      //
      // Refused is the ordinary invalid-credentials answer, and it is
      // deliberately NOT reported to RegisterFailedLogin, for the reason the
      // lockout gives above and for one more of its own: the person refused here
      // is very often the account's real owner, travelling. Charging their
      // attempts to the per-name lockout would let a trip abroad lock somebody
      // out of their mailbox at home, and charging them to the per-IP auto-ban
      // would ban a hotel's NAT for everyone behind it. The refusal is counted
      // as what it is - a country refusal - and logged as one.
      if (!CountryBlocking::Instance()->IsLogonAllowed(username, ipaddress))
      {
         std::shared_ptr<Account> empty;
         return empty;
      }

      // Whether an app password matched is asked here too, whatever the caller
      // asked: only the account's own password opens its mailbox key.
      bool viaAppPassword = false;
      std::shared_ptr<const Account> account = PasswordValidator::ValidatePassword(masqname, username, password, &viaAppPassword, matchedAppPasswordName, secondFactor);
      if (matchedAppPassword)
         *matchedAppPassword = viaAppPassword;

      // The account the password proved, asked again by its own address now that
      // it is resolved: its compromise hold and its country policy
      // (RefusedByHold), and, when it is the account that authenticated, its
      // per-name lockout. The checks above asked of the name as the client gave
      // it, and PasswordValidator takes a name for an account only when the
      // account's stored address is that name (LogonName.h), whose key is the
      // account's own - so these refuse nothing the early checks let through,
      // unless that rule is ever got round: then no spelling of a name reaches a
      // held or a locked account all the same. Up to 6.3.7 the database's
      // comparison chose the account, and a held account was signed into under
      // another spelling of its name with its password. The password was right,
      // so a refusal here is charged to nothing, as a hold or a lock is.
      //
      // An authorisation identity that is not the name that authenticated (the
      // IMAP master user, RFC 4616): the checks above judged the account that
      // proved the password, and the session is the other one's. Its country
      // policy and its compromise hold are asked here, with its own address, so
      // that a master user in a domain with no policy cannot open a mailbox
      // from a country the mailbox's domain or the mailbox itself refuses, nor
      // one held for review. Until 26 September 2026 neither was asked. Its
      // per-name lockout is not: that counts guesses at the mailbox's own
      // password, and a master user's tool is not to be locked out of a mailbox
      // by somebody guessing at it.
      const bool asItself = masqname.IsEmpty() || masqname.CompareNoCase(username) == 0;
      if (account)
      {
         if (asItself && AccountLockout::Instance()->IsLockedOut(account->GetAddress()))
         {
            ServerStatus::Instance()->OnAuthenticationFailed();

            std::shared_ptr<Account> empty;
            return empty;
         }

         if (RefusedByHold(account, ipaddress))
         {
            std::shared_ptr<Account> empty;
            return empty;
         }
      }

      if (account)
      {
         PersistentAccount::UpdateLastLogonTime(account);
         AccountLockout::Instance()->RecordSuccess(username);
         ServerStatus::Instance()->OnAuthenticationSucceeded();

         // Mailbox encryption at rest (MailboxKeys.h): the one moment the
         // account's own password is in hand, which is what opens its key -
         // not an app password, and not another account's password signing in
         // as this one (an authorisation identity that is not the name that
         // authenticated, RFC 4616).
         if (!viaAppPassword && asItself)
            MailboxKeys::OnPasswordLogon(account, password);

         return account;
      }

      ServerStatus::Instance()->OnAuthenticationFailed();

      RegisterFailedLogin(ipaddress, username, disconnect, true, chargeAddress);

      std::shared_ptr<Account> empty;
      return empty;
   }

   bool
   AccountLogon::RefusedByHold(std::shared_ptr<const Account> &account)
   {
      try
      {
         if (!account)
            return false;

         // A room or a piece of equipment is never signed into, by a password
         // (PasswordValidator refuses it) or by anything that proves identity
         // without one - a bearer token, a Kerberos ticket, a client
         // certificate, a passkey, single sign-on - all of which ask here.
         // Refused as a failed logon, and charged to nothing, as a hold is.
         if (account->IsResource())
         {
            LOG_DEBUG("AccountLogon - " + account->GetAddress() + " is a room or a piece of equipment, which cannot be signed into; the logon is refused.");
            account.reset();
            ServerStatus::Instance()->OnAuthenticationFailed();
            return true;
         }

         if (!AccountLockout::Instance()->IsHeld(account->GetAddress()))
            return false;

         LOG_DEBUG("AccountLogon - " + account->GetAddress() + " is held for review by the compromised-account detection; the logon is refused.");
         account.reset();
         ServerStatus::Instance()->OnAuthenticationFailed();
         return true;
      }
      catch (...)
      {
         // On a logon's success path, where an exception would fail an
         // authentication that has been proved: the fallback is to allow, as
         // every entry point of AccountLockout falls back.
         return false;
      }
   }

   bool
   AccountLogon::RefusedByHold(std::shared_ptr<const Account> &account, const IPAddress &remote)
   {
      if (RefusedByHold(account))
         return true;

      if (!account)
         return false;

      // The country policy, asked with the account's own address: the name the
      // client presented has already been resolved to this account, so no
      // spelling of it chooses the policy. Until 26 September 2026 only the
      // password paths and the REST API asked it, so a stolen token, a
      // certificate or a ticket signed in from a country the domain refuses.
      if (CountryBlocking::Instance()->IsLogonAllowed(account->GetAddress(), remote))
         return false;

      LOG_DEBUG("AccountLogon - " + account->GetAddress() + " may not sign in from " + String(remote.ToString()) + " under its country policy; the logon is refused.");
      account.reset();
      return true;
   }

   bool
   AccountLogon::RefusedForScram(const std::shared_ptr<const Account> &account, const IPAddress &remote, bool &chargeNothing)
   {
      chargeNothing = false;

      if (!account)
         return false;

      // The country policy first, as Logon puts it first, and asked with the
      // address the account really has, so that no spelling of the name - an
      // alias domain's, a bare local part - chooses which policy applies.
      if (!CountryBlocking::Instance()->IsLogonAllowed(account->GetAddress(), remote))
      {
         chargeNothing = true;
         return true;
      }

      // PasswordValidator's rule, applied where SCRAM can see it: once a
      // second factor is enrolled the account's own password is not a mailbox
      // credential, and an app password - which SCRAM cannot use - is the only
      // one. Without this, the phished password of an account with TOTP opened
      // SMTP submission and the mailbox over SCRAM-SHA-256.
      if (!account->GetTotpSecret().IsEmpty())
      {
         LOG_DEBUG("AccountLogon - " + account->GetAddress() + " has a second factor enrolled, so its own password cannot sign in over SCRAM; the exchange is a forced failure.");
         return true;
      }

      if (PasswordHistory::HasExpired(account))
      {
         LOG_DEBUG("AccountLogon - the password of " + account->GetAddress() + " has passed the configured maximum age, so SCRAM cannot sign in with it; the exchange is a forced failure.");
         return true;
      }

      return false;
   }

   int
   AccountLogon::TarpitDelaySeconds(int failuresOnThisConnection)
   {
      const int perFailure = IniFileSettings::Instance()->GetLogonTarpitSeconds();

      if (perFailure <= 0 || failuresOnThisConnection <= 0)
         return 0;

      // Multiplied in 64 bits so an absurd failure count cannot wrap the product
      // into a small (or negative) delay; the cap is what a client ever sees.
      const __int64 delay = static_cast<__int64>(perFailure) * failuresOnThisConnection;

      return delay > IniFileSettings::TARPIT_MAX_SECONDS ? IniFileSettings::TARPIT_MAX_SECONDS : static_cast<int>(delay);
   }

   bool
   AccountLogon::AuthorizationIdentityIsOwn(const String &authzid, const String &authcid)
   {
      if (authzid.IsEmpty())
         return true;

      // Prepared with SASLprep, as every caller prepares the authcid it passes (RFC
      // 4013), so that an authzid spelt exactly as the authcid was - with a soft
      // hyphen, which SASLprep maps to nothing, or a letter NFKC composes - is taken
      // as the client's own and signs in, as it did when every authzid was set
      // aside, rather than refused as somebody else's. An authzid SASLprep refuses,
      // or prepares to nothing, is compared as it came.
      String preparedAuthzid;
      if (!StringParser::SaslPrep(authzid, preparedAuthzid) || preparedAuthzid.IsEmpty())
         preparedAuthzid = authzid;

      std::shared_ptr<DomainAliases> aliases = ObjectCache::Instance()->GetDomainAliases();

      const String authorization = DefaultDomain::ApplyDefaultDomain(aliases->ApplyAliasesOnAddress(preparedAuthzid));
      const String authentication = DefaultDomain::ApplyDefaultDomain(aliases->ApplyAliasesOnAddress(authcid));

      return authorization.CompareNoCase(authentication) == 0;
   }

   void
   AccountLogon::RegisterFailedLogin(const IPAddress &ipaddress, const String &username, bool &disconnect,
                                     bool countTowardsAccountLockout, bool chargeAddress)
   {
      disconnect = false;

      // The per-name half, independent of the per-IP auto-ban below and of its
      // enablement: each closes a hole the other cannot see - one address
      // spraying many names, and many addresses converging on one name. See the
      // header for the failures that deliberately do not count.
      if (countTowardsAccountLockout)
         AccountLockout::Instance()->RecordFailure(username);

      // A proxy's address, which answers for every client behind it (see the
      // header): the name has been charged above, and the address is not, as
      // an address on AutoBanNeverBan is not - no failure counted, no range
      // written and no "logon failure" line for fail2ban to count.
      if (!chargeAddress)
      {
         LOG_DEBUG("AccountLogon::RegisterFailedLogin - " + String(ipaddress.ToString()) + " forwards for other clients; the failure is counted against the name alone.");
         return;
      }

      // is auto banning enabled?
      int maxInvalidLogonAttempts = Configuration::Instance()->GetMaxInvalidLogonAttempts();

      if (Configuration::Instance()->GetAutoBanLogonEnabled() == false || maxInvalidLogonAttempts == 0)
         return;

      // An address on AutoBanNeverBan is never counted towards an auto-ban, let alone
      // banned. Its failure has been counted against the name above all the same: the
      // per-name lockout is told before this list is asked, so a name mistyped from a
      // listed address is locked as from any other. The per-connection disconnect after
      // repeated failures still applies - that is the protocol handler's, not this - so
      // a stuck client from a listed address still gets dropped; it just never reaches
      // the firewall.
      if (AutoBanFirewall::IsNeverBanned(ipaddress))
      {
         LOG_DEBUG("AccountLogon::RegisterFailedLogin - " + String(ipaddress.ToString()) + " is listed in AutoBanNeverBan; the failure is not counted.");
         return;
      }

      // The never-ban list by reverse name, as far as it is already known - it is
      // never looked up here (NeverBanNameList.h). A spared address is treated as
      // one on AutoBanNeverBan: not counted, so that no "logon failure" line is
      // written for fail2ban to count either. Said once per verdict in the
      // application log, then in the debug log.
      NeverBanNameList::Verdict spared;
      if (NeverBanNameList::IsSpared(ipaddress, spared))
      {
         const String line = "Auto-ban: " + String(ipaddress.ToString()) + " is not counted or banned for failed logons: " + spared.reason;

         if (spared.first_use)
         {
            LOG_APPLICATION(line);
         }
         else
         {
            LOG_DEBUG(line);
         }

         return;
      }

      // Log on has failed.
      PersistentLogonFailure logonFailure;
      int failureCount = logonFailure.GetCurrrentFailureCount(ipaddress) + 1; // +1 because we count the current failure as well.

      // One line per counted failure, in the application log, in a fixed shape:
      // it is what a fail2ban filter matches, and it does not depend on protocol
      // logging being on. The address is the last word before " for ".
      LOG_APPLICATION("Auto-ban: logon failure " + StringParser::IntToString(failureCount) + " of " + StringParser::IntToString(maxInvalidLogonAttempts) +
         " from " + String(ipaddress.ToString()) + " for " + username);

      if (failureCount >= maxInvalidLogonAttempts)
      {
         int minutes = Configuration::Instance()->GetAutoBanMinutes();
         if (minutes == 0)
         {
            // Disconnect, but don't block: no range is written, and the spent
            // failures are cleared at once, so that the next run of failures is
            // counted from nothing.
            ClearSpentFailures_(logonFailure, ipaddress);
            disconnect = true;
            return;
         }

         // The spent failures are cleared only once the range that bans the
         // address has been saved. When it could not be (CreateIPRange has said
         // so, HM6099), they are left: each later failure from the address, while
         // the kept failures are within MaxInvalidLogonAttemptsWithin, crosses the
         // threshold again and the range is tried again, where until 6.3.7 they
         // were cleared first and an address whose range was not written was
         // given a whole new set of attempts.
         //
         // Failures from one address at the same moment - parallel connections -
         // each read the count before any of them cleared it, so each crosses the
         // threshold. Under the lock CreateIPRange takes (recursive), the question
         // and the write are one: the first writes the range, and each after it
         // finds the address banned by an auto-ban's range at least as long as its
         // own would be, within a minute, takes that for its ban, clears the
         // failures and writes and announces nothing. One that would ban for
         // longer than the range in force writes its own.
         bool covered = false;
         bool written = false;
         {
            boost::lock_guard<boost::recursive_mutex> guard(ip_range_creation_mutex_);

            DateTime until = DateTime::GetCurrentTime();
            DateTimeSpan span;
            span.SetDateTimeSpan(0, 0, minutes > 1 ? minutes - 1 : 0, 0);
            until = until + span;

            covered = PersistentSecurityRange::AutoBanCovers(ipaddress, until);
            if (!covered)
               written = CreateIPRange(ipaddress, username, minutes);
         }

         if (covered)
         {
            ClearSpentFailures_(logonFailure, ipaddress);
            LOG_DEBUG("AccountLogon::RegisterFailedLogin - " + String(ipaddress.ToString()) + " crossed the threshold while an auto-ban's range bans it; no range is written.");
         }
         else if (written)
         {
            ClearSpentFailures_(logonFailure, ipaddress);
            AutoBanFirewall::Banned(ipaddress, minutes, username, failureCount);
         }

         disconnect = true;
         return;
      }

      // logon failed, add new failure.
      //
      // This is the counting half of auto-ban, and its result was discarded - the same
      // defect as the range save in CreateIPRange below it, one method name away from
      // the sweep that found that one, because this class does not call its writers
      // SaveObject.
      //
      // It is the more serious half. If the failure cannot be recorded the count never
      // rises, GetCurrrentFailureCount keeps returning the same number, the threshold
      // is never crossed and no ban is ever created - so an attacker guesses passwords
      // indefinitely against a server whose brute-force protection is switched on,
      // configured, and silently doing nothing at all.
      if (!logonFailure.AddFailure(ipaddress))
      {
         ErrorManager::Instance()->ReportError(ErrorManager::High, 6114, "AccountLogon::RegisterFailedLogin",
            "A failed logon could not be recorded, so it does not count towards the auto-ban threshold. While this persists, repeated logon failures from any address will never trigger a ban.");
      }
   }

   void
   AccountLogon::ClearSpentFailures_(PersistentLogonFailure &logonFailure, const IPAddress &ipaddress)
   {
      // Failing here leaves this address's spent failures in the table after the
      // ban has been created, so when the ban expires the next single failure
      // re-crosses the threshold and bans it again. That errs towards blocking
      // rather than allowing, which is why it is reported rather than treated as
      // fatal - but an address that cannot get back in after its ban expires is a
      // support call, and this is the only place that would explain it.
      if (!logonFailure.ClearFailuresByIP(ipaddress))
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6113, "AccountLogon::RegisterFailedLogin",
            "The spent logon failures for a banned address could not be cleared, so it may be re-banned immediately after this ban expires.");
      }
   }

   bool
   AccountLogon::BanForSpamTrap(const IPAddress &ipaddress, const String &trapAddress, int minutes)
   {
      // The range keeps the "Auto-ban: " prefix its name is built with, because
      // that prefix is what AutoBanFirewall recognises as one of ours to mirror
      // into the firewall and what the expiry sweep lifts. The name holds as much
      // of the trap as the column's 100 characters leave beside the prefix and any
      // suffix that makes it unique (RangeName).
      if (!CreateIPRange(ipaddress, _T("spam trap ") + trapAddress, minutes))
         return false;

      AutoBanFirewall::BannedForSpamTrap(ipaddress, minutes, trapAddress);

      return true;
   }

   bool
   AccountLogon::BanForNullSenderAbuse(const IPAddress &ipaddress, int hours)
   {
      if (hours < 1)
         return false;

      // Bounded by the setter's own ceiling of a year; a range's expiry is a
      // DateTimeSpan of minutes, and a year of them fits.
      const int minutes = (hours > 8760 ? 8760 : hours) * 60;

      // The same "Auto-ban: " prefix, for the same two readers: the firewall
      // reconciliation, which mirrors every range so named, and the expiry sweep,
      // which lifts them. The address is in the name so that two hosts banned
      // for the same reason do not fight over one name: a logon ban is named for
      // its user and a trap ban for its trap, and this has neither.
      if (!CreateIPRange(ipaddress, _T("multi-recipient null sender ") + String(ipaddress.ToString()), minutes))
         return false;

      AutoBanFirewall::BannedForNullSender(ipaddress, minutes);

      return true;
   }

   String
   AccountLogon::AuthOnDeliveryPortRangeName(const IPAddress &ipaddress)
   {
      // "Auto-ban: " is what the firewall reconciliation mirrors and the expiry
      // sweep lifts; the address makes the name one per address, which is what
      // lets the next AUTH find the ban in force. 34 characters and an address
      // of at most 45 fit the column's 100, so the name is the one CreateIPRange
      // writes while no range holds it (RangeName with no suffix).
      return RangeName(AuthOnDeliveryPortReason_(ipaddress), String());
   }

   String
   AccountLogon::AuthOnDeliveryPortReason_(const IPAddress &ipaddress)
   {
      return _T("AUTH on a delivery port ") + String(ipaddress.ToString());
   }

   bool
   AccountLogon::BanForAuthOnDeliveryPort(const IPAddress &ipaddress, int minutes, int port,
                                          const String &mechanism, const String &mode, bool &alreadyBanned)
   {
      alreadyBanned = false;

      if (minutes < 1)
         return false;

      // The lock CreateIPRange takes, taken around the question and the write
      // together: sessions from one address that send AUTH at the same moment -
      // a pipelined spray over parallel connections - are answered one at a time
      // here, and only the first writes a range. Recursive, so CreateIPRange
      // takes it again below.
      boost::lock_guard<boost::recursive_mutex> guard(ip_range_creation_mutex_);

      const String name = AuthOnDeliveryPortRangeName(ipaddress);

      std::shared_ptr<SecurityRange> existing = std::shared_ptr<SecurityRange>(new SecurityRange);
      if (PersistentSecurityRange::ReadObject(existing, name) && existing->GetID() > 0)
      {
         // In force: no expiry (an administrator made it permanent) or one still
         // ahead. The window is the ban's own, so one AUTH a window is one ban.
         if (!existing->GetExpires() || DateTime::GetCurrentTime() < existing->GetExpiresTime())
         {
            alreadyBanned = true;
            return true;
         }

         // Run out, and not yet lifted by RemoveExpiredRecords, which does it
         // once a minute: it bans nothing, so it is replaced rather than left to
         // give the address up to a minute in which a new AUTH writes nothing.
         // A delete that fails leaves the address unbanned, which is said where
         // an administrator looks, as a range that could not be saved is (HM6099).
         if (!PersistentSecurityRange::DeleteObject(existing))
         {
            ErrorManager::Instance()->ReportError(ErrorManager::High, 7642, "AccountLogon::BanForAuthOnDeliveryPort",
               "The expired auto-ban IP range '" + name + "' could not be deleted to be written again, so the address that sent AUTH to a delivery port has NOT been banned.");
            return false;
         }
      }

      if (!CreateIPRange(ipaddress, AuthOnDeliveryPortReason_(ipaddress), minutes))
         return false;

      AutoBanFirewall::BannedForAuthOnDeliveryPort(ipaddress, minutes, port, mechanism, mode);

      return true;
   }

   bool
   AccountLogon::CreateIPRange(const IPAddress &ipaddress, const String &reason, int minutes)
   {
      // Why do we need a lock here? IP ranges requires unique names.
      // To prevent conflicts, we need to auto-generate names. We could
      // create random names, but that would be ugly. Instead, we create
      // names containing just an index.
      //
      // The problem is if two clients are banned at the same time. There
      // is a risk that we create two IP ranges with the same name, which
      // will result in a DB error. To prevent this, we use locking to
      // ensure that only one IP range is created at a time.
      //
      // The lock is this process's own. Servers sharing one database (an
      // active-active pair) each take theirs, so the unique index on the name,
      // not the choice made under the lock, decides between them: a name taken
      // between the choice and the insert is written once more under a name
      // unique by construction (WriteRange_).

      boost::lock_guard<boost::recursive_mutex> guard(ip_range_creation_mutex_);

      DateTime dt = DateTime::GetCurrentTime();
      DateTimeSpan span;
      span.SetDateTimeSpan(0,0,minutes,0);
      dt = dt + span;


      std::shared_ptr<SecurityRange> pSecurityRange = std::shared_ptr<SecurityRange>(new SecurityRange);
      pSecurityRange->SetName(GetIPRangeName_(reason));
      pSecurityRange->SetPriority(100);
      pSecurityRange->SetLowerIP(ipaddress);
      pSecurityRange->SetUpperIP(ipaddress);
      pSecurityRange->SetExpires(true);
      pSecurityRange->SetExpiresTime(dt);

      // While the never-ban list by reverse name has rows and nothing is known
      // about the address, it waits for the list's answer from before its range
      // exists: a reconciliation that read the range between the save and the
      // caller's announcement would otherwise mirror it below the server
      // (NeverBanNameList::Hold). Every caller announces through
      // AutoBanFirewall, whose VerifyBan takes the hold over.
      NeverBanNameList::Hold(ipaddress);

      // Auto-ban is a security control, and this is the line that applies it. Its
      // result was discarded, so a failure here meant the range was never created,
      // the address was never blocked, and nothing anywhere said so - the control
      // failed open and silently, which is the combination that makes a control
      // worth nothing. There is no caller to return to and nobody to tell but the
      // administrator, so it is reported.
      if (!WriteRange_(pSecurityRange, reason))
      {
         NeverBanNameList::Release(ipaddress);

         ErrorManager::Instance()->ReportError(ErrorManager::High, 6099, "AccountLogon::CreateIPRange",
            "The auto-ban IP range '" + pSecurityRange->GetName() + "' could not be saved, so the address has NOT been blocked although it earned the ban the name gives.");
         return false;
      }

      return true;
   }

   bool
   AccountLogon::WriteRange_(std::shared_ptr<SecurityRange> range, const String &reason)
   {
      bool nameTaken = false;
      if (PersistentSecurityRange::SaveNewObject(range, nameTaken))
         return true;

      // Refused. By the unique index (nameTaken): a range of the chosen name was
      // written after it was chosen, by another server sharing the database or
      // by an administrator, and that refusal was not reported. Or by anything
      // else, which the database layer has reported. Either way it is tried once
      // more, under a name no other range can hold - the reason with a GUID, as
      // much of the reason as leaves the GUID room - before HM6099 is reported:
      // a failure that was not the name's fails again and is reported again.
      const String chosen = range->GetName();

      range->SetID(0);
      range->SetName(UniqueRangeName(reason));

      if (!PersistentSecurityRange::SaveObject(range))
         return false;

      LOG_DEBUG("AccountLogon::CreateIPRange - the IP range could not be written as '" + chosen + "'" +
                (nameTaken ? String(_T(", a name a range was given after it was chosen")) : String()) +
                ", and was written as '" + range->GetName() + "'.");
      return true;
   }

   String
   AccountLogon::GetIPRangeName_(const String &reason)
   {
      // Each candidate is asked about as the column stores it (RangeName).
      return ChooseRangeName(reason, [](const String &name) { return PersistentSecurityRange::Exists(name); });
   }

   String
   AccountLogon::RangeName(const String &reason, const String &suffix)
   {
      // As much of the reason as the column leaves beside the prefix and the
      // suffix, so that the suffix is never what the cut takes - and the reason
      // ends before a U+0000 it holds, which no engine stores (CutName), so that
      // the suffix is not lost there either. Up to 6.3.7 the whole reason went in
      // and SaveObject cut the name to 100 characters after the check for a name
      // in use had asked about it uncut. With a user name of more than 77
      // characters a suffix could be cut: from 78 to 85 the random one, whose
      // kept digits a later ban's could repeat by chance; at 86 " (10)", which
      // the twelfth ban chose again; from 87 " (1)", which the third chose
      // again; at 89 the cut left the name and a space, which SQL Server and
      // MySQL take for the name without it; and from 90 every candidate was the
      // name in use. While one range of that name stood, a later ban named for
      // the user name was refused by the unique index, and the address it was
      // earned by was not banned.
      const String prefix = AutoBanRangePrefix;
      const int room = PersistentSecurityRange::MaxNameLength - prefix.GetLength() - suffix.GetLength();

      return PersistentSecurityRange::StoredName(prefix + PersistentSecurityRange::CutName(reason, room) + suffix);
   }

   String
   AccountLogon::ChooseRangeName(const String &reason, const std::function<bool(const String &)> &taken)
   {
      String suggestion = RangeName(reason, String());
      if (!taken(suggestion))
         return suggestion;

      for (int i = 1; i <= 10; i++)
      {
         suggestion = RangeName(reason, " (" + StringParser::IntToString(i)+ ")");
         if (!taken(suggestion))
            return suggestion;
      }

      for (int i = 1; i <= 10; i++)
      {
         String pass = PasswordGenerator::Generate();
         pass = pass.Mid(0,9);
         suggestion = RangeName(reason, " (Z" + pass + ")");
         if (!taken(suggestion))
            return suggestion;
      }

      // okay. we failed finding a unique name. not very likely.
      return UniqueRangeName(reason);
   }

   String
   AccountLogon::UniqueRangeName(const String &reason)
   {
      return RangeName(reason, " " + GUIDCreator::GetGUID());
   }

   namespace
   {
      std::string Shown(const String &text)
      {
         return std::string(Unicode::ToUtf8(text).c_str());
      }

      // What did not hold, while a Collected is in scope: every check runs and the
      // tester throws once at the end naming them all, so that a run shows each
      // part that fails and not only the first.
      std::vector<std::string> *collected = nullptr;

      void Require(bool condition, const std::string &what)
      {
         if (condition)
            return;

         if (collected != nullptr)
            collected->push_back(what);
         else
            throw std::logic_error("AccountLogonRangeNameTester: " + what);
      }

      class Collected
      {
      public:
         Collected() : was_(collected) { collected = &failures_; }
         ~Collected() { collected = was_; }

         void ThrowIfAny()
         {
            collected = was_;
            if (failures_.empty())
               return;

            std::string all = "AccountLogonRangeNameTester: " + std::to_string(failures_.size()) + " check(s) did not hold: ";
            for (size_t i = 0; i < failures_.size(); i++)
               all += (i > 0 ? "; " : "") + failures_[i];
            throw std::logic_error(all);
         }

      private:
         std::vector<std::string> failures_;
         std::vector<std::string> *was_;
      };

      // c, count times.
      String Repeat(wchar_t c, int count)
      {
         String text;
         for (int i = 0; i < count; i++)
            text += c;
         return text;
      }

      // U+1F600, one character outside the Basic Multilingual Plane: a surrogate
      // pair where wchar_t is 16 bits, one unit where it is 32.
      String OutsideTheBmp()
      {
         String text;
         if (sizeof(wchar_t) == 2)
         {
            text += (wchar_t) 0xD83D;
            text += (wchar_t) 0xDE00;
         }
         else
         {
            text += (wchar_t) 0x1F600;
         }
         return text;
      }

      // A user name of length units: a local part of u and the test domain.
      String UserName(int length)
      {
         const String domain = _T("@example.test");
         return Repeat(_T('u'), length - domain.GetLength()) + domain;
      }

      void RequireStored(const String &name, const std::string &what)
      {
         Require(name.GetLength() <= PersistentSecurityRange::MaxNameLength,
                 what + " is " + Shown(StringParser::IntToString(name.GetLength())) + " units, past the column's 100: " + Shown(name));
         Require(PersistentSecurityRange::StoredName(name) == name, what + " is not the name the column stores: " + Shown(name));
         Require(!Unicode::HasUnpairedSurrogate(name), what + " holds half of a surrogate pair: " + Shown(name));
         const int nul = name.Find((wchar_t) 0);
         Require(nul < 0, what + " holds a U+0000, which no engine stores, after " + (nul < 0 ? std::string() : Shown(name.Mid(0, nul))));
         Require(name.Left(10) == _T("Auto-ban: "), what + " has lost the \"Auto-ban: \" prefix: " + Shown(name));
      }
   }

   void
   AccountLogonRangeNameTester::Test()
   {
      Collected failures;
      Names_();
      failures.ThrowIfAny();
   }

   void
   AccountLogonRangeNameTester::TestDatabase()
   {
      Collected failures;
      Database_();
      failures.ThrowIfAny();
   }

   void
   AccountLogonRangeNameTester::TestAll()
   {
      Collected failures;
      Names_();
      Database_();
      failures.ThrowIfAny();
   }

   void
   AccountLogonRangeNameTester::Names_()
   {
      const int pairUnits = sizeof(wchar_t) == 2 ? 2 : 1;

      // The cut: nothing past 100 units, a pair never split.
      Require(PersistentSecurityRange::StoredName(String()).IsEmpty(), "an empty name is not stored empty");
      Require(PersistentSecurityRange::StoredName(Repeat(_T('a'), 100)) == Repeat(_T('a'), 100), "a name of 100 units is not stored whole");
      Require(PersistentSecurityRange::StoredName(Repeat(_T('a'), 101)) == Repeat(_T('a'), 100), "a name of 101 units is not stored as its first 100");
      {
         // The character outside the BMP straddles the cut where it is a pair:
         // units 100 and 101.
         const String name = Repeat(_T('a'), 99) + OutsideTheBmp() + _T("bb");
         const String stored = PersistentSecurityRange::StoredName(name);
         Require(!Unicode::HasUnpairedSurrogate(stored), "the cut through a surrogate pair kept half of it");
         Require(stored == (pairUnits == 2 ? Repeat(_T('a'), 99) : Repeat(_T('a'), 99) + OutsideTheBmp()),
                 "the cut through a character outside the BMP did not keep the units before it, and it whole where it fits: " + Shown(stored));
      }
      {
         // A pair that ends at the 100th unit is kept.
         const String name = Repeat(_T('a'), 100 - pairUnits) + OutsideTheBmp() + _T("b");
         Require(PersistentSecurityRange::StoredName(name) == Repeat(_T('a'), 100 - pairUnits) + OutsideTheBmp(),
                 "a pair that ends at the 100th unit was not kept");
      }

      // Every candidate a ban for a user name of 86 to 91 and 200 units can be
      // written under: within the column, the prefix kept, its suffix intact
      // after the cut, and no two the same once stored. At each of these lengths
      // every kind of suffix needs room: the column's 100 hold "Auto-ban: " and
      // 51 units beside the GUID, 77 beside a random suffix, 85 beside " (10)"
      // and 86 beside " (1)", and up to 6.3.7 a suffix was cut past each.
      const String guid = _T(" {0F1E2D3C-4B5A-6978-8796-A5B4C3D2E1F0}");
      const int lengths[] = { 86, 87, 88, 89, 90, 91, 200 };
      for (int length : lengths)
      {
         const String user = UserName(length);
         const std::string what = "for a user name of " + Shown(StringParser::IntToString(length)) + " units, ";

         std::vector<String> suffixes;
         suffixes.push_back(String());
         for (int i = 1; i <= 10; i++)
            suffixes.push_back(" (" + StringParser::IntToString(i) + ")");
         suffixes.push_back(_T(" (Z012345678)"));
         suffixes.push_back(_T(" (Zabcdefghi)"));
         suffixes.push_back(guid);

         std::set<String> stored;
         for (const String &suffix : suffixes)
         {
            const String name = AccountLogon::RangeName(user, suffix);
            RequireStored(name, what + "the candidate with [" + Shown(suffix) + "]");
            Require(name.Right(suffix.GetLength()) == suffix, what + "the candidate with [" + Shown(suffix) + "] lost its suffix to the cut: " + Shown(name));
            Require(stored.insert(name).second, what + "two candidates are the same name once stored: " + Shown(name));
         }

         // The name the user name alone gives is the whole of it while it fits.
         if (length <= 90)
            Require(AccountLogon::RangeName(user, String()) == _T("Auto-ban: ") + user, what + "the name without a suffix is not the whole user name");

         // The chooser, against a store holding the name a ban for this user name
         // stands under: it never chooses that name, asks only about names as the
         // column stores them, and chooses the next suffix.
         const String standing = AccountLogon::RangeName(user, String());
         std::set<String> store;
         store.insert(standing);
         std::vector<String> asked;
         const String chosen = AccountLogon::ChooseRangeName(user, [&](const String &name)
         {
            asked.push_back(name);
            return store.count(name) > 0;
         });

         RequireStored(chosen, what + "the chosen name");
         Require(chosen != standing, what + "the name a range stands under was chosen again: " + Shown(chosen));
         Require(chosen == AccountLogon::RangeName(user, _T(" (1)")), what + "the first free suffix was not chosen: " + Shown(chosen));
         for (const String &name : asked)
            RequireStored(name, what + "a name the chooser asked about");

         // Every counted suffix taken: a random one, then a GUID.
         for (int i = 1; i <= 10; i++)
            store.insert(AccountLogon::RangeName(user, " (" + StringParser::IntToString(i) + ")"));
         const String random = AccountLogon::ChooseRangeName(user, [&](const String &name) { return store.count(name) > 0; });
         RequireStored(random, what + "the name chosen past the counted suffixes");
         Require(random.Find(_T(" (Z")) > 0 && random.Right(1) == _T(")") && store.count(random) == 0, what + "past the counted suffixes, the name is not a free one with a random suffix: " + Shown(random));

         const String last = AccountLogon::ChooseRangeName(user, [](const String &) { return true; });
         RequireStored(last, what + "the name chosen when every candidate is taken");
         Require(last.Right(1) == _T("}") && last.Find(_T(" {")) > 0, what + "when every candidate is taken, the name does not end in a GUID: " + Shown(last));

         // The name the save's retry is written under: unique by construction.
         const String first = AccountLogon::UniqueRangeName(user);
         const String second = AccountLogon::UniqueRangeName(user);
         RequireStored(first, what + "the retry's name");
         Require(first != second && first.Right(1) == _T("}"), what + "two retries' names are not two GUID names: " + Shown(first) + " and " + Shown(second));
      }

      // A user name with a character outside the BMP where the cut for a suffix
      // falls: the pair goes whole, or not at all.
      {
         const String user = Repeat(_T('a'), 85) + OutsideTheBmp() + Repeat(_T('b'), 10);
         for (int i = 1; i <= 10; i++)
         {
            const String suffix = " (" + StringParser::IntToString(i) + ")";
            const String name = AccountLogon::RangeName(user, suffix);
            RequireStored(name, "the candidate for a user name with a pair at the cut, with [" + Shown(suffix) + "]");
            Require(name.Right(suffix.GetLength()) == suffix, "the candidate for a user name with a pair at the cut lost its suffix: " + Shown(name));
         }
      }

      // A U+0000 in the reason - the web services listener's HTTP Basic decoder
      // keeps one in a user name. No engine stores it (CutName): SQL Server and
      // Compact bind a name up to it, so that every candidate would be bound as
      // the name before it and none could be written while a range of that name
      // stood, and MySQL and PostgreSQL fail the statement. The name ends before
      // it and the suffix goes after: at the start, before the cut, and in the
      // part of a long name that a suffix's room takes.
      {
         String nul;
         nul += (wchar_t) 0;
         Require(PersistentSecurityRange::CutName(_T("ab") + nul + _T("cd"), 100) == _T("ab"), "a name is not cut before a U+0000 it holds");
         Require(PersistentSecurityRange::StoredName(nul + _T("cd")).IsEmpty(), "a name that begins with a U+0000 is not stored empty");
         Require(PersistentSecurityRange::StoredName(Repeat(_T('a'), 100) + nul) == Repeat(_T('a'), 100), "a U+0000 past the column changes the name of 100 units before it");

         struct NulCase { const char *what; String user; String before; };
         const NulCase cases[] =
         {
            { "at the start", nul + _T("junk@example.test"), String() },
            { "before the cut", _T("victim@example.test") + nul + _T("junk") + Repeat(_T('x'), 100), _T("victim@example.test") },
            { "where a suffix's room cuts", Repeat(_T('v'), 88) + nul + Repeat(_T('x'), 31), Repeat(_T('v'), 88) },
         };

         for (const NulCase &nulCase : cases)
         {
            const std::string what = std::string("for a user name with a U+0000 ") + nulCase.what + ", ";

            std::vector<String> suffixes;
            suffixes.push_back(String());
            for (int i = 1; i <= 10; i++)
               suffixes.push_back(" (" + StringParser::IntToString(i) + ")");
            suffixes.push_back(_T(" (Z012345678)"));
            suffixes.push_back(guid);

            std::set<String> stored;
            for (const String &suffix : suffixes)
            {
               const String name = AccountLogon::RangeName(nulCase.user, suffix);
               RequireStored(name, what + "the candidate with [" + Shown(suffix) + "]");
               Require(name.Right(suffix.GetLength()) == suffix, what + "the candidate with [" + Shown(suffix) + "] lost its suffix: " + Shown(name));
               Require(name == _T("Auto-ban: ") + PersistentSecurityRange::CutName(nulCase.before, 100 - 10 - suffix.GetLength()) + suffix,
                       what + "the candidate with [" + Shown(suffix) + "] is not the user name before the U+0000 and the suffix: " + Shown(name));
               Require(stored.insert(name).second, what + "two candidates are the same name once stored: " + Shown(name));
            }

            // Against a store holding the name the user name before the U+0000
            // gives - what the bindings stored up to 6.3.7 - the chooser chooses
            // the next, which is what the INSERT then binds.
            std::set<String> store;
            store.insert(AccountLogon::RangeName(nulCase.before, String()));
            const String chosen = AccountLogon::ChooseRangeName(nulCase.user, [&](const String &name) { return store.count(name) > 0; });
            RequireStored(chosen, what + "the chosen name");
            Require(chosen == AccountLogon::RangeName(nulCase.before, _T(" (1)")), what + "the chooser did not choose the next suffix after the name before the U+0000: " + Shown(chosen));
         }
      }

      // The other writers' names: a spam trap of any length, the null sender's
      // and AUTH on a delivery port's for the longest address text.
      {
         const String trap = _T("spam trap ") + Repeat(_T('t'), 180) + _T("@example.test");
         RequireStored(AccountLogon::RangeName(trap, String()), "a spam trap's name");
         RequireStored(AccountLogon::RangeName(trap, _T(" (10)")), "a spam trap's name with a suffix");
         Require(AccountLogon::RangeName(trap, _T(" (10)")).Right(5) == _T(" (10)"), "a spam trap's name lost its suffix");

         IPAddress longest;
         longest.TryParse(AnsiString("ffff:ffff:ffff:ffff:ffff:ffff:255.255.255.255"), false);
         const String delivery = AccountLogon::AuthOnDeliveryPortRangeName(longest);
         RequireStored(delivery, "the name of a ban for AUTH on a delivery port");
         Require(delivery == _T("Auto-ban: AUTH on a delivery port ") + String(longest.ToString()), "the name of a ban for AUTH on a delivery port is not its whole reason: " + Shown(delivery));
         RequireStored(AccountLogon::RangeName(_T("multi-recipient null sender ") + String(longest.ToString()), _T(" (10)")), "the null sender's name");
      }
   }

   void
   AccountLogonRangeNameTester::Database_()
   {
      // Rows of the tester's own, at addresses of TEST-NET-1 that nothing else
      // uses, expiring in two minutes, and deleted again however the test ends.
      struct TesterRows
      {
         std::vector<std::shared_ptr<SecurityRange>> rows;

         ~TesterRows()
         {
            for (std::shared_ptr<SecurityRange> row : rows)
            {
               try
               {
                  if (row->GetID() > 0)
                     PersistentSecurityRange::DeleteObject(row);
               }
               catch (...)
               {
                  LOG_DEBUG("AccountLogonRangeNameTester: a range of the test's could not be deleted; it expires in two minutes.");
               }
            }
         }
      };

      TesterRows tester;

      DateTime expires = DateTime::GetCurrentTime();
      DateTimeSpan span;
      span.SetDateTimeSpan(0, 0, 2, 0);
      expires = expires + span;

      auto range = [&](const String &name, const char *address)
      {
         std::shared_ptr<SecurityRange> row(new SecurityRange);
         IPAddress ip;
         ip.TryParse(AnsiString(address), false);
         row->SetName(name);
         row->SetPriority(100);
         row->SetLowerIP(ip);
         row->SetUpperIP(ip);
         row->SetExpires(true);
         row->SetExpiresTime(expires);
         tester.rows.push_back(row);
         return row;
      };

      const String token = GUIDCreator::GetGUID();

      // A reason past the column: the row is stored under its cut name, and the
      // whole name asks about the row. Until 6.3.7 Exists asked about the whole
      // name, which no row has.
      const String longReason = _T("AccountLogonRangeNameTester ") + token + _T(" ") + Repeat(_T('x'), 120);
      std::shared_ptr<SecurityRange> stored = range(_T("Auto-ban: ") + longReason, "192.0.2.201");
      Require(PersistentSecurityRange::SaveObject(stored), "a range with a name past the column could not be saved");
      Require(stored->GetName() == AccountLogon::RangeName(longReason, String()), "the range saved is not given the name the column stores: " + Shown(stored->GetName()));
      Require(PersistentSecurityRange::Exists(_T("Auto-ban: ") + longReason), "a name past the column does not find the range stored under its cut");

      // The chooser against the database: not the stored name.
      const String chosen = AccountLogon::ChooseRangeName(longReason, [](const String &name) { return PersistentSecurityRange::Exists(name); });
      Require(chosen == AccountLogon::RangeName(longReason, _T(" (1)")), "the chooser against the database did not choose the next suffix: " + Shown(chosen));

      // A name of characters outside the Latin code pages, and one outside the
      // BMP: found by the name it was stored under.
      String foreign = _T("AccountLogonRangeNameTester ") + token + _T(" ");
      foreign += (wchar_t) 0x0418; // Cyrillic I
      foreign += (wchar_t) 0x0432; // Cyrillic ve
      foreign += (wchar_t) 0x0430; // Cyrillic a
      foreign += (wchar_t) 0x043D; // Cyrillic en
      foreign += (wchar_t) 0x7528; // CJK
      foreign += (wchar_t) 0x6237; // CJK
      foreign += OutsideTheBmp();
      std::shared_ptr<SecurityRange> foreignRow = range(AccountLogon::RangeName(foreign, String()), "192.0.2.202");
      Require(PersistentSecurityRange::SaveObject(foreignRow), "a range with a name outside the code page could not be saved");
      Require(PersistentSecurityRange::Exists(AccountLogon::RangeName(foreign, String())), "a name outside the code page does not find the range stored under it");

      // The unique index refusing a name taken between the choice and the
      // insert: told, not reported, and written again under a GUID.
      std::shared_ptr<SecurityRange> collision = range(stored->GetName(), "192.0.2.203");
      bool nameTaken = false;
      Require(!PersistentSecurityRange::SaveNewObject(collision, nameTaken) && nameTaken && collision->GetID() == 0,
              "a new range under a name a range holds was not refused as taken");

      Require(AccountLogon::WriteRange_(collision, longReason), "the write under a name taken was not written again under another");
      Require(collision->GetID() > 0 && collision->GetName() != stored->GetName() && collision->GetName().Right(1) == _T("}"),
              "the write under a name taken was not written under a GUID name: " + Shown(collision->GetName()));
      RequireStored(collision->GetName(), "the name the write was written again under");

      // An auto-ban's range in force bans its address for as long as it lasts
      // (AutoBanCovers), so that a burst's later threshold writes no second
      // range; an administrator's range does not, nor one that lets the address
      // in, nor one that ends before the ban asked about would.
      {
         IPAddress address;
         address.TryParse(AnsiString("192.0.2.204"), false);

         DateTime soon = DateTime::GetCurrentTime();
         DateTimeSpan minute;
         minute.SetDateTimeSpan(0, 0, 1, 0);
         soon = soon + minute;

         DateTime later = soon + minute + minute + minute;

         Require(!PersistentSecurityRange::AutoBanCovers(address, soon), "an address no range holds is found banned");

         std::shared_ptr<SecurityRange> ban = range(AccountLogon::RangeName(_T("AccountLogonRangeNameTester ") + token + _T(" covers"), String()), "192.0.2.204");
         Require(PersistentSecurityRange::SaveObject(ban), "an auto-ban's range to cover the address could not be saved");
         Require(PersistentSecurityRange::AutoBanCovers(address, soon), "an auto-ban's range in force for two minutes is not found to ban its address for one");
         Require(!PersistentSecurityRange::AutoBanCovers(address, later), "an auto-ban's range in force for two minutes is found to ban its address for four");

         ban->SetAllowPOP3(true);
         Require(PersistentSecurityRange::SaveObject(ban), "the auto-ban's range could not be saved letting POP3 in");
         Require(!PersistentSecurityRange::AutoBanCovers(address, soon), "an auto-ban's range that lets POP3 in is found to ban its address");

         ban->SetAllowPOP3(false);
         ban->SetName(_T("AccountLogonRangeNameTester ") + token + _T(" administrator's"));
         Require(PersistentSecurityRange::SaveObject(ban), "the range could not be saved under an administrator's name");
         Require(!PersistentSecurityRange::AutoBanCovers(address, soon), "an administrator's range is found to be an auto-ban's");
      }
   }
}
