// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

namespace HM
{
   class Account;

   class AccountLogon
   {
   public:
      AccountLogon(void);
      ~AccountLogon(void);

      // matchedAppPassword, when given, says whether the password that logged
      // the account on was one of its app passwords (see PasswordValidator).
      std::shared_ptr<const Account> Logon(const IPAddress &ipaddress, const String &sUsername, const String &sPassword, bool &disconnect,
                                           bool *matchedAppPassword = nullptr, String *matchedAppPasswordName = nullptr);
      std::shared_ptr<const Account> Logon(const IPAddress &ipaddress, const String &sMasqname, const String &sUsername, const String &sPassword, bool &disconnect,
                                           bool *matchedAppPassword = nullptr, String *matchedAppPasswordName = nullptr);

      // Record a failed authentication attempt for auto-ban accounting. Sets
      // 'disconnect' to true when the connection should be dropped (and possibly the
      // IP auto-banned). Used both by Logon and by mechanisms such as SCRAM that
      // verify the proof themselves without ever holding a clear-text password.
      //
      // countTowardsAccountLockout: pass false when the failure is not a password
      // guess. An OAuth2 bearer token that was invalid or expired is the case that
      // matters - a client looping on a stale token would otherwise lock its own
      // user out of every password-based client for the lockout duration, and a
      // token is not guessable by brute force, so counting it protects nothing.
      // The REST API passes false too: its Basic credential is the administrator
      // password from the ini, not a mailbox name, so a per-name lock could only
      // ever lock a string nobody authenticates as. Such failures still feed the
      // per-IP auto-ban, which is the control that fits them.
      void RegisterFailedLogin(const IPAddress &ipaddress, const String &username, bool &disconnect,
                               bool countTowardsAccountLockout = true);

      // The compromised-account detection's hold (CompromiseDetector.h) for
      // the logons that prove identity without a password - a bearer token, a
      // Kerberos ticket, a client certificate - which never pass through Logon
      // above and so never reach the lockout check it makes first. True, with
      // the account dropped, when the name is held: the caller answers as it
      // answers any failed logon, and charges the refusal to nothing - not the
      // per-name lockout, which has nothing to count, and not the per-IP
      // auto-ban, for the reason Logon gives: the client most likely to keep
      // presenting a valid token for a locked account is its owner's, on a NAT
      // shared with everybody else. Counted as a failed authentication in the
      // server status, as Logon's refusal is. Never throws.
      static bool RefusedByHold(std::shared_ptr<const Account> &account);

      // The logon tarpit: how long a connection's next refusal should wait, given
      // how many logons it has failed so far (this one included). Progressive - the
      // n-th failure waits n times LogonTarpitSeconds - and capped, so a session
      // that keeps guessing is slowed by more each time without ever outlasting
      // the protocol's idle timeout. 0 when the tarpit is off, which it is unless
      // hMailServer.ini says otherwise. Every protocol queues the result with
      // TCPConnection::EnqueueDelay; none of them sleeps.
      static int TarpitDelaySeconds(int failuresOnThisConnection);

      // A spam trap was addressed: the same expiring IP range a run of failed
      // logons creates, under a name that says which trap it was, and the same
      // notification below the server. Returns whether the range was saved,
      // which is the only case in which the address is really banned.
      //
      // Whether this address MAY be banned at all is the caller's decision and
      // is made before this is reached: an authenticated session, a trusted IP
      // range, the white list, an incoming relay and AutoBanNeverBan never
      // arrive here. See SpamTrapMonitor::OnHit.
      bool BanForSpamTrap(const IPAddress &ipaddress, const String &trapAddress, int minutes);

      // A null sender named more than one recipient (discussion #283): the same
      // expiring range again, under a name that says why, for the hours
      // BanMultiRcptNullSenderHours gives. Whether the address MAY be banned is
      // decided by the caller as it is for a trap: SMTPConnection reaches this
      // only for an unauthenticated session of a range that runs the spam
      // tests, never for the anti-spam white list, an incoming relay or an
      // address on AutoBanNeverBan.
      bool BanForNullSenderAbuse(const IPAddress &ipaddress, int hours);

   private:

      // True if the range was saved, which is the only case in which the address
      // is actually banned and the firewall may be told.
      bool CreateIPRange(const IPAddress &ipaddress, const String &username, int minutes);

      String GetIPRangeName_(const String &username);

      static boost::recursive_mutex ip_range_creation_mutex_;
   };
}