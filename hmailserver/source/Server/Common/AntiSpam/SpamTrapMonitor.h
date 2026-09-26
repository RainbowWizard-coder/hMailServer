// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "../Util/VariantDateTime.h"

#include <vector>

namespace HM
{
   class Domain;
   class IPAddress;

   // Spam traps: addresses in a hosted domain that were never real.
   //
   // WHAT A TRAP IS FOR
   //
   // Nobody is ever given a trap address, so nobody has a reason to write to it.
   // A message addressed to one is therefore not a message that went astray: the
   // sender is working from a list they harvested, bought or guessed, and this is
   // the one piece of evidence a mail server gets that needs no scoring and has
   // no false positive to weigh - the address does not exist and never did.
   //
   // WHAT HAPPENS AT RCPT TO
   //
   // The session is refused exactly as it would be for any address this server
   // does not have ("550 Unknown user"), because the one thing a trap must not do
   // is announce itself: a sender who can tell a trap from an unknown address
   // removes it from their list and the trap stops working. Then, and only for a
   // session that could have been refused anyway:
   //
   //   * the connecting address is banned through the server's own auto-ban - the
   //     same expiring IP range a run of failed logons creates, so it is lifted by
   //     the same sweep, shown on the same page and mirrored into the firewall by
   //     the same reconciliation. SpamTrapBanMinutes decides for how long, and 0
   //     switches the ban off;
   //   * the envelope sender is added to the server's block list (hm_senderlist at
   //     server scope) with an expiry of SpamTrapBlockDays, so that mail from the
   //     same sender to a real mailbox is held for review where the quarantine is
   //     on and marked where it is not. 0 switches that off;
   //   * the hit is recorded in hm_spamtraphits and counted into
   //     hmailserver_spam_trap_hits_total.
   //
   // WHO IS NEVER BANNED, AND WHY EACH
   //
   //   * an authenticated session - that is one of our own people, and a trap in
   //     their address book is a mistake, not an attack;
   //   * an address whose IP range does not run the spam tests - the operator has
   //     already said that range is trusted, and a trap must not overrule that;
   //   * an address or sender the anti-spam white list covers, for the same reason;
   //   * an incoming relay - banning the machine that fronts this server would
   //     refuse everybody's mail, and the sender is still blocked;
   //   * an address on AutoBanNeverBan, which is the list that exists precisely so
   //     that a ban cannot lock an administrator out of the machine.
   //
   // The sender is not blocked for an authenticated session, for the null sender
   // (a bounce, which no list may swallow), for a sender in a domain this server
   // hosts (which is a forgery, and blocking it would block the real user), or
   // where the server's own allow list already names it - an administrator's
   // decision outranks an automatic one.
   //
   // TRAINING THE SCORE, HONESTLY
   //
   // A trap hit is refused at RCPT TO, so no content ever arrives: there is
   // nothing to teach a content classifier from the hit itself, and this server
   // has no classifier of its own to teach. What it has is spamd's Bayes store
   // (SpamAssassinLearner, the spamc TELL command). So when a session that
   // addressed a trap goes on to send a message to a real mailbox in the same
   // transaction, THAT message - which is a message the same sender sent while
   // demonstrably working from a harvested list - is taught to spamd as spam, if
   // SpamTrapTeachSpamAssassin is on and SpamAssassin is configured. A trap hit
   // with no message after it teaches nothing, and the documentation says so
   // rather than implying that a score is trained by every hit.
   //
   // The lesson is given only for a session the hit was allowed to punish
   // (Outcome::teachable). Everybody spared the ban above is spared this too,
   // and for a stronger reason: a ban expires by itself and a poisoned Bayes
   // store does not.
   class SpamTrapMonitor
   {
   public:

      // ---------------------------------------------------------------- the list

      // Every save and delete of a trap, and every domain save (a rename changes
      // what the addresses are), tells the cache to read the table again.
      static void SetNeedRefresh();

