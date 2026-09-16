// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Country blocking: which countries may open a connection, and which may sign
// in to a domain.
//
// WHAT IT IS
//
// One policy per listener family - SMTP, IMAP, POP3 and the HTTP listeners (the
// REST API and the web services) - and one per domain. Each is written as one
// string, because it is one decision:
//
//     off                 nothing is judged by country            (the default)
//     allow:GB,IE,FR      only these countries may connect or sign in
//     block:CN,RU         these countries may not connect or sign in
//
// The server-wide policies are applied AT ACCEPT, before the greeting, before
// the TLS handshake and before anything touches the database - the same place
// and for the same reason an IP range refuses a connection. The per-domain
// policy is applied AT AUTHENTICATION, because a domain is not known until a
// name has been given.
//
// WHAT IS EXEMPT, AND WHY EACH ONE
//
//   Everything named in CountryBlockingExemptRanges, in the grammar the
//   trusted-proxy and never-ban lists already use: a comma-separated list of
//   addresses and CIDR ranges. There is deliberately NO second, hidden list of
//   exemptions compiled into the server - what is exempt is exactly what that
//   setting says, so an administrator can read it, and so a test can take it
//   away.
//
//   What it says when nothing has been changed is the loopback address and the
//   private networks (DefaultExemptRanges below, which the schema step seeds).
//   The loopback address is there because the webmail, the Control Panel, hmctl
//   on the machine itself and every local script come from it, and blocking it
//   could only ever lock the administrator out of their own server. The private
//   ranges are there because no geographic database has a country for them -
//   RFC 1918 space is not allocated to anybody - so a policy that judged them
//   would judge every office machine as "unknown", which an allow list refuses.
//   An administrator who empties the setting has said that their own network is
//   to be judged like anybody else's, which is a thing they may mean.
//
//   An open session. Nothing here is consulted again once a connection has been
//   accepted, so a policy change, a database reload or a database that has gone
//   missing never drops a session that is already running, and never interrupts
//   one that has authenticated.
//
// FAILING OPEN, DELIBERATELY
//
// With no database configured, or one that could not be read, EVERYTHING IS
// ALLOWED and the reason is written to the application log once. A mail server
// that refuses every connection because a file was renamed is a worse failure
// than one that stops applying a geographic policy, and the administrator whose
// database has gone missing is the person who most needs to be able to sign in
// and fix it.
//
// With a database loaded, an address the database has no country for is
// refused by an ALLOW list (that is what an allow list means) and allowed by a
// BLOCK list (it is not in the list). That is the only place where the two
// kinds of policy differ in what they do about the unknown, and it is stated in
// the setting's own description so that nobody has to infer it.
//
// WHAT A REFUSED CLIENT IS TOLD
//
// Nothing. The connection is closed, exactly as a connection refused by an IP
// range is closed - no banner, no code, no text. A client that learns "you were
// refused because you are in Chile" has learned the policy, and the next thing
// it does is come back through somewhere else. The administrator's copy is in
// the application log, the counters and the reports.

#pragma once

#include <memory>
#include <set>
#include <string>

namespace HM
{
   class GeoIpDatabase;
   class IPAddress;

   class CountryBlocking : public Singleton<CountryBlocking>
   {
   public:
      CountryBlocking();
      virtual ~CountryBlocking();

      enum Mode
      {
         ModeOff = 0,
         ModeAllow = 1,
         ModeBlock = 2
      };

      // The listener families a server-wide policy can be written for. The
      // names are what the COM parameter, the REST keys and the documents use.
      enum Surface
      {
         SurfaceSmtp = 0,
         SurfaceImap = 1,
         SurfacePop3 = 2,
         SurfaceHttp = 3
      };

      struct Policy
      {
         Policy() : mode(ModeOff) { }

         Mode mode;
         std::set<AnsiString> countries;   // upper-case ISO 3166-1 alpha-2
      };

      // The longest a policy may be written out, which is what the settings row
      // and the domain column hold. 255 characters is a block list of eighty
      // countries; an administrator who wants more than that wants an allow
      // list, and is told so.
      static const int MaxPolicyLength = 255;

