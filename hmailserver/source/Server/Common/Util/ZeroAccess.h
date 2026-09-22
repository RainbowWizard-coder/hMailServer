// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Zero-access mailboxes (docs/ZeroAccess.md, schema 6083): a mailbox whose
// mail this server holds and cannot read.
//
// WHAT IT IS
//
// A person switches it on from the webmail's Security page, for their own
// account, once an administrator has allowed it server-wide. From then on
// every message delivered to the account is encrypted - after the filters
// have run, since spam scoring, virus scanning, the rules and the Sieve
// script all need the text - to the account's own OpenPGP key from the ring
// (OpenPgpRing.h, a KindOwn row), by OpenPgpWriter, and what is written into
// the mailbox is a PGP/MIME message: the original's headers in the clear,
// the body and every attachment under the key.
//
// THE KEY, AND WHO CAN OPEN IT
//
// The ring already holds the key's secret half encrypted under a passphrase
// this server never sees. A zero-access mailbox needs the key opened at every
// sign-in without a passphrase being typed, and it needs a second way in for
// the day the password is lost, so this table holds two more copies of the
// same secret key, both in the OpenPGP format and both made in the browser:
//
//   zasecret    sealed under a passphrase the browser derives from the
//               account password with Argon2id (RFC 9106) - the salt and the
//               three parameters are beside it - so that signing in is enough
//               to open the mail, and a password change made THROUGH THE PAGE
//               seals it again under the new one (HandleMePassword_). A
//               password an administrator resets, or a reset link sets,
//               cannot: the page never had the old one. That is what the
//               second copy is for.
//   zarecovery  sealed under the recovery key: 30 random bytes the browser
//               drew, shown once as eight groups of six base32 characters,
//               which the person wrote down and typed back before the switch
//               was thrown. It is the only other way in. The same file,
//               saved, imports into GnuPG with the recovery key as its
//               passphrase, because it is an ordinary OpenPGP secret key.
//
// This server stores both and can open neither. It checks each is an
// encrypted secret key of the fingerprint named (OpenPgpKey::ReadSecretKey),
// which is the one thing it can check.
//
// WHAT THE SERVER CAN NO LONGER DO, and refuses by name
//
//   * read the mail: the administrator's message source and export answer
//     403 (RestApiAccountResources, RestApiOperator); the row's headers are
//     answered, because the headers were kept in the clear;
//   * search it: the full-text index and IMAP SEARCH see the headers and the
//     armour, never the text (MessageIndexer says why the armour is indexed);
//   * recover it: a lost password without the recovery key is the mail lost,
//     and switching zero access off decrypts nothing already stored.
//
// IMAP and POP hand out the ciphertext, which is a valid PGP/MIME message
// any client holding the key opens. The webmail opens it in the browser, as
// it opens any other OpenPGP message, with the key it unwrapped at sign-in.
//
// WHERE THE PLAINTEXT IS, HONESTLY
//
// The row's phrase is "the plaintext never touches the disk", and the doc
// says what is true instead: the message was on disk in the clear from the
// moment it was accepted until this ran - the queue file, and the account's
// copy while the filters read it. SealFile overwrites the account's copy
// with zeros before the sealed message replaces it, which is the most a
// program can do; whether the bytes are gone is the filesystem's decision,
// and the queue file is deleted as every queue file is. A journal rule's
// copy is made at acceptance, in the clear, because the organisation's
// journal is the organisation's; the row is about the administrator reading
// the MAILBOX.
//
// A zero-access account with the switch on and no usable key is an
// inconsistency the routes make impossible; if it is met anyway (a database
// edited by hand), the delivery is REFUSED rather than made in the clear -
// the sender is told, the log says why - because a mailbox that promised
// never to hold a message in the clear must not start doing so in silence.
#pragma once

#include <vector>

namespace HM
{
   class Account;
   class Message;

