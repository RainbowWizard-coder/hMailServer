// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Mailbox encryption at rest (docs/EncryptionAtRest.md, schema 6099): the
// keys, where they are kept, and when they are open.
//
// THE MODEL, which is Dovecot's mail-crypt with per-user keys
//
//   * Each account an administrator switches on has an X25519 key pair, the
//     MAILBOX KEY (hm_accountkeys). Its public half is stored as it is; every
//     message stored in the account's folders from then on is encrypted to it
//     (MessageStoreFormat, stage 3), which needs no secret at all - so mail is
//     encrypted when it arrives whether anybody is signed in or not.
//
//   * Its private half is kept wrapped by the account's password (Argon2id,
//     MailboxCrypto.h), and sealed to the DOMAIN'S RECOVERY KEY
//     (hm_domainrecovery), whose own private half is wrapped by a passphrase an
//     administrator chose and this server never stores. Between the switch and
//     the account's first sign-in with its password there is a third form, the
//     PENDING copy, under the machine's own key (DataProtector: DPAPI's machine
//     key on Windows, the key file INSIDE the data directory on Linux) - which
//     this server, and whoever holds the data directory with the database,
//     can open without any password. The switch cannot wrap it by a password
//     it does not have; the first password sign-in wraps it and removes that
//     copy. The switch-on leaves the key open, as a sign-in would.
//
//   * A sign-in that carries the account's own password (AccountLogon::Logon:
//     IMAP LOGIN and AUTHENTICATE PLAIN, POP3 USER/PASS and AUTH PLAIN, SMTP
//     AUTH, ManageSieve, the DAV services, the webmail's sign-in and the REST
//     API's Basic) OPENS the key: it is unwrapped and held in memory here,
//     keyed by account. While it is open the server reads the account's mail
//     as it reads any other; an IMAP or POP3 session holds it open for as long
//     as it lasts (MailboxKeyHold), and it closes MailboxKeyIdleMinutes after
//     the owner's last use once nothing holds it - or at a restart. Only the
//     owner's own sign-ins, sessions and webmail requests count as a use: a
//     delegate's reads and a support session do not keep it open.
//
//   * An IMAP or POP3 sign-in to a keyed account that is not its own password
//     - an app password, a token, a certificate, a Kerberos ticket, another
//     account signing in as it (the IMAP master user), SCRAM with the key
//     closed - is REFUSED with a sentence that says why (SessionRefusal), and
//     so is a webmail session made by a passkey, a single sign-on or an app
//     password, and every /api/v1/me request made with an app password. SMTP,
//     ManageSieve and DAV are not refused: they do not read the mailbox.
//
//   * A second factor (TOTP) and this are never on one account: with a secret
//     enrolled the password stops being a mailbox credential where no second
//     factor is asked, and this opens only with the password - so the
//     switch-on refuses an account with a secret, and the enrolment refuses a
//     keyed account.
//
//   * Which accounts are keyed is a set in memory (KeyedState): read whole at
//     the start, changed by every write here as it is made, read again after
//     a restore. A server with no keyed account asks the database nothing for
//     it. A read that fails keeps the set it had; only while no read has ever
//     succeeded in this process is the answer unknown, and unknown is always
//     temporary: a delivery waits in the queue and is tried again, IMAP
//     answers NO [UNAVAILABLE], POP3 -ERR [SYS/TEMP], REST 503 - never a
//     bounce, a discarded message or an ended session, and never a message
//     stored in the clear on a guess. A row whose key is not a key is DAMAGED,
//     which is permanent until an administrator acts: answered at once with a
//     sentence naming the forfeit, which removes the row (DamagedSentence).
//
// HOW IT DIFFERS FROM A ZERO-ACCESS MAILBOX (docs/ZeroAccess.md)
//
//   Zero access is end to end: the person's OpenPGP key, made in the browser;
//   this server cannot read the mail, ever, and hands IMAP and POP3 the
//   ciphertext. This is at rest: the server holds the key while the person is
//   signed in, reads, searches and serves the mail as usual, and an
//   administrator with the domain's recovery passphrase can open it. What it
//   protects is the store and its copies - a disk, a snapshot, a backup
//   archive, the data directory and the database carried off - not the mail
//   from the running server. The two are never on one account together: the
//   switch-on here is refused for a zero-access mailbox, and ZeroAccess::Enable
//   refuses a keyed one.
//
// Every write of the two tables is here. The REST routes (RestApiMailboxKeys
// .cpp) are the administrator's and the account's way in; COM has none.
#pragma once

#include <memory>
#include <string>

// XNode is a class at global scope (XmlTree.h), as IniSettingStore.h says:
// declared the way it is declared there, never as a class in namespace HM.
class XNode;

namespace HM
{
   class Account;

