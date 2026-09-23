// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The four session policies, resolved into one answer.
//
// A session here is a SIGNED-IN BROWSER: the self-service portal, the webmail
// and the Control Deck, which all hold a cookie rather than a password (see
// RestApiServer::AuthenticateSession_). A mail client's IMAP, POP3 or SMTP
// connection is not a session in this sense - it re-authenticates on every
// connection and each protocol has its own idle timeout - so these policies
// govern the cookie, and revoking a device is what ends a mail connection.
//
// Four things an administrator can say:
//
//   idle       how long a session may go unused before it ends
//   lifetime   how long a session may live at all, however busy it is
//   one per device
//              a second sign-in from a device ends that device's earlier
//              session, so a person who signs in on a shared machine and
//              forgets leaves nothing behind when they sign in again
//   bind to address
//              the session is refused from any address but the one it began
//              on, so a stolen cookie is useless away from the victim's
//              network. It also ends a session for somebody whose address
//              changes underneath them - a phone moving between wi-fi and a
//              mobile network - which is why it is off unless asked for.
//
// WHERE THEY LIVE, and what 0 means. Each is a row of hm_settings
// (SessionIdleSeconds, SessionLifetimeSeconds, SessionOnePerDevice,
// SessionBindToAddress) and a column of hm_domains, and every one of the eight
// is 0 after the 6050 upgrade. 0 does NOT mean "no limit": it means THE POLICY
// IS NOT SET, and what applies is what applied before there were policies -
// thirty minutes idle, twelve hours in all, neither switch on. That is what
// makes the upgrade safe: nobody is locked out of anything, and nobody is
// given a session that outlives the one they had.
//
// A domain's value wins over the server's where it is set, and an account's
// own value - schema 6088's overlay, docs/SettingLevels.md - over its domain's.
// The four are settings with levels (session_idle_seconds, session_lifetime_seconds,
// session_one_per_device, session_bind_to_address), resolved by
// Application/EffectiveSetting; the domain of a session is the domain of the
// account that signed in, and the administrator's own session belongs to no
// domain and is governed by the server-wide values alone.
//
// The seconds are seconds and not minutes on purpose: a value under a minute
// is no use to an administrator but is exactly what a regression test needs to
// prove that an idle session is refused without waiting out a minute of the
// suite's time. The documented advice, and the Deck's own wording, is that
// anything under sixty seconds is for testing.

#pragma once

namespace HM
{
   class Domain;
   class Account;

   struct SessionPolicy
   {
      SessionPolicy()
         : idle_seconds(0), lifetime_seconds(0), one_per_device(false), bind_to_address(false),
           server_idle_seconds(0), server_lifetime_seconds(0), server_one_per_device(false), server_bind_to_address(false),
           domain_idle_seconds(0), domain_lifetime_seconds(0), domain_one_per_device(0), domain_bind_to_address(0),
           has_domain(false), has_account(false)
      {
      }

      // What is in force. Always a positive number of seconds: the resolution
      // below falls back to the built-in ceiling rather than to "for ever".
      int idle_seconds;
      int lifetime_seconds;
      bool one_per_device;
      bool bind_to_address;

      // What was stored, so that an interface can show an administrator which
      // of the three levels an answer came from rather than only the answer.
      int server_idle_seconds;
      int server_lifetime_seconds;
      bool server_one_per_device;
      bool server_bind_to_address;

      // The domain's own row: 0 is "inherit" for the two numbers, and for the
      // two switches 0 is inherit, 1 is on and 2 is off. A switch needs three
      // states because a domain must be able to say "not here" about a server
      // -wide policy as well as "here too".
      int domain_idle_seconds;
      int domain_lifetime_seconds;
      int domain_one_per_device;
      int domain_bind_to_address;

      bool has_domain;
      String domain_name;

      // The account's own values, as the overlay holds them: empty is "take its
      // domain's". Only for a policy asked for an account.
      bool has_account;
      String account_address;
      String account_idle_seconds;
      String account_lifetime_seconds;
      String account_one_per_device;
      String account_bind_to_address;
   };

   class SessionPolicies
   {
   public:
      // The ceilings that have always applied, and still do wherever nothing
      // has been set. Thirty minutes and twelve hours.
      static const int DefaultIdleSeconds = 1800;
      static const int DefaultLifetimeSeconds = 43200;

      // Thirty days. A number larger than this is refused at every interface
      // rather than stored: a session that may live for a year is not a
      // session policy, it is the absence of one.
      static const int MaximumSeconds = 2592000;

      // The domain switches, as stored.
      enum Switch
      {
         SwitchInherit = 0,
         SwitchOn = 1,
         SwitchOff = 2
      };

      static SessionPolicy ServerWide();

      // An empty name answers ServerWide(); a name this server does not host
      // answers ServerWide() as well, because there is no domain row to read.
      static SessionPolicy ForDomain(const String &domainName);

      // The policy for an account, by its address: the account's own values,
      // then its domain's, then the server's. An address that is no account
      // answers its domain's policy, as it always did.
      static SessionPolicy ForAddress(const String &address);

      // The same for an account at hand; ForAddress's answer for its address.
      static SessionPolicy ForAccount(std::shared_ptr<const Account> account);

      // The refusal sentence for a value that is not acceptable, or an empty
      // string. Shared by REST, COM and the command line so that all three
      // refuse the same numbers with the same words.
      static String CheckSeconds(int seconds);
      static String CheckSwitch(int value);

      // The policy as a JSON object: what is in force, and what each of the
      // three levels holds. Used by COM (Utilities.GetSessionPolicy) and by
      // the REST reads, so that the two documents are one piece of code.
      static AnsiString ToJson(const SessionPolicy &policy);

   private:
      static void ReadServer_(SessionPolicy &policy);
      static void ApplyDomain_(SessionPolicy &policy, std::shared_ptr<const Domain> domain);
      static void ApplyAccount_(SessionPolicy &policy, std::shared_ptr<const Account> account);
      static void Resolve_(SessionPolicy &policy, std::shared_ptr<const Domain> domain, std::shared_ptr<const Account> account);
   };
}