   class ZeroAccess
   {
   public:
      // RFC 9106 section 4's second recommendation, which the page derives
      // with: three passes over 64 MiB in four lanes. The page may lower the
      // memory where a device cannot afford it (docs/ZeroAccess.md says what
      // was measured); these bound what a row may carry, so a row cannot
      // name parameters that make the derivation trivial.
      static const int MinimumPasses = 1;
      static const int MaximumPasses = 10;
      static const int MinimumMemoryKiB = 8 * 1024;
      static const int MaximumMemoryKiB = 1024 * 1024;
      static const int MinimumLanes = 1;
      static const int MaximumLanes = 16;

      // Octets of the salt, base64 in the row.
      static const int SaltBytes = 16;

      struct Row
      {
         Row() : id(0), account_id(0), passes(0), memory_kib(0), lanes(0), enabled_at(0) {}

         __int64 id;
         __int64 account_id;
         AnsiString fingerprint;        // the ring row's, lower-case hex
         AnsiString salt;               // base64 of SaltBytes octets
         int passes;
         int memory_kib;
         int lanes;
         AnsiString password_wrapped;   // the armoured secret key, under the password-derived passphrase
         AnsiString recovery_wrapped;   // the armoured secret key, under the recovery key
         __int64 enabled_at;
      };

      // Whether the administrator allows the opt-in at all.
      static bool Allowed();

      // The account's row, if it has one.
      static bool Get(__int64 accountId, Row &row);

      // Switches the mailbox on: writes the row and the account's flag in that
      // order, so that the flag is never on without the row. problem is a
      // sentence for the caller when false; status the HTTP status it fits.
      static bool Enable(std::shared_ptr<const Account> account, Row &row, AnsiString &problem, int &status);

      // Replaces the password-wrapped copy and the derivation parameters: a
      // password change through the page, or a recovery-key unlock followed
      // by a re-wrap under the current password.
      static bool Rewrap(std::shared_ptr<const Account> account, const Row &fresh, AnsiString &problem, int &status);

      // Switches it off: the flag first, then the row. Decrypts nothing.
      static bool Disable(std::shared_ptr<const Account> account, AnsiString &problem);

      // What a request's row must be, whether for Enable or Rewrap: the
      // parameters within bounds, the salt SaltBytes of base64, each wrapped
      // copy an encrypted OpenPGP secret key of the fingerprint. recovery may
      // be empty for a Rewrap, which does not replace it.
      static bool Validate(__int64 accountId, const Row &row, bool requireRecovery, AnsiString &problem, int &status);

      // The row as the page reads it: both wrapped copies, the parameters,
      // when it was switched on. Never a correspondent's business.
      static AnsiString RowJson(const Row &row);

      // Whether the ring's own key with this fingerprint is the one a
      // zero-access mailbox stands on, so that the ring's DELETE can refuse.
      static bool IsMailboxKey(__int64 accountId, const AnsiString &fingerprint);

      // The delivery hook. The file is the account's copy of the message,
      // filters run, about to be saved. On success the file holds the sealed
      // message and the plaintext has been overwritten before the replacement;
      // on failure the file is left as it was and problem says why, and the
      // caller refuses the delivery. A false with an empty problem never
      // happens.
      static bool SealFile(std::shared_ptr<const Account> account, const String &fileName, AnsiString &problem);

      // A copy this server files into the mailbox itself rather than
      // delivers - the Sent copy of a message sent from the webmail, or of one
      // released after a hold - sealed the same way when the account has zero
      // access on, and left alone when it has not. On failure the file is
      // scrubbed and removed, the error log carries 7071 with what and why,
      // and false says the copy is not to be kept; the send itself is not
      // affected. The copy's size is set to the sealed file's on success.
      static bool SealStoredCopy(std::shared_ptr<const Account> account, std::shared_ptr<Message> copy, const char *what);

      // Overwrites the file's bytes with zeros in place, without truncating,
      // which is the most a program can do about a plaintext that was on the
      // disk (docs/ZeroAccess.md says what the filesystem may still keep).
      // For the plaintext a failed seal is about to delete, and SealFile's
      // own original.
      static void Scrub(const String &fileName);

      // The kept headers: what SealFile leaves in the clear, in one sentence
      // for a document or a log line.
      static const char *KeptHeaders();

   private:
      static bool Delete_(__int64 accountId);
      static bool SetFlag_(std::shared_ptr<const Account> account, bool on, AnsiString &problem);
   };
}
