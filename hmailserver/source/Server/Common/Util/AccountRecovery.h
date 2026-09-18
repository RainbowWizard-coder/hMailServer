// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// A recovery address and a set of one-time recovery codes, so that a forgotten
// password or a lost authenticator is something a person does for themselves
// rather than a ticket somebody else has to act on. Until this existed only an
// administrator could reset either.
//
// THREE TABLES (schema 6076)
//
//   hm_accountrecovery   one row per account: the address, whether it has been
//                        proved, and when it was set and last changed. The
//                        account's own choice, and in the backup with it
//                        (AccountStores).
//   hm_recoverytokens    the one-shot links - prove this address, reset this
//                        password - each 32 random bytes stored as a SHA-256
//                        hex digest, good once, dead at its expiry. Not in the
//                        backup, for the reason hm_digesttokens is not: a
//                        restored capability is one nobody issued.
//   hm_recoverycodes     ten codes, hashed with the same scheme the account
//                        password uses, each usable once. In the backup: they
//                        are the account's credentials, and a restore that
//                        dropped them would leave a printed card that silently
//                        stops working.
//
// WHAT IS CAREFUL HERE, AND WHY
//
// 1. A link in an e-mail is a capability, so it is random, one-shot and short
//    lived, and only ever the digest of it is stored. Nothing is an id and
//    nothing is a signature over an id: there is nothing to guess and nothing
//    to forge. The reset link lives RecoveryResetLinkMinutes (30 by default)
//    and the verification link RecoveryVerifyLinkHours (24).
//
// 2. The forgotten-password route never says whether an address exists, whether
//    it has a recovery address, or whether that address was proved. Every answer
//    is the same sentence and the same status, for a mailbox that is not there
//    as much as for one that is, because a refusal that differs is an address
//    directory for anybody who asks.
//
// 3. A code is never shown twice. Generate() answers the ten in the clear once,
//    stores their hashes, and nothing here can ever read one back - not an
//    administrator, not a database dump, not this class.
//
// 4. The recovery address is exactly as strong as the mailbox that receives it,
//    and AccountRecovery.md says so in those words. This is a convenience for
//    the person who has forgotten a password, not a second factor.
//
// 5. Nothing is sent to an address that has not been proved. A reset link goes
//    to the verified recovery address and nowhere else - never to the mailbox
//    that is locked out, which is the mailbox somebody who cannot get in cannot
//    read.
#pragma once

#include <vector>

namespace HM
{
   class Account;
   class IPAddress;

   class AccountRecovery
   {
   public:
      // How many codes a set holds. Ten, which is what every provider that has
      // this feature issues: enough to keep some in a wallet and some in a safe.
      static const int CodesPerAccount = 10;

      // ---- settings -------------------------------------------------------

      // Whether accounts may set a recovery address and use it at all.
      static bool Enabled();

      // How long a password-reset link lives, in minutes (5 to 1440; 30 by
      // default), and how long a verification link lives, in hours (1 to 168).
      static int ResetLinkMinutes();
      static int VerifyLinkHours();

      // The absolute base URL the links are written against, without a trailing
      // slash. The confidential message's, which falls back to the quarantine
      // digest's and the import sign-in's. Empty when no address is configured,
      // and then no link can be written and no reset is offered.
      static String BaseUrl();

      // ---- the address ----------------------------------------------------

      struct Address
      {
         bool exists = false;
         String address;
         bool verified = false;
         __int64 created = 0;
         __int64 updated = 0;
      };

      static bool Get(__int64 accountId, Address &address);

      // The address with all but the first letter of the local part and of each
      // domain label replaced by dots: "a...@e......e.com". What the account's
      // own page shows back, and the whole of what an administrator is shown -
      // enough to recognise the address they set, never enough to read one they
      // did not.
      static String Mask(const String &address);

      // Sets, or changes, the recovery address. The row is always unverified
      // afterwards - a change re-proves - and a verification message is sent to
      // the new address. False with a sentence in problem.
      //
      // retryAfter, when given, comes back as the seconds to wait when the
      // refusal was the per-address budget below rather than anything about the
      // address itself; the caller answers 429 then.
      static bool Set(std::shared_ptr<const Account> account, const String &address, String &problem,
                      int *retryAfter = nullptr);