      // Whether this RCPT TO address is a trap, after domain aliases and plus
      // addressing. False for an address that is also an account, an alias or a
      // distribution list: whatever the table says, a real mailbox is a real
      // mailbox, and refusing mail to one is the worse failure of the two.
      static bool Match(const String &recipient, String &trapAddress, __int64 &domainId);

      // Whether an address somebody is about to give to an account, an alias or a
      // distribution list is a trap of its domain. Read from the table rather than
      // the cache: this answers an administrator, once, and must not depend on
      // when the cache last refreshed.
      static bool IsTrapAddress(const String &address);

      struct Listed
      {
         Listed() : id(0) { }

         __int64 id;
         String address;
         String local_part;
         DateTime created;
      };

      // This domain's traps. False when the table could not be read.
      static bool List(std::shared_ptr<const Domain> domain, std::vector<Listed> &traps);

      enum AddResult
      {
         AddCreated = 0,
         AddAlreadyATrap = 1,

         // The address cannot be a trap of this domain; refusal says why.
         AddRefused = 2,

         // An account, an alias or a distribution list already has it.
         AddTaken = 3,

         AddFailed = 4
      };

      // Declares address a trap of the domain. address may be a whole address or
      // just the part before the @, which is taken to be in this domain.
      static AddResult Add(std::shared_ptr<const Domain> domain, const String &address,
                           String &canonicalAddress, String &refusal);

      // Removes the trap. found says whether there was one; the return value is
      // false only when the delete itself failed.
      static bool Remove(std::shared_ptr<const Domain> domain, const String &address,
                         String &canonicalAddress, bool &found);

      // ----------------------------------------------------------------- a hit

      struct Outcome
      {
         Outcome() : banned(false), blocked(false), teachable(false) { }

         bool banned;
         bool blocked;

         // Whether a message this session goes on to send may be taught to
         // spamd as spam. False for every session the hit deliberately did not
         // punish - an authenticated one, a range that does not run the spam
         // tests, a white-listed sender, an address on AutoBanNeverBan - because
         // a lesson is a punishment too, and a heavier one than a ban: a ban
         // expires, and a poisoned Bayes store does not. One of our own people
         // who mistypes a trap address and then sends a real message would
         // otherwise teach spamd that their own mail is spam.
         //
         // An incoming relay is the one exception that does NOT stop it: the
         // relay is the machine that carried the message, not the party that
         // wrote it, and the message still came from somebody working from a
         // harvested list.
         bool teachable;
      };

      // Everything a hit does, decided here so that the SMTP path holds no policy:
      // the ban, the block, the record, the log line and the counter.
      //
      // The envelope sender is blocked server-wide only when SPF passes for it
      // from the connecting address (heloHost is the session's HELO, which SPF
      // needs only for the null sender, never blocked anyway). The envelope is
      // whatever the client typed, and trap addresses are published: without the
      // check, one unauthenticated connection put any correspondent it named on
      // the block list for every mailbox for SpamTrapBlockDays.
      static Outcome OnHit(const String &trapAddress, __int64 domainId, const IPAddress &address,
                           const String &envelopeSender, const String &heloHost, bool authenticated, bool rangeRunsSpamTests,
                           bool whiteListed, bool incomingRelay, bool bannedEarlierInSession);

      // The message a session that addressed a trap went on to send, taught to
      // spamd as spam. Copies the file first, because the message is delivered and
      // removed on another thread while the lesson is still being given.
      static void TeachSpamAssassin(const String &messageFile);

      // -------------------------------------------------------------- the record

      // The newest hits as a JSON document: {"items":[...]} for one domain, with
      // hits_since_start as well for the whole server.
      static AnsiString HitsDocument(__int64 domainId, int maxRows, bool &readable);

      // How many were removed, or -1 when the table could not be written.
      static int ClearHits(__int64 domainId);

      // Hits older than SpamTrapHitRetentionDays, removed. Called hourly.
      static void SweepHits();

   private:

      static bool Refresh_();
      static bool BlockSender_(const String &sender, const String &trapAddress,
                               const String &connectingAddress, int days, String &note);
   };
}
