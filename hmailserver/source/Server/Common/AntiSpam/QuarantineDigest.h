// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The quarantine digest: one message to an account that has something newly
// held for it, listing what was held, with a link per entry that releases it and
// one that releases the lot.
//
// The quarantine has had a page in the webmail since it was written, and a page
// is only seen by somebody who goes and looks. Nobody goes and looks at a page
// for messages they do not know exist - which is why the false positive that
// matters is always found weeks later, by the sender asking why they were
// ignored. The digest is the half of the feature that reaches out.
//
// FIVE THINGS THIS FILE IS CAREFUL ABOUT
//
// 1. A link in an e-mail is a capability. Anybody who reads the message, or the
//    log of a mail server it passed through, can follow it. So a release link
//    here is 32 random bytes recorded in a table against ONE account and ONE
//    entry, usable ONCE, and dead after QuarantineDigestLinkHours. It is not an
//    id and it is not a signature over an id: there is nothing to guess, nothing
//    to forge and nothing to replay. Redeeming one that names another account's
//    entry answers exactly as one that names nothing does.
//
// 2. A digest sent to every account at the same moment is a mail storm this
//    server aims at itself. The task spreads a domain's sends across its digest
//    hour; see QuarantineDigestTask.
//
// 3. An empty digest is never sent. "Nothing was held for you" is not news, and
//    a daily message saying it is how a feature teaches people to filter it out.
//
// 4. Nothing in the message is remote and nothing in it is a link to anywhere
//    but this server. It is text/plain, which makes that true by construction
//    rather than by review: there is no image to load, no stylesheet to fetch
//    and no markup to get wrong, and a reader who is being phished by something
//    that looks like this can tell, because the real one never asks for a
//    password.
//
// 5. The footer says how to stop it, and the link that stops it is a token of
//    the same kind. A person who cannot turn a thing off does not consider it a
//    service.

#pragma once

#include <vector>

#include "../Util/VariantDateTime.h"

namespace HM
{
   class Account;
   class Domain;
   struct QuarantinedMessage;

   class QuarantineDigest
   {
   public:
      enum TokenKind
      {
         // One quarantined entry, released to this account.
         KindRelease = 0,

         // Everything the digest listed: every entry still held for this account
         // whose id is at or below the one recorded on the token, so a message
         // quarantined after the digest was written is never released by a link
         // the reader followed without seeing it.
         KindReleaseAll = 1,

         // Stop sending digests to this account.
         KindOptOut = 2
      };

      struct Token
      {
         __int64 id = 0;
         __int64 account_id = 0;
         int kind = KindRelease;
         __int64 quarantine_id = 0;
         bool used = false;
         bool expired = false;
      };

      struct State
      {
         bool exists = false;
         bool opted_out = false;

         // The highest quarantine entry id the last digest listed. Everything
         // above it is what "new since last time" means, and it is why a digest
         // never repeats an entry the reader has already been shown and ignored.
         __int64 last_id = 0;

         // When that digest went out, for the once-a-day rule. Invalid when this
         // account has never had one.
         DateTime last_sent;
      };

      // ---- settings -------------------------------------------------------

      // The absolute base URL the links are written against, without a trailing
      // slash - "https://mail.example.com". Empty when the administrator has not
      // set one, and then no digest is sent at all.
      static String BaseUrl();

      // How long a link lives. Clamped to 1..720 hours; the stored 0 of a
      // database that has never been told means the shipped 72.
      static int LinkHours();

      // ---- the account's own state ----------------------------------------

      static bool GetState(__int64 accountId, State &out_state);
      static bool SetOptOut(__int64 accountId, bool optOut);
      static bool RecordSent(__int64 accountId, __int64 highestId);

      // ---- tokens ---------------------------------------------------------

      // A new one-shot capability. Returns the token text, or empty when it
      // could not be made - no entropy, or the row could not be written, both of
      // which must stop the digest rather than produce a message whose links do
      // nothing.
      static AnsiString IssueToken(__int64 accountId, int kind, __int64 quarantineId);

      static bool FindToken(const AnsiString &token, Token &out_token);
      static bool MarkUsed(__int64 tokenId);

      // Removes tokens that are used or expired, and sender-list entries whose
      // expiry has passed. Housekeeping: both are already ignored by the code
      // that reads them, so a failure here costs nothing but rows.
      static int Sweep();

      // ---- redeeming ------------------------------------------------------

      struct Outcome
      {
         bool ok = false;

         // What to tell the person, in the page's own words. Never names
         // anything that belongs to somebody else.
         String heading;
         String detail;

         int http_status = 200;
         int released = 0;

         // The kind of token, for the page that asks: its button says Deliver or
         // Stop.
         int kind = KindRelease;
      };

      // What a token would do, without doing it and without spending it: the
      // page a link opens says "deliver this message from X?" and waits for a
      // click, and this is what it says. ok is false, with the same one answer
      // Redeem gives, for every token that names nothing this person may act on.
      //
      // WHY THE LINK DOES NOT ACT ON ITS OWN. Mail security products follow
      // every link in a message before the reader sees it - to scan the page
      // behind it - and a GET that released mail would have one of those
      // releasing everything in every digest, spam included, into every inbox.
      // So the link opens a page, and the page's button is the release.
      static Outcome Inspect(const AnsiString &token);

      // Redeems a token: releases the entry, releases what the digest listed, or
      // stops the digest. A token that is unknown, used, expired, or whose
      // account or entry has gone answers with an Outcome that says so and
      // changes nothing.
      static Outcome Redeem(const AnsiString &token);

      // ---- building and sending -------------------------------------------

      // The entries held for this account with an id above `sinceId`, newest
      // first. Empty when there is nothing new, which is when no digest is sent.
      static std::vector<QuarantinedMessage> Held(const String &address, __int64 sinceId);

      // Builds and queues one account's digest. False when nothing was sent -
      // no base URL, the account opted out, nothing new held, or a token could
      // not be issued. out_highestId is the highest entry id the digest listed,
      // which is what RecordSent stores.
      static bool SendFor(std::shared_ptr<const Account> account, std::shared_ptr<const Domain> domain,
                          const std::vector<QuarantinedMessage> &held, __int64 &out_highestId);

      // The message itself, as the bytes that go into the spool. Separated from
      // the sending so that a test can read what a reader would read without a
      // delivery pass, and so that "no remote image, no external link" is a
      // property of a string a test can assert about.
      static AnsiString BuildMessage(const String &toAddress, const String &fromAddress,
                                     const std::vector<QuarantinedMessage> &held,
                                     const std::vector<AnsiString> &releaseTokens,
                                     const AnsiString &releaseAllToken,
                                     const AnsiString &optOutToken,
                                     const String &baseUrl);

      // The postmaster of the domain, or postmaster@<domain> when it has none.
      // The digest comes from the domain rather than from a global address so
      // that a reply, which nobody should send, at least reaches somebody who
      // could answer it.
      static String SenderFor(std::shared_ptr<const Domain> domain);

   private:
      static bool ReadToken_(const String &where, const String &value, Token &out_token);
   };
}