      // The verification message again, for the address already stored.
      //
      // Charged against the SAME per-address budget a password-reset link is
      // charged against - one a minute and six an hour for the address the
      // message would go to. A verification message is the same capability as a
      // reset link, the server posting mail to an address somebody named, and a
      // route behind a credential is no reason to leave that unmetered: an
      // account can name any address in the world.
      static bool SendVerification(std::shared_ptr<const Account> account, const Address &stored, String &problem,
                                   int *retryAfter = nullptr);

      // Removes the address, its tokens and nothing else. True when a row went.
      static bool Clear(__int64 accountId);

      // Removes every recovery code the account has. Separate from Clear because
      // the two answer different reports: "that address is not mine any more" is
      // Clear, and "somebody else has my code card" is this. A set of ten codes
      // stands in for the second factor, so an administrator who could not end
      // one had no remedy for a leaked card at all - the account holder's own
      // replacement from the webmail was the only way, and somebody locked out
      // of the webmail cannot reach it.
      static bool ClearCodes(__int64 accountId);

      // ---- the one-shot links ---------------------------------------------

      enum TokenKind
      {
         KindVerifyAddress = 1,
         KindPasswordReset = 2
      };

      struct Token
      {
         __int64 id = 0;
         __int64 account_id = 0;
         int kind = 0;
         String address;
         __int64 expires = 0;
         __int64 used = 0;
         __int64 created = 0;
      };

      // The row a token names, whatever state it is in. False for a token that
      // is not 64 hex digits, or that names nothing.
      static bool FindToken(const AnsiString &token, Token &found);

      // Proves the recovery address a verification token names. The token is
      // spent whether or not the row could be written, so a link works once
      // however it ends.
      static bool VerifyAddress(const AnsiString &token, String &address, String &problem);

      // ---- the forgotten password -----------------------------------------

      // What a request for a reset link did. The caller answers the same
      // sentence for every one of them: only the log and the counters differ.
      enum ResetOutcome
      {
         ResetSent,          // a link is on its way to a verified recovery address
         ResetNothingToDo,   // no such account, no recovery address, or it is not proved
         ResetTooSoon,       // one went to this address within the last minute
         ResetTooMany,       // this address, or this client, has asked too often
         ResetUnavailable,   // the feature is off, or no base URL is configured
         ResetFailed         // the token or the message could not be made
      };

      static ResetOutcome BeginReset(const String &mailbox, const IPAddress &peer);

      // Sets the new password a reset link's holder chose, through the same
      // policy, reuse history and breach check as the webmail's own password
      // form; ends every browser session of the account; and tells the mailbox
      // and the recovery address that it happened. problem carries the sentence
      // to show and status the HTTP status that fits it.
      enum CompleteOutcome
      {
         CompleteDone,
         CompleteBadToken,
         CompleteRefused,
         CompleteFailed
      };

      static CompleteOutcome CompleteReset(const AnsiString &token, const String &newPassword,
                                           String &problem, int &status, String &mailbox);

      // ---- the codes ------------------------------------------------------

      struct CodeState
      {
         int total = 0;
         int unused = 0;
         __int64 created = 0;   // when the set was made; 0 when there is none
      };

      static bool GetCodes(__int64 accountId, CodeState &state);

      // A fresh set of ten, which invalidates whatever the account had. The
      // clear text is answered here and is never stored, logged or shown again.
      static bool Generate(__int64 accountId, std::vector<String> &codes);

      // The row id of an unused code that matches, or 0. Deliberately does NOT
      // spend it: the caller spends it only once whatever else it was checking
      // - the account's own password - has been accepted, so a wrong password
      // cannot burn a code.
      static __int64 CheckCode(__int64 accountId, const String &code);
      static bool SpendCode(__int64 id);

      // CheckCode and SpendCode in one, for a caller that has already proved
      // the password. True when a code was matched and spent; remaining is how
      // many are left afterwards.
      static bool ConsumeCode(__int64 accountId, const String &code, int &remaining);

      // ---- housekeeping ---------------------------------------------------

      // The one-shot links that have run out. Rows whose account is gone are the
      // foreign key's ON DELETE CASCADE, not this. True when the statement ran.
      static bool Sweep();

      // "Your password was changed", to the mailbox and to the recovery address.
      // Best effort: a notice that could not be queued never fails the change it
      // describes.
      static void SendPasswordChanged(std::shared_ptr<const Account> account, const String &recoveryAddress,
                                      const String &how);
   };
}