   // An IMAP or POP3 session's hold on an account's open key: while one lives,
   // the key does not close for idleness. Made by MailboxKeys::Hold, which
   // counts it; let go of when the last copy of the pointer goes.
   class MailboxKeyHold
   {
   public:
      ~MailboxKeyHold();

      __int64 GetAccountID() const { return account_id_; }

      MailboxKeyHold(const MailboxKeyHold &) = delete;
      MailboxKeyHold &operator=(const MailboxKeyHold &) = delete;

   private:
      friend class MailboxKeys;
      explicit MailboxKeyHold(__int64 accountId);

      __int64 account_id_;
   };

   class MailboxKeys
   {
   public:

      struct Row
      {
         Row() : id(0), account_id(0), enabled(false), created(0), changed(0) {}

         __int64 id;
         __int64 account_id;
         bool enabled;                  // new mail is encrypted to the key
         std::string public_key;        // 32 raw bytes
         AnsiString password_wrap;      // MailboxCrypto's wrapped form; empty while pending
         AnsiString recovery_seal;      // sealed to the domain's recovery key
         std::string recovery_public;   // 32 raw bytes: the recovery key it is sealed to
         AnsiString pending;            // DataProtector's envelope until the first password sign-in
         __int64 created;               // seconds since 1970
         __int64 changed;
      };

      struct Recovery
      {
         Recovery() : id(0), domain_id(0), created(0) {}

         __int64 id;
         __int64 domain_id;
         std::string public_key;        // 32 raw bytes
         AnsiString sealed;             // the private half, wrapped by the passphrase
         String hint;
         __int64 created;

         // While a replacement is under way, and after one that was cut short:
         // the key it replaces, wrapped by the NEW passphrase, which accounts
         // not yet sealed to the new key are still sealed to. Empty otherwise.
         std::string previous_public;
         AnsiString previous_sealed;
      };

      // Codes, 7360 to 7369.
      enum Codes
      {
         CodeStale = 7360,          // the key did not open with a password that is the account's own
         CodeUnwrapped = 7361,      // a pending key could not be wrapped at the first sign-in
         CodeReEncrypt = 7362,      // a rewritten stored message could not be encrypted again
         CodeCopy = 7363,           // a copy between accounts could not be encrypted for its new owner
         CodeStore = 7364,          // a key row could not be written, or restored
         CodePlace = 7365,          // a message for an encrypted mailbox could not be stored encrypted, and was refused
         CodeRead = 7366,           // the accounts' keys could not be read into memory (KeyedState)
         CodeSweep = 7367           // the start's sweep of a keyed mailbox's directory failed
      };

      // ---- the rows ---------------------------------------------------------------

      static bool GetRow(__int64 accountId, Row &row);
      static bool GetRecovery(__int64 domainId, Recovery &recovery);

      // ---- which accounts are keyed ------------------------------------------------
      //
      // A set in memory of the accounts that have a key row - whether each is
      // switched on, and its public half - in the shape of DiscoveryHolds::
      // IsAccountHeld: read whole at the start (Start, from Application::
      // StartServers), changed by every write here as it is made - the
      // switch-on and off, the forfeit, an account deleted - and read whole
      // again at the next question after a restore wrote the table (Invalidate,
      // InvalidateAll). On a server with no keyed account the set is empty and
      // every question below is answered with no query at all.
      //
      // A read of the table that fails keeps the set it had, and is tried again
      // at the next question five seconds on at most. The answer is UNKNOWN
      // only while no read has ever succeeded in this process, and unknown is
      // temporary to every caller (the header above; docs/EncryptionAtRest.md,
      // "When the database cannot be read"). DAMAGED is an account whose row
      // holds no key: keyed, since files encrypted to the key it held may be in
      // its folders, but with nothing to encrypt to or open with - permanent,
      // answered at once with DamagedSentence, until the forfeit removes the row.
      enum Keyed
      {
         KeyedNo,
         KeyedYes,
         KeyedUnknown,
         KeyedDamaged
      };

      static Keyed KeyedState(__int64 accountId);

      // KeyedState, with the key's public half when the account is keyed -
      // switched on or not - from the set: CopyIntoAccount's question, whether
      // a stored file is under the destination's own key.
      static Keyed KeyOf(__int64 accountId, std::string &publicKey);

      // KeyedState != KeyedNo: whether files encrypted to a key may be in the
      // account's folders, failing closed - for the callers that only decide
      // what to keep in the clear (a deleted item's subject, the command line's
      // walks), never for one that would refuse, bounce or end something.
      static bool IsKeyed(__int64 accountId);

      // Whether the account has a key row, from the set; unknown is set, and
      // false answered, when it cannot say. MailboxMove's question.
      static bool HasKey(__int64 accountId, bool &unknown);