      // Reads "off", "" , "allow:GB,IE" or "block:CN,RU". False with a sentence
      // in 'error' for anything else - which is what every setter refuses with,
      // so the Control Panel, the Deck, hmctl and COM all say the same thing.
      static bool ParsePolicy(const String &text, Policy &policy, String &error);

      // The policy written back out in its canonical form: lower-case keyword,
      // upper-case codes, sorted, no spaces. What a setter stores, so that two
      // spellings of one policy are one stored value.
      static String FormatPolicy(const Policy &policy);

      // Parse and format in one step, for a setter. Empty result for "off".
      static bool Canonicalise(const String &text, String &canonical, String &error);

      // The name of a surface, as a policy setting is keyed by it.
      static String SurfaceName(Surface surface);
      static bool SurfaceFromName(const String &name, Surface &surface);

      // The country of an address as the loaded database answers it: an
      // upper-case two-letter code, or empty when there is no database or the
      // database has nothing for that address. Never throws.
      AnsiString CountryOf(const IPAddress &address);

      // The English name of the country, for the log line and the status.
      // Empty when unknown.
      AnsiString CountryNameOf(const IPAddress &address);

      // The connection decision, applied at accept. True when the connection may
      // proceed - which it always may when the policy is off, the address is
      // exempt or no database is loaded. When it refuses, it has already written
      // the application-log line and counted the refusal, because every caller
      // would otherwise have to remember to.
      bool IsConnectionAllowed(Surface surface, const IPAddress &address);

      // The sign-in decision, applied wherever a name and a password are
      // checked. username is the address being signed in as; the policy is the
      // one on its domain. Counts and logs its own refusals, as above.
      bool IsLogonAllowed(const String &username, const IPAddress &address);

      // Whether the address is one no policy judges, by the configured list.
      bool IsExempt(const IPAddress &address);

      // The same against a list given here rather than read from the settings:
      // the seam the tests drive, and the only place the matching happens.
      static bool IsExemptBy(const String &exemptRanges, const IPAddress &address);

      // What CountryBlockingExemptRanges holds until an administrator changes
      // it, seeded by the schema step that creates the row. Named here so that
      // the server, the documents and the SQL cannot drift apart.
      static const wchar_t *DefaultExemptRanges();

      // An IPv4 client of a dual-stack listener arrives as ::ffff:a.b.c.d. Every
      // question here is asked of the address it really is.
      static IPAddress Unmapped(const IPAddress &address);

      // The state of the database, as JSON: what is configured, whether it
      // loaded, what it is, when it was built, whether that is stale, and the
      // refusals counted since the server started, by country. What COM's
      // Utilities.GeoIpDatabaseStatus, the REST settings group and the Control
      // Deck all show.
      AnsiString StatusJson();

      // The same in one sentence, for a settings page and a log line.
      String StatusText();

      // Forget the loaded database; the next question loads it again. Called
      // when the file or the staleness setting is written.
      void Invalidate();

      // The policy in force for a surface, read from the settings each time so
      // that a change applies to the next connection.
      Policy PolicyFor(Surface surface);

      // The decision itself, with no database, no logging and no counting: does
      // this policy admit this country, where an empty country means the
      // database had no answer. Public because it is the whole meaning of a
      // policy and the tests drive it directly.
      static bool Allows(const Policy &policy, const AnsiString &country);

   private:
      // Hands back the database to ask, or an empty pointer when there is none.
      // Loads or reloads it if it is time to; never holds the lock across a
      // lookup.
      std::shared_ptr<const GeoIpDatabase> Database_();

      // One refusal: the log line and the counters. surfaceName is "SMTP",
      // "IMAP", "POP3", "HTTP" or "sign-in".
      void RecordRefusal_(const String &surfaceName, const IPAddress &address, const AnsiString &country, const String &detail);

      boost::recursive_mutex mutex_;

      std::shared_ptr<GeoIpDatabase> database_;
      String configured_path_;
      String load_error_;
      unsigned __int64 file_size_;
      __int64 file_written_;
      time_t next_recheck_;
      bool reported_missing_;
   };

   // The in-process tests of the policy grammar and of the decision it makes.
   // The database is a separate unit with its own tests (GeoIpDatabaseTester);
   // this is the half that says what a policy MEANS, which is where the
   // allow-list-and-unknown-country question lives.
   class CountryBlockingTester
   {
   public:
      void Test();
   };
}
