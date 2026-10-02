// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

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
#include "VariantDateTime.h"
#include "Time.h"
#include "GUIDCreator.h"
#include "PasswordGenerator.h"
#include "ServerStatus.h"


#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   boost::recursive_mutex AccountLogon::ip_range_creation_mutex_;

   AccountLogon::AccountLogon(void)
   {
   }

   AccountLogon::~AccountLogon(void)
   {
   }

   std::shared_ptr<const Account>
   AccountLogon::Logon(const IPAddress &ipaddress, const String &username, const String &password, bool &disconnect,
                       bool *matchedAppPassword, String *matchedAppPasswordName, SecondFactorOffer *secondFactor)
   {
      return Logon(ipaddress, _T(""), username, password, disconnect, matchedAppPassword, matchedAppPasswordName, secondFactor);
   }

   std::shared_ptr<const Account>
   AccountLogon::Logon(const IPAddress &ipaddress, const String &masqname, const String &username, const String &password, bool &disconnect,
                       bool *matchedAppPassword, String *matchedAppPasswordName, SecondFactorOffer *secondFactor)
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

      // An authorisation identity that is not the name that authenticated (the
      // IMAP master user, RFC 4616): the checks above judged the account that
      // proved the password, and the session is the other one's. Its country
      // policy and its compromise hold are asked too, with its own address, so
      // that a master user in a domain with no policy cannot open a mailbox
      // from a country the mailbox's domain or the mailbox itself refuses, nor
      // one held for review. Until 26 September 2026 neither was asked. The
      // password was right, so the refusal is charged to nothing, as a country
      // refusal or a hold is for the account itself.
      if (account && !masqname.IsEmpty() && masqname.CompareNoCase(username) != 0)
      {
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
         if (!viaAppPassword && (masqname.IsEmpty() || masqname.CompareNoCase(username) == 0))
            MailboxKeys::OnPasswordLogon(account, password);

         return account;
      }

      ServerStatus::Instance()->OnAuthenticationFailed();

      RegisterFailedLogin(ipaddress, username, disconnect);

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

   void
   AccountLogon::RegisterFailedLogin(const IPAddress &ipaddress, const String &username, bool &disconnect,
                                     bool countTowardsAccountLockout)
   {
      disconnect = false;

      // The per-name half, independent of the per-IP auto-ban below and of its
      // enablement: each closes a hole the other cannot see - one address
      // spraying many names, and many addresses converging on one name. See the
      // header for the failures that deliberately do not count.
      if (countTowardsAccountLockout)
         AccountLockout::Instance()->RecordFailure(username);

      // is auto banning enabled?
      int maxInvalidLogonAttempts = Configuration::Instance()->GetMaxInvalidLogonAttempts();

      if (Configuration::Instance()->GetAutoBanLogonEnabled() == false || maxInvalidLogonAttempts == 0)
         return;

      // An address on AutoBanNeverBan is never counted, let alone banned. The
      // per-connection disconnect after repeated failures still applies - that is
      // the protocol handler's, not this - so a stuck client from a listed address
      // still gets dropped; it just never reaches the firewall.
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
         // Unchecked. Failing here leaves this address's spent failures in the table
         // after the ban has been created, so when the ban expires the next single
         // failure re-crosses the threshold and bans it again. That errs towards
         // blocking rather than allowing, which is why it is reported rather than
         // treated as fatal - but an address that cannot get back in after its ban
         // expires is a support call, and this is the only place that would explain it.
         if (!logonFailure.ClearFailuresByIP(ipaddress))
         {
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6113, "AccountLogon::RegisterFailedLogin",
               "The spent logon failures for a banned address could not be cleared, so it may be re-banned immediately after this ban expires.");
         }

         int minutes = Configuration::Instance()->GetAutoBanMinutes();
         if (minutes == 0)
         {
            // disconnect, but don't block.
            disconnect = true;
            return;
         }

         if (CreateIPRange(ipaddress, username, minutes))
            AutoBanFirewall::Banned(ipaddress, minutes, username, failureCount);

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

   bool
   AccountLogon::BanForSpamTrap(const IPAddress &ipaddress, const String &trapAddress, int minutes)
   {
      // The range keeps the "Auto-ban: " prefix its name is built with, because
      // that prefix is what AutoBanFirewall recognises as one of ours to mirror
      // into the firewall and what the expiry sweep lifts. The trap is cut to 60
      // characters: the name column holds 100 and GetIPRangeName_ may add a
      // suffix to make the name unique.
      const String suffix = trapAddress.GetLength() > 60 ? trapAddress.Left(60) : trapAddress;

      if (!CreateIPRange(ipaddress, _T("spam trap ") + suffix, minutes))
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
      // of at most 45 fit the column's 100.
      return _T("Auto-ban: ") + AuthOnDeliveryPortReason_(ipaddress);
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
   AccountLogon::CreateIPRange(const IPAddress &ipaddress, const String &username, int minutes)
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

      boost::lock_guard<boost::recursive_mutex> guard(ip_range_creation_mutex_);

      DateTime dt = DateTime::GetCurrentTime();         
      DateTimeSpan span;
      span.SetDateTimeSpan(0,0,minutes,0);
      dt = dt + span;


      std::shared_ptr<SecurityRange> pSecurityRange = std::shared_ptr<SecurityRange>(new SecurityRange);
      pSecurityRange->SetName(GetIPRangeName_(username));
      pSecurityRange->SetPriority(100);
      pSecurityRange->SetLowerIP(ipaddress);
      pSecurityRange->SetUpperIP(ipaddress);
      pSecurityRange->SetExpires(true);
      pSecurityRange->SetExpiresTime(dt);

      // Auto-ban is a security control, and this is the line that applies it. Its
      // result was discarded, so a failure here meant the range was never created,
      // the address was never blocked, and nothing anywhere said so - the control
      // failed open and silently, which is the combination that makes a control
      // worth nothing. There is no caller to return to and nobody to tell but the
      // administrator, so it is reported.
      if (!PersistentSecurityRange::SaveObject(pSecurityRange))
      {
         ErrorManager::Instance()->ReportError(ErrorManager::High, 6099, "AccountLogon::CreateIPRange",
            "The auto-ban IP range '" + pSecurityRange->GetName() + "' could not be saved, so the address has NOT been blocked although it earned the ban the name gives.");
         return false;
      }

      return true;
   }      

   String 
   AccountLogon::GetIPRangeName_(const String &username)
   {
      String suggestion = "Auto-ban: " + username;
      if (!PersistentSecurityRange::Exists(suggestion))
         return suggestion;

      for (int i = 1; i <= 10; i++)
      {
         suggestion = "Auto-ban: " + username + " (" + StringParser::IntToString(i)+ ")";
         if (!PersistentSecurityRange::Exists(suggestion))
            return suggestion;
      }

      for (int i = 1; i <= 10; i++)
      {
         String pass = PasswordGenerator::Generate();
         pass = pass.Mid(0,9);
         suggestion = "Auto-ban: " + username + " (Z" + pass + ")";
         if (!PersistentSecurityRange::Exists(suggestion))
            return suggestion;
      }

      // okay. we failed finding a unique password. not very likely.
      suggestion = "Auto-ban: " + username + " " + GUIDCreator::GetGUID() + "";
      return suggestion;
   }

}