      // What new mail for the account is stored as: encrypted to publicKey -
      // its key is switched on and it is not a zero-access mailbox - or as it
      // comes, or not known (KeyedUnknown), which every writer answers with a
      // temporary refusal rather than a guess, or not storable at all while
      // its row holds no key (KeyedDamaged), which every writer refuses at once
      // with DamagedSentence. From the set.
      enum Encryption
      {
         EncryptionNone,
         EncryptionOn,
         EncryptionUnknown,
         EncryptionDamaged
      };

      static Encryption EncryptionFor(std::shared_ptr<const Account> account, std::string &publicKey);

      // The start: the set read, and what an encryption a crash cut short left
      // beside a keyed mailbox's files swept (MessageStore::
      // SweepEncryptionLeftovers) - before any listener or delivery runs.
      // Nothing is read for the sweep on a server with no keyed account.
      static void Start();

      // ---- the open keys ------------------------------------------------------------

      // The private key for a public one, when its account's key is open; the
      // reason when it is not, which IsLockedProblem recognises. NOT a use:
      // whoever reads - a delegate's client, a push, a support session - does
      // not keep the owner's key open; the owner's own sessions and sign-ins
      // do (Hold, Touch, OnPasswordLogon).
      static bool FindOpenKey(const std::string &publicKey, std::string &privateKey, String &problem);
      static bool IsLockedProblem(const String &problem);

      static bool IsOpen(__int64 accountId);
      static void Touch(__int64 accountId);

      // Which opening of the account's key is the current one: a number that
      // changes every time the key opens from closed, and 0 while it is closed.
      // A webmail session remembers the opening it was made under, and is good
      // only while that opening is the current one (RestApiServer): one made
      // before the key closed, or before the mailbox was encrypted at all, is
      // over, and one made a moment after it opened again is not ended for it.
      static unsigned __int64 OpenEpoch(__int64 accountId);

      // A hold on the account's key while it is open; null when it is not.
      static std::shared_ptr<MailboxKeyHold> Hold(__int64 accountId);

      // Every key nothing holds and nobody has used for MailboxKeyIdleMinutes
      // closed: its private half written over and forgotten. The minute's sweep
      // (RemoveExpiredRecords).
      static void CloseIdle();

      // An account's key opened by something that is not a sign-in: the
      // recovery route, and the command line's --decrypt-store.
      static void OpenFor(__int64 accountId, const std::string &publicKey, const std::string &privateKey);

      // ---- sign-in -------------------------------------------------------------------

      // A sign-in with the account's own password: the key opened, or wrapped
      // for the first time when it is pending, or wrapped again when it is open
      // and the password it is wrapped by is no longer the account's. Never
      // throws, never refuses anything; the session decides with SessionRefusal.
      static void OnPasswordLogon(std::shared_ptr<const Account> account, const String &password);

      // The sentence an IMAP or POP3 sign-in, a webmail session or an
      // /api/v1/me request is refused with, or empty when it is not:
      // credential is SignInStore's, or CredentialMasquerade.
      static String SessionRefusal(std::shared_ptr<const Account> account, int credential);

      // Whether a refusal of SessionRefusal, PrepareForNewPassword or
      // CanWrapNewPassword is the temporary one - whether the mailbox is
      // encrypted could not be known - which the caller answers as such:
      // IMAP [UNAVAILABLE], POP3 [SYS/TEMP], REST 503.
      static bool IsTemporaryRefusal(const String &refusal);

      // The sentence a mailbox whose key row holds no key is refused with,
      // everywhere - a sign-in, a password, a delivery, a write - naming the
      // forfeit that removes the row, and whether a refusal is it.
      static String DamagedSentence();
      static bool IsDamagedRefusal(const String &refusal);

      // The HTTP status a refusal of the three above is answered with: 503 for
      // the temporary one, 409 for a damaged row, otherwise as the route
      // answers a refusal of its own.
      static int RefusalStatus(const String &refusal, int otherwise);

      // The text of an IMAP NO for a refusal of SessionRefusal, with its RFC
      // 5530 response code: [CONTACTADMIN] for a damaged row - "the user should
      // contact the system administrator or support desk", the one code whose
      // remedy is an administrator's act, where [UNAVAILABLE] is "temporary
      // failure because a subsystem is down" and tells a client to try again -
      // and [UNAVAILABLE] for every other, the temporary one and the refusals
      // of a credential alike, as before.
      static String ImapRefusal(const String &refusal);

      // SessionRefusal's credential for another account signing in as this one
      // - the IMAP master user's AUTHENTICATE PLAIN with an authorization
      // identity - which SignInStore has no kind for.
      static const int CredentialMasquerade = 1000;

      // ---- a new password ----------------------------------------------------------

      // Before an account's password is changed: the key wrapped by the new one,
      // kept on the account object for PersistentAccount::SaveObject to store
      // once the account is saved (StorePreparedWrap). False, with the sentence,
      // when the account is keyed and its key is closed, so that nothing can
      // wrap it: the caller refuses the change - a password set then would open
      // nothing, and the recovery route is the way to set one.
      static bool PrepareForNewPassword(std::shared_ptr<Account> account, const String &password, String &refusal);
      static void StorePreparedWrap(std::shared_ptr<Account> account);

      // Whether PrepareForNewPassword would wrap the key now, asked before
      // anything is spent - the reset link, SCIM's judgement of a password.
      static bool CanWrapNewPassword(__int64 accountId, String &refusal);

      // The password wrap and the pending copy put back as they were read
      // before a change that was then undone - SCIM's put-back, which puts the
      // old password back and must put back what that password opens.
      static bool RestoreWraps(__int64 accountId, const Row &before);

      // ---- administration -------------------------------------------------------------

      // status is the HTTP status the outcome fits; problem the sentence.
      // Enable refuses an account with a second factor enrolled, a directory
      // account and a zero-access mailbox; Forfeit refuses an account under a
      // legal hold (DiscoveryHolds.h), whose mail the key is the only way to.
      static bool Enable(std::shared_ptr<const Account> account, AnsiString &problem, int &status);
      static bool Disable(std::shared_ptr<const Account> account, AnsiString &problem, int &status);
      static bool Forfeit(std::shared_ptr<const Account> account, AnsiString &problem, int &status);

      // The account's key opened with the domain's recovery passphrase, and left
      // open, so that the caller can set a password that wraps it.
      static bool OpenWithRecovery(std::shared_ptr<const Account> account, const AnsiString &passphrase, AnsiString &problem, int &status);

      // The domain's recovery key made, or replaced - which needs the current
      // passphrase, and seals every account's key of the domain to the new one
      // before the old one goes. The new row is written first, with the key it
      // replaces kept beside it wrapped by the new passphrase, then each
      // account, then the old key goes: cut short anywhere, every account still
      // opens with the new passphrase, and the next replacement finishes the
      // one before. resealed is how many. One lock in this process runs a
      // replacement, a removal, a restore of the row and a switch-on's read of
      // the recovery key to its stored key one at a time: a switch-on in the
      // middle of a replacement would seal its key to the key the replacement
      // then removes, and two replacements at once would leave accounts sealed
      // to one run's key while the row holds the other's.
      static bool SetRecovery(__int64 domainId, const AnsiString &passphrase, const String &hint, const AnsiString &currentPassphrase,
                              AnsiString &problem, int &status, int &resealed);
      static bool RemoveRecovery(__int64 domainId, AnsiString &problem, int &status);
      static int KeyedAccountsInDomain(__int64 domainId);

      // The private key of an account opened by the domain's passphrase alone,
      // for the command line's --decrypt-store, which runs without the server.
      static bool OpenPrivateWithRecovery(__int64 accountId, const AnsiString &passphrase, std::string &publicKey,
                                          std::string &privateKey, String &problem);

      // What the routes answer.
      static AnsiString AccountJson(std::shared_ptr<const Account> account, bool forTheAccount);
      static AnsiString RecoveryJson(__int64 domainId);

      // ---- the rest ---------------------------------------------------------------------

      // The domain's recovery row in a backup, beside the domain (Domain::XMLStore).
      static bool XMLStoreDomain(XNode *parentNode, __int64 domainId);
      static bool XMLLoadDomain(XNode *parentNode, __int64 domainId);

      // The set read whole again at the next question: a restore wrote the
      // table (AccountStores, Domain::XMLLoad). Until that read succeeds the
      // set answers as it did.
      static void Invalidate(__int64 accountId);
      static void InvalidateAll();

      // An account deleted - called once its row is gone, so that a delete
      // that fails leaves the set saying what the table does: its key closed,
      // and the account out of the set. And a new account, whose id may be one
      // a deleted account had: out of the set, since a new account has no key.
      static void ForgetAccount(__int64 accountId);
      static void NewAccount(__int64 accountId);

      // What an encrypted account's index holds: nothing built from its text
      // (docs/EncryptionAtRest.md, "The index"). Dropped at the switch-on.
      static void ForgetIndex(__int64 accountId);

   private:
      // Start's sweep: the keyed accounts' directories, read from the
      // database, handed to MessageStore::SweepEncryptionLeftovers.
      static void SweepKeyedDirectories_();

   public:
      // What each wrapped and sealed copy is bound to: the key's own public
      // half, so that a copy cannot be moved to another account's row.
      static std::string PasswordPurpose(const std::string &publicKey);
      static std::string RecoveryPurpose(const std::string &publicKey);
      static std::string DomainPurpose(const std::string &recoveryPublic);

      // The minimum length of a recovery passphrase, in characters.
      static const int MinimumPassphraseLength = 12;
   };
}
