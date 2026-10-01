// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "MailboxKeys.h"
#include "MailboxCrypto.h"
#include "DataProtector.h"
#include "DiscoveryHolds.h"
#include "FileUtilities.h"
#include "PasswordValidator.h"
#include "SignInStore.h"
#include "StorePath.h"
#include "Unicode.h"
#include "XmlTree.h"
#include "Parsing/StringParser.h"
#include "../Application/Application.h"
#include "../Application/Configuration.h"
#include "../Application/ErrorManager.h"
#include "../Application/IniFileSettings.h"
#include "../Application/Logger.h"
#include "../BO/Account.h"
#include "../Persistence/MessageStore.h"
#include "../SQL/SQLCommand.h"
#include "../SQL/SQLStatement.h"
#include "../SQL/DALRecordset.h"

#include <openssl/sha.h>

#include <ctime>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      const char *Columns = "akid, akaccountid, akenabled, akpublic, akpassword, akrecovery, akrecoverypublic, akpending, akcreated, akchanged";
      const char *RecoveryColumns = "drid, drdomainid, drpublic, drsealed, drhint, drcreated, drprevpublic, drprevsealed";

      // The words a read of a message whose key is closed is refused with -
      // not damage, and not reported as damage (MessageStore::ReportUnreadable).
      const wchar_t *LockedWords = L"its mailbox is encrypted at rest and its key is not open";

      __int64 Now()
      {
         return (__int64) time(nullptr);
      }

      AnsiString Utf8(const String &value)
      {
         AnsiString out;
         Unicode::WideToMultiByte(value, out);
         return out;
      }

      std::string Bytes(const AnsiString &text)
      {
         return std::string(text.c_str(), (size_t) text.GetLength());
      }

      // A column of base64 as its raw bytes; empty when it is not base64.
      std::string Raw(const String &column)
      {
         std::string bytes;
         if (!MailboxCrypto::FromBase64(Bytes(AnsiString(column)), bytes))
            return std::string();
         return bytes;
      }

      String Base64Column(const std::string &raw)
      {
         return String(AnsiString(MailboxCrypto::Base64(raw).c_str()));
      }

      AnsiString EscapeJson(const AnsiString &value)
      {
         AnsiString out;
         for (int i = 0; i < value.GetLength(); i++)
         {
            unsigned char c = (unsigned char) value.c_str()[i];
            if (c == '"') out += "\\\"";
            else if (c == '\\') out += "\\\\";
            else if (c == '\n') out += "\\n";
            else if (c == '\r') out += "\\r";
            else if (c == '\t') out += "\\t";
            else if (c < 0x20) { char buffer[8]; sprintf_s(buffer, sizeof(buffer), "\\u%04x", c); out += buffer; }
            else out += (char) c;
         }
         return out;
      }

      void ReadRow(std::shared_ptr<DALRecordset> recordset, MailboxKeys::Row &row)
      {
         row.id = recordset->GetInt64Value("akid");
         row.account_id = recordset->GetInt64Value("akaccountid");
         row.enabled = recordset->GetLongValue("akenabled") != 0;
         row.public_key = Raw(recordset->GetStringValue("akpublic"));
         row.password_wrap = AnsiString(recordset->GetStringValue("akpassword"));
         row.recovery_seal = AnsiString(recordset->GetStringValue("akrecovery"));
         row.recovery_public = Raw(recordset->GetStringValue("akrecoverypublic"));
         row.pending = AnsiString(recordset->GetStringValue("akpending"));
         row.created = recordset->GetInt64Value("akcreated");
         row.changed = recordset->GetInt64Value("akchanged");
      }

      // The row, whether the query itself failed (as against finding none),
      // and whether the row it found holds no key (damaged).
      bool ReadRowFor(__int64 accountId, MailboxKeys::Row &row, bool &failed, bool &damaged)
      {
         failed = false;
         damaged = false;
         row = MailboxKeys::Row();

         SQLCommand command(String(_T("select ")) + String(Columns) + _T(" from hm_accountkeys where akaccountid = @ACCOUNTID"));
         command.AddParameter("@ACCOUNTID", accountId);

         std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
         if (!recordset)
         {
            failed = true;
            return false;
         }

         if (recordset->IsEOF())
            return false;

         ReadRow(recordset, row);

         // A row whose key is not a key is damage, not the absence of a key.
         if (row.public_key.size() != (size_t) MailboxCrypto::KeyBytes)
         {
            damaged = true;
            return false;
         }

         return true;
      }

      // The same, damage answered as a read that failed: for the callers that
      // have asked the set about damage before they read.
      bool ReadRowFor(__int64 accountId, MailboxKeys::Row &row, bool &failed)
      {
         bool damaged = false;
         const bool found = ReadRowFor(accountId, row, failed, damaged);
         if (damaged)
            failed = true;
         return found;
      }

      // ---- which accounts are keyed (MailboxKeys.h) ------------------------------------

      struct KeyedEntry
      {
         KeyedEntry() : enabled(false), damaged(false) {}
         bool enabled;               // new mail is encrypted to the key
         bool damaged;               // a row whose public half is not a key
         std::string public_key;     // 32 raw bytes
      };

      struct KeyedStatus
      {
         KeyedStatus() : loaded(false), stale(true), generation(0), tried_at(0), reported_at(0) {}

         bool loaded;                  // a read of the whole table has succeeded in this process
         bool stale;                   // it is read again at the next question
         unsigned __int64 generation;  // moved on by every change made here
         __int64 tried_at;
         __int64 reported_at;
      };

      // A failed read of the table is tried again this long after at most,
      // while a question wants it; the error log hears of it once a minute.
      const __int64 RetrySeconds = 5;
      const __int64 ReportEverySeconds = 60;

      std::mutex &KeyedLock()
      {
         static std::mutex lock;
         return lock;
      }

      std::map<__int64, KeyedEntry> &KeyedSet()
      {
         static std::map<__int64, KeyedEntry> set;
         return set;
      }

      KeyedStatus &KeyedRead()
      {
         static KeyedStatus status;
         return status;
      }

      // The whole table read into the set; false when it could not be, and the
      // set it had stands. A change made here while it was read is newer than
      // the read, which is then not kept: the next question reads again.
      bool ReadKeyed()
      {
         unsigned __int64 generation = 0;
         {
            std::lock_guard<std::mutex> guard(KeyedLock());
            generation = KeyedRead().generation;
            KeyedRead().tried_at = Now();
         }

         SQLCommand command("select akaccountid, akenabled, akpublic from hm_accountkeys");
         std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);

         std::map<__int64, KeyedEntry> read;
         if (recordset)
         {
            while (!recordset->IsEOF())
            {
               KeyedEntry entry;
               entry.enabled = recordset->GetLongValue("akenabled") != 0;
               entry.public_key = Raw(recordset->GetStringValue("akpublic"));
               entry.damaged = entry.public_key.size() != (size_t) MailboxCrypto::KeyBytes;
               read[recordset->GetInt64Value("akaccountid")] = entry;
               recordset->MoveNext();
            }
         }

         bool report = false;
         bool neverRead = false;
         {
            std::lock_guard<std::mutex> guard(KeyedLock());
            if (recordset)
            {
               if (KeyedRead().generation != generation)
               {
                  KeyedRead().tried_at = 0;
                  return false;
               }

               KeyedSet().swap(read);
               KeyedRead().loaded = true;
               KeyedRead().stale = false;
               return true;
            }

            KeyedRead().stale = true;
            const __int64 now = Now();
            if (now < KeyedRead().reported_at || now - KeyedRead().reported_at >= ReportEverySeconds)
            {
               KeyedRead().reported_at = now;
               report = true;
               neverRead = !KeyedRead().loaded;
            }
         }

         if (report)
         {
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, MailboxKeys::CodeRead, "MailboxKeys::ReadKeyed",
               neverRead
                  ? "The accounts' mailbox keys (hm_accountkeys) could not be read, and have not been read since the server started, so whether any mailbox is encrypted at rest is not known: until they can be read, local deliveries wait in the queue and are tried again, and IMAP, POP3 and the webmail refuse sign-ins for now with a sentence to try again. Nothing is stored in the clear on a guess, and nothing is bounced."
                  : "The accounts' mailbox keys (hm_accountkeys) could not be read again; the server answers from what it read last until they can be, and nothing about any mailbox waits on them meanwhile.");
         }

         return false;
      }

      // A change this process made, put into the set as it is made - after its
      // row is written - and newer than any read of the table under way.
      void RememberKeyed(__int64 accountId, bool enabled, const std::string &publicKey)
      {
         std::lock_guard<std::mutex> guard(KeyedLock());
         KeyedEntry &entry = KeyedSet()[accountId];
         entry.enabled = enabled;
         entry.public_key = publicKey;
         entry.damaged = publicKey.size() != (size_t) MailboxCrypto::KeyBytes;
         KeyedRead().generation++;
      }

      void ForgetKeyed(__int64 accountId)
      {
         std::lock_guard<std::mutex> guard(KeyedLock());
         KeyedSet().erase(accountId);
         KeyedRead().generation++;
      }

      MailboxKeys::Keyed KeyedEntryOf(__int64 accountId, KeyedEntry &entry)
      {
         entry = KeyedEntry();
         if (accountId <= 0)
            return MailboxKeys::KeyedNo;

         // One question reads, and the others meanwhile answer from the set as
         // it is: the attempt is marked before the read is made.
         bool read = false;
         {
            std::lock_guard<std::mutex> guard(KeyedLock());
            const __int64 now = Now();
            read = KeyedRead().stale && (now < KeyedRead().tried_at || now - KeyedRead().tried_at >= RetrySeconds);
            if (read)
               KeyedRead().tried_at = now;
         }

         if (read)
            ReadKeyed();

         std::lock_guard<std::mutex> guard(KeyedLock());
         if (!KeyedRead().loaded)
            return MailboxKeys::KeyedUnknown;

         // The common case - no keyed account on the server - asks nothing more.
         if (KeyedSet().empty())
            return MailboxKeys::KeyedNo;

         std::map<__int64, KeyedEntry>::const_iterator found = KeyedSet().find(accountId);
         if (found == KeyedSet().end())
            return MailboxKeys::KeyedNo;

         entry = found->second;
         return entry.damaged ? MailboxKeys::KeyedDamaged : MailboxKeys::KeyedYes;
      }

      // One replacement of a recovery key at a time, and no switch-on between
      // its read of the recovery key and its stored key (MailboxKeys.h,
      // SetRecovery): per process, across domains - a replacement is rare, and
      // one lock is one thing to reason about.
      std::mutex &RecoveryLock()
      {
         static std::mutex lock;
         return lock;
      }

      // ---- the open keys -------------------------------------------------------------

      struct OpenKey
      {
         OpenKey() : holds(0), last_used(0), epoch(0) {}
         std::string public_key;
         std::string private_key;

         // A digest of the password that last opened the key or was found to
         // wrap it, under a salt this process drew: a sign-in with the same
         // password finds it here and spends no Argon2id on the wrap.
         std::string password_digest;

         int holds;
         __int64 last_used;

         // Which opening this is (MailboxKeys::OpenEpoch): drawn when the key
         // opens from closed, and kept while it stays open.
         unsigned __int64 epoch;
      };

      // The last opening drawn, under OpenLock.
      unsigned __int64 &LastEpoch()
      {
         static unsigned __int64 epoch = 0;
         return epoch;
      }

      std::mutex &OpenLock()
      {
         static std::mutex lock;
         return lock;
      }

      std::map<__int64, OpenKey> &OpenKeys()
      {
         static std::map<__int64, OpenKey> keys;
         return keys;
      }

      std::string PasswordDigest(const AnsiString &secret)
      {
         static std::once_flag once;
         static std::string salt;
         std::call_once(once, [] { salt = MailboxCrypto::RandomBytes(32); });

         std::string input = salt + Bytes(secret);
         unsigned char digest[SHA256_DIGEST_LENGTH] = {};
         SHA256((const unsigned char *) input.data(), input.size(), digest);
         MailboxCrypto::Forget(input);
         return std::string((const char *) digest, sizeof(digest));
      }

      void OpenInMemory(__int64 accountId, const std::string &publicKey, const std::string &privateKey, const std::string &digest)
      {
         std::lock_guard<std::mutex> guard(OpenLock());
         const bool opening = OpenKeys().find(accountId) == OpenKeys().end();
         OpenKey &entry = OpenKeys()[accountId];
         if (entry.public_key != publicKey)
         {
            MailboxCrypto::Forget(entry.private_key);
            entry.password_digest.clear();
         }

         if (opening || entry.public_key != publicKey)
            entry.epoch = ++LastEpoch();

         entry.public_key = publicKey;
         entry.private_key = privateKey;
         if (!digest.empty())
            entry.password_digest = digest;
         entry.last_used = Now();
      }

      // The open private key of the account, when it is the key the row names.
      // A use only when use says so: the owner's own sign-in is one, and an
      // administrator's password set, which only wraps the key, is not.
      bool OpenPrivateOf(__int64 accountId, const std::string &publicKey, std::string &privateKey, std::string *digest, bool use)
      {
         std::lock_guard<std::mutex> guard(OpenLock());
         std::map<__int64, OpenKey>::iterator found = OpenKeys().find(accountId);
         if (found == OpenKeys().end() || found->second.public_key != publicKey)
            return false;

         if (use)
            found->second.last_used = Now();
         privateKey = found->second.private_key;
         if (digest)
            *digest = found->second.password_digest;
         return true;
      }

      void SetDigest(__int64 accountId, const std::string &digest)
      {
         std::lock_guard<std::mutex> guard(OpenLock());
         std::map<__int64, OpenKey>::iterator found = OpenKeys().find(accountId);
         if (found != OpenKeys().end())
            found->second.password_digest = digest;
      }

      void CloseNow(__int64 accountId)
      {
         {
            std::lock_guard<std::mutex> guard(OpenLock());
            std::map<__int64, OpenKey>::iterator found = OpenKeys().find(accountId);
            if (found == OpenKeys().end())
               return;

            MailboxCrypto::Forget(found->second.private_key);
            OpenKeys().erase(found);
         }

         // What was decoded with it a moment ago goes with it.
         MessageStore::ForgetDecodes();
      }

      // ---- writes ----------------------------------------------------------------------

      bool StoreWrap(__int64 accountId, const std::string &wrap)
      {
         SQLCommand command("update hm_accountkeys set akpassword = @PASSWORD, akpending = @PENDING, akchanged = @CHANGED where akaccountid = @ACCOUNTID");
         command.AddParameter("@PASSWORD", String(AnsiString(wrap.c_str())));
         command.AddParameter("@PENDING", String());
         command.AddParameter("@CHANGED", Now());
         command.AddParameter("@ACCOUNTID", accountId);

         // The set holds nothing of the wraps: nothing in it changes.
         const bool stored = Application::Instance()->GetDBManager()->Execute(command);

         if (!stored)
         {
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, MailboxKeys::CodeStore, "MailboxKeys::StoreWrap",
               "The mailbox key of an account could not be stored wrapped by its password. The key stays as it was: under the earlier password, or held for the first sign-in.");
         }

         return stored;
      }

      // A pending key: the machine's envelope opened, and checked to be the key
      // the row names.
      bool OpenPending(const MailboxKeys::Row &row, std::string &privateKey)
      {
         privateKey.clear();
         if (row.pending.IsEmpty())
            return false;

         AnsiString text;
         if (!DataProtector::Unprotect(row.pending, text))
            return false;

         std::string encoded = Bytes(text);
         std::string raw;
         const bool ok = MailboxCrypto::FromBase64(encoded, raw) && MailboxCrypto::PublicKeyOf(raw) == row.public_key;
         MailboxCrypto::Forget(encoded);

         if (!ok)
         {
            MailboxCrypto::Forget(raw);
            return false;
         }

         privateKey.swap(raw);
         return true;
      }

      // Said once an hour an account at most: the log is not a place to repeat
      // a sentence at every sign-in of a phone.
      bool FirstReportThisHour(__int64 accountId)
      {
         static std::mutex lock;
         static std::map<__int64, __int64> reported;

         std::lock_guard<std::mutex> guard(lock);
         const __int64 now = Now();
         std::map<__int64, __int64>::iterator found = reported.find(accountId);
         if (found != reported.end() && now >= found->second && now - found->second < 3600)
            return false;

         reported[accountId] = now;
         return true;
      }

      void ReadRecovery(std::shared_ptr<DALRecordset> recordset, MailboxKeys::Recovery &recovery)
      {
         recovery.id = recordset->GetInt64Value("drid");
         recovery.domain_id = recordset->GetInt64Value("drdomainid");
         recovery.public_key = Raw(recordset->GetStringValue("drpublic"));
         recovery.sealed = AnsiString(recordset->GetStringValue("drsealed"));
         recovery.hint = recordset->GetStringValue("drhint");
         recovery.created = recordset->GetInt64Value("drcreated");
         recovery.previous_public = Raw(recordset->GetStringValue("drprevpublic"));
         recovery.previous_sealed = AnsiString(recordset->GetStringValue("drprevsealed"));
         if (recovery.previous_public.size() != (size_t) MailboxCrypto::KeyBytes)
         {
            recovery.previous_public.clear();
            recovery.previous_sealed = AnsiString();
         }
      }

      // The recovery key an account's recovery copy is sealed to, as this
      // domain holds it: the current one, or the one a replacement under way
      // (or cut short) keeps beside it. False when it is neither.
      bool RecoveryFor(const MailboxKeys::Row &row, const MailboxKeys::Recovery &recovery, MailboxKeys::Recovery &opener)
      {
         opener = recovery;
         if (row.recovery_public == recovery.public_key)
            return true;

         if (!recovery.previous_public.empty() && row.recovery_public == recovery.previous_public)
         {
            opener.public_key = recovery.previous_public;
            opener.sealed = recovery.previous_sealed;
            return true;
         }

         return false;
      }

      // The recovery key's private half, opened by the passphrase and checked.
      bool OpenRecoveryPrivate(const MailboxKeys::Recovery &recovery, const AnsiString &passphrase, std::string &privateKey, String &problem)
      {
         privateKey.clear();

         std::string opened;
         if (!MailboxCrypto::UnwrapWithSecret(passphrase, MailboxKeys::DomainPurpose(recovery.public_key), Bytes(recovery.sealed), opened, problem))
            return false;

         if (MailboxCrypto::PublicKeyOf(opened) != recovery.public_key)
         {
            MailboxCrypto::Forget(opened);
            problem = _T("what the passphrase opened is not the domain's recovery key");
            return false;
         }

         privateKey.swap(opened);
         return true;
      }

      // An account's key opened from its recovery copy, and checked.
      bool OpenAccountFromRecovery(const MailboxKeys::Row &row, const std::string &recoveryPrivate, std::string &privateKey, String &problem)
      {
         privateKey.clear();

         std::string opened;
         if (!MailboxCrypto::OpenSealed(recoveryPrivate, MailboxKeys::RecoveryPurpose(row.public_key), Bytes(row.recovery_seal), opened, problem))
            return false;

         if (MailboxCrypto::PublicKeyOf(opened) != row.public_key)
         {
            MailboxCrypto::Forget(opened);
            problem = _T("what the recovery key opened is not the account's mailbox key");
            return false;
         }

         privateKey.swap(opened);
         return true;
      }

      String ClosedKeySentence()
      {
         return _T("This mailbox is encrypted at rest and its key is closed, so a password set now would open nothing: its mail would stay unreadable. ")
                _T("An administrator sets it with the recovery, which opens the key with the domain's recovery passphrase (POST /api/v1/accounts/{address}/mailbox-encryption/recover, or the Control Deck's account page) - or, while the person is signed in with their password, it can be set as usual.");
      }

      String StaleSentence()
      {
         return _T("This mailbox is encrypted at rest, and its key did not open: sign in with the account's own password (LOGIN or AUTHENTICATE PLAIN). If the password is right and this is still refused, the key is sealed under an earlier password, and an administrator can open it again with the domain's recovery key.");
      }

      String UnreadableSentence()
      {
         return _T("Whether this mailbox is encrypted at rest could not be read from the database just now, so it is not opened on a guess. Try again in a moment.");
      }

      String DamagedWords()
      {
         return _T("This mailbox is encrypted at rest and its key's row in the database holds no key, so nothing can be stored in it or read from it until an administrator removes the row - DELETE /api/v1/accounts/{address}/mailbox-encryption?confirm=forfeit, after which the mail encrypted to the key it held can never be read - or puts it back from a backup.");
      }
   }

   // ---- MailboxKeyHold -----------------------------------------------------------------

   MailboxKeyHold::MailboxKeyHold(__int64 accountId) :
      account_id_(accountId)
   {
   }

   MailboxKeyHold::~MailboxKeyHold()
   {
      try
      {
         std::lock_guard<std::mutex> guard(OpenLock());
         std::map<__int64, OpenKey>::iterator found = OpenKeys().find(account_id_);
         if (found != OpenKeys().end())
         {
            if (found->second.holds > 0)
               found->second.holds--;

            // The idle time counts from the moment the last session let go.
            found->second.last_used = Now();
         }
      }
      catch (...)
      {
         // A destructor: nothing to be done but not to throw.
      }
   }

   // ---- the rows ---------------------------------------------------------------------------

   std::string
   MailboxKeys::PasswordPurpose(const std::string &publicKey)
   {
      return "mailbox key/password/" + MailboxCrypto::Base64(publicKey);
   }

   std::string
   MailboxKeys::RecoveryPurpose(const std::string &publicKey)
   {
      return "mailbox key/recovery/" + MailboxCrypto::Base64(publicKey);
   }

   std::string
   MailboxKeys::DomainPurpose(const std::string &recoveryPublic)
   {
      return "domain recovery key/" + MailboxCrypto::Base64(recoveryPublic);
   }

   bool
   MailboxKeys::GetRow(__int64 accountId, Row &row)
   {
      bool failed = false;
      return ReadRowFor(accountId, row, failed);
   }

   bool
   MailboxKeys::GetRecovery(__int64 domainId, Recovery &recovery)
   {
      recovery = Recovery();

      SQLCommand command(String(_T("select ")) + String(RecoveryColumns) + _T(" from hm_domainrecovery where drdomainid = @DOMAINID"));
      command.AddParameter("@DOMAINID", domainId);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset || recordset->IsEOF())
         return false;

      ReadRecovery(recordset, recovery);
      return recovery.public_key.size() == (size_t) MailboxCrypto::KeyBytes;
   }

   MailboxKeys::Keyed
   MailboxKeys::KeyedState(__int64 accountId)
   {
      KeyedEntry entry;
      return KeyedEntryOf(accountId, entry);
   }

   bool
   MailboxKeys::IsKeyed(__int64 accountId)
   {
      return KeyedState(accountId) != KeyedNo;
   }

   MailboxKeys::Keyed
   MailboxKeys::KeyOf(__int64 accountId, std::string &publicKey)
   {
      KeyedEntry entry;
      const Keyed keyed = KeyedEntryOf(accountId, entry);
      publicKey = keyed == KeyedYes ? entry.public_key : std::string();
      return keyed;
   }

   bool
   MailboxKeys::HasKey(__int64 accountId, bool &unknown)
   {
      unknown = false;

      // A row whose key is not a key is a row: the account has a key, damaged.
      KeyedEntry entry;
      const Keyed keyed = KeyedEntryOf(accountId, entry);
      if (keyed == KeyedUnknown)
      {
         unknown = true;
         return false;
      }

      return keyed == KeyedYes || keyed == KeyedDamaged;
   }

   MailboxKeys::Encryption
   MailboxKeys::EncryptionFor(std::shared_ptr<const Account> account, std::string &publicKey)
   {
      publicKey.clear();

      // A zero-access mailbox's delivered mail is ciphertext to the person's
      // own OpenPGP key already, and the two are never on one account; this
      // is the belt to ZeroAccess::Enable's braces.
      if (!account || account->GetZeroAccess())
         return EncryptionNone;

      KeyedEntry entry;
      const Keyed keyed = KeyedEntryOf(account->GetID(), entry);
      if (keyed == KeyedUnknown)
         return EncryptionUnknown;

      // Damaged whether switched on or not: a row that holds no key says
      // nothing true about whether new mail is to be encrypted.
      if (keyed == KeyedDamaged)
         return EncryptionDamaged;

      if (keyed == KeyedNo || !entry.enabled)
         return EncryptionNone;

      publicKey = entry.public_key;
      return EncryptionOn;
   }

   void
   MailboxKeys::Start()
   {
      // Read now, whatever was read before: a start, or a reinitialise.
      {
         std::lock_guard<std::mutex> guard(KeyedLock());
         KeyedRead().stale = true;
         KeyedRead().tried_at = 0;
      }

      // Nothing here stops the servers from starting: a read that throws is a
      // read that failed, and a sweep that throws is reported and left for the
      // next start (StartServers has set the state to starting by now, and an
      // exception out of it would leave a server neither started nor
      // startable).
      try
      {
         ReadKeyed();
      }
      catch (...)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, CodeRead, "MailboxKeys::Start",
            "Reading the accounts' mailbox keys at the start failed with an exception; they are read again at the next question.");
         return;
      }

      try
      {
         SweepKeyedDirectories_();
      }
      catch (...)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, CodeSweep, "MailboxKeys::Start",
            "Sweeping the keyed mailboxes' directories for what an encryption cut short left beside their files failed with an exception; nothing more was swept, and the next start sweeps again.");
      }
   }

   void
   MailboxKeys::SweepKeyedDirectories_()
   {
      // The keyed mailboxes' directories swept: a server with no keyed
      // account, or whose keys could not be read, reads nothing more.
      {
         std::lock_guard<std::mutex> guard(KeyedLock());
         if (!KeyedRead().loaded || KeyedSet().empty())
            return;
      }

      SQLCommand command("select a.accountaddress from hm_accounts a inner join hm_accountkeys k on k.akaccountid = a.accountid");
      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset)
         return;

      const String dataDirectory = IniFileSettings::Instance()->GetDataDirectory();
      std::vector<String> directories;
      while (!recordset->IsEOF())
      {
         const String address = recordset->GetStringValue("accountaddress");
         const String domain = StoreDirectoryName(StringParser::ExtractDomain(address));
         const String mailbox = StoreDirectoryName(StringParser::ExtractAddress(address));
         if (!domain.IsEmpty() && !mailbox.IsEmpty())
            directories.push_back(FileUtilities::Combine(FileUtilities::Combine(dataDirectory, domain), mailbox));
         recordset->MoveNext();
      }

      MessageStore::SweepEncryptionLeftovers(directories);
   }

   void
   MailboxKeys::Invalidate(__int64 accountId)
   {
      (void) accountId;
      InvalidateAll();
   }

   void
   MailboxKeys::InvalidateAll()
   {
      std::lock_guard<std::mutex> guard(KeyedLock());
      KeyedRead().stale = true;
      KeyedRead().tried_at = 0;
      KeyedRead().generation++;
   }

   void
   MailboxKeys::ForgetAccount(__int64 accountId)
   {
      CloseNow(accountId);
      ForgetKeyed(accountId);
   }

   void
   MailboxKeys::NewAccount(__int64 accountId)
   {
      if (accountId > 0)
         ForgetKeyed(accountId);
   }

   // ---- the open keys ---------------------------------------------------------------------

   bool
   MailboxKeys::FindOpenKey(const std::string &publicKey, std::string &privateKey, String &problem)
   {
      privateKey.clear();

      {
         std::lock_guard<std::mutex> guard(OpenLock());
         for (std::map<__int64, OpenKey>::const_iterator it = OpenKeys().begin(); it != OpenKeys().end(); ++it)
         {
            if (it->second.public_key != publicKey)
               continue;

            // Not a use: whoever reads - a delegate's client, a push, a
            // support session - does not keep the owner's key open. The
            // owner's sessions and sign-ins do (Hold, Touch, OnPasswordLogon).
            privateKey = it->second.private_key;
            return true;
         }
      }

      problem = String(LockedWords) + _T(" (the key opens at a sign-in with the account's password, and closes MailboxKeyIdleMinutes after its last use)");
      return false;
   }

   bool
   MailboxKeys::IsLockedProblem(const String &problem)
   {
      return problem.Find(LockedWords) == 0;
   }

   bool
   MailboxKeys::IsOpen(__int64 accountId)
   {
      std::lock_guard<std::mutex> guard(OpenLock());
      return OpenKeys().find(accountId) != OpenKeys().end();
   }

   unsigned __int64
   MailboxKeys::OpenEpoch(__int64 accountId)
   {
      std::lock_guard<std::mutex> guard(OpenLock());
      std::map<__int64, OpenKey>::const_iterator found = OpenKeys().find(accountId);
      return found == OpenKeys().end() ? 0 : found->second.epoch;
   }

   void
   MailboxKeys::Touch(__int64 accountId)
   {
      std::lock_guard<std::mutex> guard(OpenLock());
      std::map<__int64, OpenKey>::iterator found = OpenKeys().find(accountId);
      if (found != OpenKeys().end())
         found->second.last_used = Now();
   }

   std::shared_ptr<MailboxKeyHold>
   MailboxKeys::Hold(__int64 accountId)
   {
      {
         std::lock_guard<std::mutex> guard(OpenLock());
         std::map<__int64, OpenKey>::iterator found = OpenKeys().find(accountId);
         if (found == OpenKeys().end())
            return std::shared_ptr<MailboxKeyHold>();

         found->second.holds++;
         found->second.last_used = Now();
      }

      return std::shared_ptr<MailboxKeyHold>(new MailboxKeyHold(accountId));
   }

   void
   MailboxKeys::CloseIdle()
   {
      int minutes = Configuration::Instance() ? Configuration::Instance()->GetMailboxKeyIdleMinutes() : 15;
      if (minutes < 1)
         minutes = 1;

      const __int64 now = Now();
      bool closed = false;
      {
         std::lock_guard<std::mutex> guard(OpenLock());
         for (std::map<__int64, OpenKey>::iterator it = OpenKeys().begin(); it != OpenKeys().end(); )
         {
            const bool idle = it->second.holds <= 0 && (now < it->second.last_used || now - it->second.last_used >= (__int64) minutes * 60);
            if (!idle)
            {
               ++it;
               continue;
            }

            MailboxCrypto::Forget(it->second.private_key);
            it = OpenKeys().erase(it);
            closed = true;
         }
      }

      if (closed)
         MessageStore::ForgetDecodes();
   }

   void
   MailboxKeys::OpenFor(__int64 accountId, const std::string &publicKey, const std::string &privateKey)
   {
      OpenInMemory(accountId, publicKey, privateKey, std::string());
   }

   // ---- sign-in -------------------------------------------------------------------------------

   void
   MailboxKeys::OnPasswordLogon(std::shared_ptr<const Account> account, const String &password)
   {
      try
      {
         if (!account || password.IsEmpty() || !IsKeyed(account->GetID()))
            return;

         const __int64 accountId = account->GetID();

         Row row;
         if (!GetRow(accountId, row))
            return;

         AnsiString secret = Utf8(password);
         const std::string digest = PasswordDigest(secret);

         std::string privateKey;
         std::string knownDigest;
         if (OpenPrivateOf(accountId, row.public_key, privateKey, &knownDigest, true))
         {
            // Open already. The quick path: this password is the one that
            // opened it, and the key is wrapped by a password, not pending.
            if (!knownDigest.empty() && MailboxCrypto::ConstantTimeEquals(knownDigest, digest) && !row.password_wrap.IsEmpty() && row.pending.IsEmpty())
            {
               MailboxCrypto::Forget(privateKey);
               return;
            }

            // Wrapped by this password: nothing to do but remember it.
            std::string opened;
            String problem;
            if (!row.password_wrap.IsEmpty() && row.pending.IsEmpty() &&
                MailboxCrypto::UnwrapWithSecret(secret, PasswordPurpose(row.public_key), Bytes(row.password_wrap), opened, problem) &&
                opened == privateKey)
            {
               MailboxCrypto::Forget(opened);
               MailboxCrypto::Forget(privateKey);
               SetDigest(accountId, digest);
               return;
            }
            MailboxCrypto::Forget(opened);

            // Pending, or wrapped by an earlier password - one an administrator
            // or a reset link set where nothing could wrap the key again. The
            // key is open, so it is wrapped by this password now: once this
            // password is proved to be the account's own, since a script's
            // OnClientValidatePassword may have let a sign-in through on any.
            if (PasswordValidator::ValidateAccountPasswordOnly(account, password))
            {
               std::string wrapped;
               if (MailboxCrypto::WrapWithSecret(secret, PasswordPurpose(row.public_key), privateKey, wrapped, problem) && StoreWrap(accountId, wrapped))
               {
                  SetDigest(accountId, digest);
                  LOG_APPLICATION("Mailbox encryption at rest: the key of " + account->GetAddress() + " is now wrapped by the password it signed in with.");
               }
            }

            MailboxCrypto::Forget(privateKey);
            return;
         }

         // Closed. A pending key is wrapped by the password for the first time
         // and its machine copy removed; any other is unwrapped by it.
         if (!row.pending.IsEmpty())
         {
            if (!PasswordValidator::ValidateAccountPasswordOnly(account, password))
               return;

            if (!OpenPending(row, privateKey))
            {
               if (FirstReportThisHour(accountId))
               {
                  ErrorManager::Instance()->ReportError(ErrorManager::Medium, CodeUnwrapped, "MailboxKeys::OnPasswordLogon",
                     "The mailbox key of " + account->GetAddress() + " is held for its first sign-in under this machine's own key, and that copy could not be opened here - a database carried to another machine, or a machine key that changed. "
                     "Its mail stays unreadable until an administrator opens the key with the domain's recovery key (POST /api/v1/accounts/{address}/mailbox-encryption/recover).");
               }
               return;
            }

            std::string wrapped;
            String problem;
            if (!MailboxCrypto::WrapWithSecret(secret, PasswordPurpose(row.public_key), privateKey, wrapped, problem) || !StoreWrap(accountId, wrapped))
            {
               // Opened all the same, for this sign-in: the machine copy is
               // still there, and the next sign-in tries the wrap again.
               OpenInMemory(accountId, row.public_key, privateKey, std::string());
               MailboxCrypto::Forget(privateKey);
               return;
            }

            OpenInMemory(accountId, row.public_key, privateKey, digest);
            MailboxCrypto::Forget(privateKey);
            LOG_APPLICATION("Mailbox encryption at rest: " + account->GetAddress() + " signed in with its password for the first time since its mailbox was encrypted; the key is wrapped by the password now, and the copy under this machine's key is gone.");
            return;
         }

         if (row.password_wrap.IsEmpty())
            return;

         String problem;
         if (MailboxCrypto::UnwrapWithSecret(secret, PasswordPurpose(row.public_key), Bytes(row.password_wrap), privateKey, problem) &&
             MailboxCrypto::PublicKeyOf(privateKey) == row.public_key)
         {
            OpenInMemory(accountId, row.public_key, privateKey, digest);
            MailboxCrypto::Forget(privateKey);
            return;
         }
         MailboxCrypto::Forget(privateKey);

         // Not this password's. Said only when the password is the account's
         // own - then the wrap is an earlier password's, and somebody has to
         // recover the key - and not for a sign-in a script let through.
         if (PasswordValidator::ValidateAccountPasswordOnly(account, password) && FirstReportThisHour(accountId))
         {
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, CodeStale, "MailboxKeys::OnPasswordLogon",
               "The mailbox key of " + account->GetAddress() + " did not open with the password it signed in with: it is wrapped by an earlier password, set where the key was closed and could not be wrapped again. "
               "Its mail stays unreadable, and IMAP and POP3 refuse it, until an administrator opens the key with the domain's recovery key (POST /api/v1/accounts/{address}/mailbox-encryption/recover), which wraps it by a password again.");
         }
      }
      catch (...)
      {
         // On the success path of a sign-in, which has been proved: the key
         // simply stays as it was, and the session decides with SessionRefusal.
         // Said once an hour an account at most, as the failures above are.
         if (account && FirstReportThisHour(account->GetID()))
         {
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, CodeRead, "MailboxKeys::OnPasswordLogon",
               _T("The mailbox key of ") + account->GetAddress() + _T(" could not be opened at its sign-in: an unexpected error stopped it. The key stays as it was, and the next sign-in tries again."));
         }
      }
   }

   String
   MailboxKeys::SessionRefusal(std::shared_ptr<const Account> account, int credential)
   {
      if (!account)
         return String();

      KeyedEntry entry;
      const Keyed keyed = KeyedEntryOf(account->GetID(), entry);
      if (keyed == KeyedNo)
         return String();

      if (keyed == KeyedUnknown)
         return UnreadableSentence();

      if (keyed == KeyedDamaged)
         return DamagedWords();

      if (credential == CredentialMasquerade)
         return _T("This mailbox is encrypted at rest and opens only with its own password: another account signing in as it, the IMAP master user included, cannot open it, and is not let in while its owner's key happens to be open.");

      if (credential != SignInStore::CredentialPassword)
         return _T("This mailbox is encrypted at rest and opens only with its own password: an app password, a token, a certificate, a passkey or a single sign-on cannot open it. Sign in with the account's password.");

      // The password was the account's and the key did not open. When the
      // key's row could not be read for it just now, that is the reason - for
      // now - and not a wrap under an earlier password.
      if (!IsOpen(account->GetID()))
      {
         bool failed = false;
         Row row;
         ReadRowFor(account->GetID(), row, failed);
         return failed ? UnreadableSentence() : StaleSentence();
      }

      return String();
   }

   bool
   MailboxKeys::IsTemporaryRefusal(const String &refusal)
   {
      return !refusal.IsEmpty() && refusal == UnreadableSentence();
   }

   String
   MailboxKeys::DamagedSentence()
   {
      return DamagedWords();
   }

   bool
   MailboxKeys::IsDamagedRefusal(const String &refusal)
   {
      return !refusal.IsEmpty() && refusal == DamagedWords();
   }

   String
   MailboxKeys::ImapRefusal(const String &refusal)
   {
      return (IsDamagedRefusal(refusal) ? String(_T("[CONTACTADMIN] ")) : String(_T("[UNAVAILABLE] "))) + refusal;
   }

   int
   MailboxKeys::RefusalStatus(const String &refusal, int otherwise)
   {
      if (IsTemporaryRefusal(refusal))
         return 503;
      if (IsDamagedRefusal(refusal))
         return 409;
      return otherwise;
   }

   // ---- a new password -----------------------------------------------------------------------

   bool
   MailboxKeys::PrepareForNewPassword(std::shared_ptr<Account> account, const String &password, String &refusal)
   {
      refusal = String();

      const Keyed keyed = account && account->GetID() != 0 ? KeyedState(account->GetID()) : KeyedNo;
      if (keyed == KeyedNo)
         return true;

      if (keyed == KeyedDamaged)
      {
         refusal = DamagedWords();
         return false;
      }

      // Read now, for the pending copy, which the set does not hold. When the
      // database will not give the row, the key's public half is the set's,
      // and a key that is open is wrapped with it - nothing else of the row
      // is needed for that; one that is not open is refused for now.
      bool failed = false;
      Row row;
      if (!ReadRowFor(account->GetID(), row, failed))
      {
         if (!failed)
            return true;

         KeyedEntry entry;
         if (KeyedEntryOf(account->GetID(), entry) != KeyedYes)
         {
            refusal = UnreadableSentence();
            return false;
         }

         row = Row();
         row.public_key = entry.public_key;
      }

      // Not a use: an administrator's password set wraps the key and does not
      // keep it open for the person (docs/EncryptionAtRest.md, "Passwords").
      std::string privateKey;
      if (!OpenPrivateOf(account->GetID(), row.public_key, privateKey, nullptr, false) && !OpenPending(row, privateKey))
      {
         refusal = failed ? UnreadableSentence() : ClosedKeySentence();
         return false;
      }

      std::string wrapped;
      String problem;
      const bool ok = MailboxCrypto::WrapWithSecret(Utf8(password), PasswordPurpose(row.public_key), privateKey, wrapped, problem);
      MailboxCrypto::Forget(privateKey);

      if (!ok)
      {
         refusal = _T("The mailbox key could not be wrapped by the new password: ") + problem + _T(". Nothing was changed.");
         return false;
      }

      account->SetMailboxKeyWrap(AnsiString(wrapped.c_str()));
      return true;
   }

   bool
   MailboxKeys::CanWrapNewPassword(__int64 accountId, String &refusal)
   {
      refusal = String();

      const Keyed keyed = accountId > 0 ? KeyedState(accountId) : KeyedNo;
      if (keyed == KeyedNo)
         return true;

      if (keyed == KeyedDamaged)
      {
         refusal = DamagedWords();
         return false;
      }

      bool failed = false;
      Row row;
      if (!ReadRowFor(accountId, row, failed))
      {
         if (!failed)
            return true;

         // An open key can be wrapped without the row; anything else is not
         // known for now.
         if (KeyedState(accountId) == KeyedYes && IsOpen(accountId))
            return true;

         refusal = UnreadableSentence();
         return false;
      }

      // Open, or pending under the machine's key: either can be wrapped.
      if (IsOpen(accountId) || !row.pending.IsEmpty())
         return true;

      refusal = ClosedKeySentence();
      return false;
   }

   bool
   MailboxKeys::RestoreWraps(__int64 accountId, const Row &before)
   {
      SQLCommand command("update hm_accountkeys set akpassword = @PASSWORD, akpending = @PENDING, akchanged = @CHANGED where akaccountid = @ACCOUNTID");
      command.AddParameter("@PASSWORD", String(before.password_wrap));
      command.AddParameter("@PENDING", String(before.pending));
      command.AddParameter("@CHANGED", Now());
      command.AddParameter("@ACCOUNTID", accountId);

      const bool stored = Application::Instance()->GetDBManager()->Execute(command);

      // The next sign-in proves its password against the wrap put back.
      SetDigest(accountId, std::string());

      if (!stored)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, CodeStore, "MailboxKeys::RestoreWraps",
            "The mailbox key of an account could not be put back under the password a change that was undone had replaced. Its key is wrapped by the password that change set; the domain's recovery key opens it.");
      }

      return stored;
   }

   void
   MailboxKeys::StorePreparedWrap(std::shared_ptr<Account> account)
   {
      if (!account)
         return;

      const AnsiString wrap = account->GetMailboxKeyWrap();
      if (wrap.IsEmpty())
         return;

      account->SetMailboxKeyWrap(AnsiString());
      StoreWrap(account->GetID(), Bytes(wrap));

      // The next sign-in proves its password against the new wrap.
      SetDigest(account->GetID(), std::string());
   }

   // ---- administration -----------------------------------------------------------------------

   bool
   MailboxKeys::Enable(std::shared_ptr<const Account> account, AnsiString &problem, int &status)
   {
      status = 500;

      if (!account)
      {
         problem = "no such account";
         status = 404;
         return false;
      }

      if (account->GetIsAD())
      {
         problem = "this account signs in against a Windows directory, whose password this server never holds and whose changes it never sees; its key could not be kept wrapped by it";
         status = 409;
         return false;
      }

      if (account->GetZeroAccess())
      {
         problem = "this is a zero-access mailbox: its mail is already encrypted, end to end, to a key this server cannot open (docs/ZeroAccess.md), and the two are not put on one mailbox";
         status = 409;
         return false;
      }

      // A second factor makes an app password the only credential where none
      // is asked - IMAP, POP3, SMTP, ManageSieve, DAV - and an encrypted
      // mailbox opens only with its own password: every client would be shut
      // out. The two are not put on one account.
      if (!account->GetTotpSecret().IsEmpty())
      {
         problem = "this account has a second factor (TOTP) enrolled, which makes an app password the only credential its mail clients can sign in with, and an encrypted mailbox opens only with its own password: every client would be shut out. Remove the second factor first, or leave this mailbox unencrypted";
         status = 409;
         return false;
      }

      // Read now: whether a row is there decides between switching an existing
      // key on and making one, and the database not answering is neither.
      bool failed = false;
      bool damaged = false;
      Row existing;
      const bool hasRow = ReadRowFor(account->GetID(), existing, failed, damaged);
      if (failed)
      {
         problem = "whether the account already has a key could not be read just now; try again in a moment";
         status = 503;
         return false;
      }

      if (damaged)
      {
         problem = Utf8(DamagedWords());
         status = 409;
         return false;
      }

      if (hasRow)
      {
         if (!existing.enabled)
         {
            SQLCommand command("update hm_accountkeys set akenabled = @ENABLED, akchanged = @CHANGED where akaccountid = @ACCOUNTID");
            command.AddParameter("@ENABLED", 1);
            command.AddParameter("@CHANGED", Now());
            command.AddParameter("@ACCOUNTID", account->GetID());
            if (!Application::Instance()->GetDBManager()->Execute(command))
            {
               problem = "the key could not be switched on";
               return false;
            }
         }

         RememberKeyed(account->GetID(), true, existing.public_key);
         status = 200;
         return true;
      }

      // From the read of the recovery key to the stored key, no replacement of
      // it runs (RecoveryLock): the key is sealed to the recovery key the row
      // still holds when it is stored.
      std::lock_guard<std::mutex> recoveryGuard(RecoveryLock());

      // A second switch-on of the same account that read "no key" alongside
      // this one: the row is read again under the lock, and the one that
      // comes second is told, where it met the unique index and a 500.
      {
         bool againFailed = false;
         bool againDamaged = false;
         Row made;
         if (ReadRowFor(account->GetID(), made, againFailed, againDamaged) || againDamaged)
         {
            problem = "the account's mailbox key was made a moment ago, by another request; it is switched on";
            status = 409;
            return false;
         }

         if (againFailed)
         {
            problem = "whether the account already has a key could not be read just now; try again in a moment";
            status = 503;
            return false;
         }
      }

      Recovery recovery;
      if (!GetRecovery(account->GetDomainID(), recovery))
      {
         problem = "the account's domain has no recovery key: make it first (PUT /api/v1/domains/{domain}/mailbox-recovery), so that a mailbox whose password is lost is not a mailbox lost";
         status = 409;
         return false;
      }

      std::string privateKey;
      std::string publicKey;
      if (!MailboxCrypto::GenerateKeyPair(privateKey, publicKey))
      {
         problem = "no key pair could be made";
         return false;
      }

      std::string sealed;
      String sealProblem;
      if (!MailboxCrypto::SealToPublic(recovery.public_key, RecoveryPurpose(publicKey), privateKey, sealed, sealProblem))
      {
         MailboxCrypto::Forget(privateKey);
         problem = "the key could not be sealed to the domain's recovery key: " + Utf8(sealProblem);
         return false;
      }

      // Until the first sign-in with the password, under this machine's key:
      // nothing here has the password to wrap it by.
      AnsiString pending;
      if (!DataProtector::Protect(AnsiString(MailboxCrypto::Base64(privateKey).c_str()), pending) || pending.IsEmpty())
      {
         MailboxCrypto::Forget(privateKey);
         problem = "the key could not be held for the first sign-in under this machine's secret store (on Linux, the key file inside the data directory; the error log says why)";
         return false;
      }

      const __int64 now = Now();

      SQLStatement statement;
      statement.SetTable("hm_accountkeys");
      statement.SetStatementType(SQLStatement::STInsert);
      statement.SetIdentityColumn("akid");
      statement.AddColumnInt64("akaccountid", account->GetID());
      statement.AddColumn("akenabled", (long) 1);
      statement.AddColumn("akpublic", Base64Column(publicKey));
      statement.AddColumn("akpassword", String());
      statement.AddColumn("akrecovery", String(AnsiString(sealed.c_str())));
      statement.AddColumn("akrecoverypublic", Base64Column(recovery.public_key));
      statement.AddColumn("akpending", String(pending));
      statement.AddColumnInt64("akcreated", now);
      statement.AddColumnInt64("akchanged", now);

      __int64 id = 0;
      if (!Application::Instance()->GetDBManager()->Execute(statement, &id) || id == 0)
      {
         MailboxCrypto::Forget(privateKey);
         problem = "the key could not be stored";
         return false;
      }

      // Open from the switch on - the sessions the account had are ended by
      // the route that switched it on (RestApiMailboxKeys.cpp), and a new one
      // is made only on the password - and it closes as any key does.
      OpenInMemory(account->GetID(), publicKey, privateKey, std::string());
      MailboxCrypto::Forget(privateKey);

      // Keyed in the set from here on; then what the index held of this
      // account, built from its text, goes - and an indexer pass that read "no
      // key" before the switch drops what it wrote after it, since it asks the
      // set again once it has written (MessageIndexer).
      RememberKeyed(account->GetID(), true, publicKey);
      ForgetIndex(account->GetID());

      LOG_APPLICATION("Mailbox encryption at rest: switched on for " + account->GetAddress() + ". Mail stored from now on is encrypted to its key; hmailserver --encrypt-store --account encrypts what is already there.");

      status = 201;
      return true;
   }

   bool
   MailboxKeys::Disable(std::shared_ptr<const Account> account, AnsiString &problem, int &status)
   {
      status = 500;

      Row existing;
      bool failed = false;
      bool damaged = false;
      if (!account || !ReadRowFor(account->GetID(), existing, failed, damaged))
      {
         if (failed)
         {
            problem = "the account's mailbox key could not be read just now; try again in a moment";
            status = 503;
            return false;
         }

         if (damaged)
         {
            problem = Utf8(DamagedWords());
            status = 409;
            return false;
         }

         problem = "the account has no mailbox key";
         status = 404;
         return false;
      }

      SQLCommand command("update hm_accountkeys set akenabled = @ENABLED, akchanged = @CHANGED where akaccountid = @ACCOUNTID");
      command.AddParameter("@ENABLED", 0);
      command.AddParameter("@CHANGED", Now());
      command.AddParameter("@ACCOUNTID", account->GetID());
      if (!Application::Instance()->GetDBManager()->Execute(command))
      {
         problem = "the key could not be switched off";
         return false;
      }

      RememberKeyed(account->GetID(), false, existing.public_key);

      LOG_APPLICATION("Mailbox encryption at rest: switched off for " + account->GetAddress() + ". New mail is stored as it comes; what is encrypted stays encrypted, and the key stays to read it.");

      status = 200;
      return true;
   }

   bool
   MailboxKeys::Forfeit(std::shared_ptr<const Account> account, AnsiString &problem, int &status)
   {
      status = 500;

      // A row whose key is not a key is removed like any other: it is the one
      // way out for an account whose row was damaged (DamagedSentence).
      Row existing;
      bool failed = false;
      bool damaged = false;
      if (!account || (!ReadRowFor(account->GetID(), existing, failed, damaged) && !damaged))
      {
         if (failed)
         {
            problem = "the account's mailbox key could not be read just now; try again in a moment";
            status = 503;
            return false;
         }

         problem = "the account has no mailbox key";
         status = 404;
         return false;
      }

      // The key is the only way to the mail encrypted to it: removing it is
      // deleting that mail, which a legal hold forbids (DiscoveryHolds.h).
      if (DiscoveryHolds::IsAccountHeld(account->GetID()))
      {
         problem = AnsiString("The account ") + Utf8(account->GetAddress()) +
                   AnsiString(" is under a legal hold, so its mailbox key - without which the mail encrypted to it can never be read again - cannot be removed until the hold is released (docs/Discovery.md).");
         status = 409;
         return false;
      }

      SQLCommand command("delete from hm_accountkeys where akaccountid = @ACCOUNTID");
      command.AddParameter("@ACCOUNTID", account->GetID());
      if (!Application::Instance()->GetDBManager()->Execute(command))
      {
         problem = "the key could not be removed";
         return false;
      }

      CloseNow(account->GetID());
      ForgetKeyed(account->GetID());

      LOG_APPLICATION("Mailbox encryption at rest: the key of " + account->GetAddress() + " was removed. Any of its messages still encrypted to it can no longer be read by anybody.");

      status = 200;
      return true;
   }

   bool
   MailboxKeys::OpenPrivateWithRecovery(__int64 accountId, const AnsiString &passphrase, std::string &publicKey,
                                        std::string &privateKey, String &problem)
   {
      publicKey.clear();
      privateKey.clear();

      Row row;
      if (!GetRow(accountId, row))
      {
         problem = _T("the account has no mailbox key");
         return false;
      }

      SQLCommand command("select accountdomainid from hm_accounts where accountid = @ACCOUNTID");
      command.AddParameter("@ACCOUNTID", accountId);
      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset || recordset->IsEOF())
      {
         problem = _T("the account could not be read");
         return false;
      }

      const __int64 domainId = recordset->GetInt64Value("accountdomainid");

      Recovery recovery;
      if (!GetRecovery(domainId, recovery))
      {
         problem = _T("the account's domain has no recovery key");
         return false;
      }

      // The current key, or the one a replacement under way (or cut short)
      // keeps beside it: the passphrase opens both.
      Recovery opener;
      if (!RecoveryFor(row, recovery, opener))
      {
         problem = _T("the account's key is sealed to a recovery key its domain does not have - an earlier one, or another domain's");
         return false;
      }

      std::string recoveryPrivate;
      if (!OpenRecoveryPrivate(opener, passphrase, recoveryPrivate, problem))
      {
         problem = _T("the passphrase does not open the domain's recovery key");
         return false;
      }

      const bool opened = OpenAccountFromRecovery(row, recoveryPrivate, privateKey, problem);
      MailboxCrypto::Forget(recoveryPrivate);
      if (!opened)
         return false;

      publicKey = row.public_key;
      return true;
   }

   bool
   MailboxKeys::OpenWithRecovery(std::shared_ptr<const Account> account, const AnsiString &passphrase, AnsiString &problem, int &status)
   {
      status = 500;

      const Keyed keyed = account ? KeyedState(account->GetID()) : KeyedNo;
      if (keyed == KeyedUnknown)
      {
         problem = "whether the account has a mailbox key could not be read just now; try again in a moment";
         status = 503;
         return false;
      }

      if (keyed == KeyedDamaged)
      {
         problem = Utf8(DamagedWords());
         status = 409;
         return false;
      }

      if (keyed == KeyedNo)
      {
         problem = "the account has no mailbox key";
         status = 404;
         return false;
      }

      std::string publicKey;
      std::string privateKey;
      String why;
      if (!OpenPrivateWithRecovery(account->GetID(), passphrase, publicKey, privateKey, why))
      {
         problem = Utf8(why);
         status = why == _T("the passphrase does not open the domain's recovery key") ? 403 : 409;
         return false;
      }

      OpenInMemory(account->GetID(), publicKey, privateKey, std::string());
      MailboxCrypto::Forget(privateKey);

      status = 200;
      return true;
   }

   int
   MailboxKeys::KeyedAccountsInDomain(__int64 domainId)
   {
      SQLCommand command("select k.akaccountid from hm_accountkeys k inner join hm_accounts a on k.akaccountid = a.accountid where a.accountdomainid = @DOMAINID");
      command.AddParameter("@DOMAINID", domainId);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset)
         return -1;

      int count = 0;
      while (!recordset->IsEOF())
      {
         count++;
         recordset->MoveNext();
      }
      return count;
   }

   bool
   MailboxKeys::SetRecovery(__int64 domainId, const AnsiString &passphrase, const String &hint, const AnsiString &currentPassphrase,
                            AnsiString &problem, int &status, int &resealed)
   {
      status = 500;
      resealed = 0;

      String passphraseText;
      Unicode::MultiByteToWide(passphrase, passphraseText);
      if (passphraseText.GetLength() < MinimumPassphraseLength)
      {
         problem.Format("passphrase: at least %d characters - it is the one thing between a stolen database and every encrypted mailbox of the domain", (int) MinimumPassphraseLength);
         status = 400;
         return false;
      }

      if (hint.GetLength() > 255)
      {
         problem = "hint: at most 255 characters";
         status = 400;
         return false;
      }

      // One at a time, and no switch-on in the middle (RecoveryLock): from the
      // read of the domain's row below to the last write.
      std::lock_guard<std::mutex> recoveryGuard(RecoveryLock());

      Recovery existing;
      const bool replacing = GetRecovery(domainId, existing);

      std::string newPrivate;
      std::string newPublic;
      if (!MailboxCrypto::GenerateKeyPair(newPrivate, newPublic))
      {
         problem = "no key pair could be made";
         return false;
      }

      // ---- the first recovery key of the domain ----------------------------------
      if (!replacing)
      {
         std::string wrapped;
         String wrapProblem;
         const bool wrappedOk = MailboxCrypto::WrapWithSecret(passphrase, DomainPurpose(newPublic), newPrivate, wrapped, wrapProblem);
         MailboxCrypto::Forget(newPrivate);
         if (!wrappedOk)
         {
            problem = "the recovery key could not be wrapped by the passphrase: " + Utf8(wrapProblem);
            return false;
         }

         SQLStatement statement;
         statement.SetTable("hm_domainrecovery");
         statement.SetStatementType(SQLStatement::STInsert);
         statement.SetIdentityColumn("drid");
         statement.AddColumnInt64("drdomainid", domainId);
         statement.AddColumn("drpublic", Base64Column(newPublic));
         statement.AddColumn("drsealed", String(AnsiString(wrapped.c_str())));
         statement.AddColumn("drhint", hint);
         statement.AddColumnInt64("drcreated", Now());
         statement.AddColumn("drprevpublic", String());
         statement.AddColumn("drprevsealed", String());

         __int64 id = 0;
         if (!Application::Instance()->GetDBManager()->Execute(statement, &id) || id == 0)
         {
            problem = "the recovery key could not be stored";
            return false;
         }

         status = 201;
         return true;
      }

      // ---- a replacement ------------------------------------------------------------
      //
      // Everything that can refuse is asked before anything is written: the
      // current passphrase opens the domain's key, and the key an earlier
      // replacement that was cut short left beside it; every account's key of
      // the domain opens with whichever of the two it is sealed to, and is
      // sealed to the new key. Then the writes, in an order that loses nothing
      // wherever it stops - there is no transaction across them to lean on:
      //
      //   0. an earlier replacement left unfinished is finished: its accounts
      //      sealed to the key it replaced are sealed to the current one, and
      //      the replaced key goes;
      //   1. the domain's row in one statement: the new key, wrapped by the new
      //      passphrase, and beside it the current key, wrapped by the new
      //      passphrase as well - from here the new passphrase opens every
      //      account, whichever key it is sealed to;
      //   2. each account sealed to the new key;
      //   3. the replaced key goes.
      //
      // Cut short after 1, the next replacement - given the new passphrase as
      // the current one - finishes this one at its step 0, and the recovery
      // opens every account meanwhile (RecoveryFor).
      if (currentPassphrase.IsEmpty())
      {
         MailboxCrypto::Forget(newPrivate);
         problem = "current_passphrase: the domain has a recovery key, and replacing it needs the passphrase that opens it, since every account's key is sealed to it again";
         status = 409;
         return false;
      }

      std::string currentPrivate;
      String why;
      if (!OpenRecoveryPrivate(existing, currentPassphrase, currentPrivate, why))
      {
         MailboxCrypto::Forget(newPrivate);
         problem = "current_passphrase does not open the domain's recovery key";
         status = 403;
         return false;
      }

      std::string previousPrivate;
      if (!existing.previous_public.empty())
      {
         Recovery previous;
         previous.public_key = existing.previous_public;
         previous.sealed = existing.previous_sealed;
         if (!OpenRecoveryPrivate(previous, currentPassphrase, previousPrivate, why))
         {
            MailboxCrypto::Forget(currentPrivate);
            MailboxCrypto::Forget(newPrivate);
            problem = "the key an earlier replacement of this recovery key left beside it does not open with current_passphrase; nothing was changed";
            status = 409;
            return false;
         }
      }

      struct Reseal
      {
         __int64 account_id = 0;
         bool on_previous = false;  // sealed to the key an earlier replacement left
         std::string to_current;    // sealed to the current key: step 0's
         std::string to_new;        // sealed to the new key: step 2's
      };

      std::vector<Reseal> reseals;
      {
         SQLCommand command(String(_T("select ")) + _T("k.akid, k.akaccountid, k.akenabled, k.akpublic, k.akpassword, k.akrecovery, k.akrecoverypublic, k.akpending, k.akcreated, k.akchanged, a.accountaddress") +
                            _T(" from hm_accountkeys k inner join hm_accounts a on k.akaccountid = a.accountid where a.accountdomainid = @DOMAINID"));
         command.AddParameter("@DOMAINID", domainId);

         std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
         if (!recordset)
         {
            MailboxCrypto::Forget(previousPrivate);
            MailboxCrypto::Forget(currentPrivate);
            MailboxCrypto::Forget(newPrivate);
            problem = "the domain's mailbox keys could not be read";
            return false;
         }

         while (!recordset->IsEOF())
         {
            Row row;
            ReadRow(recordset, row);
            const String address = recordset->GetStringValue("accountaddress");
            recordset->MoveNext();

            // A row whose key is not a key cannot be sealed again, and is named:
            // the replacement is refused with the way out, as everything else
            // such a row meets is, rather than with a number and a sealing
            // error. Nothing is changed, so that a row put back from a backup
            // still finds the recovery key it was sealed to.
            if (row.public_key.size() != (size_t) MailboxCrypto::KeyBytes)
            {
               MailboxCrypto::Forget(previousPrivate);
               MailboxCrypto::Forget(currentPrivate);
               MailboxCrypto::Forget(newPrivate);
               problem = "the recovery key was not replaced, and nothing was changed: the mailbox key row of " + Utf8(address) + " holds no key, so it cannot be sealed to the new one. " + Utf8(DamagedWords());
               status = 409;
               return false;
            }

            Reseal reseal;
            reseal.account_id = row.account_id;
            reseal.on_previous = !existing.previous_public.empty() && row.recovery_public == existing.previous_public;

            const bool known = row.recovery_public == existing.public_key || reseal.on_previous;

            std::string accountPrivate;
            String sealProblem;
            bool ok = known &&
                      OpenAccountFromRecovery(row, reseal.on_previous ? previousPrivate : currentPrivate, accountPrivate, sealProblem) &&
                      MailboxCrypto::SealToPublic(newPublic, RecoveryPurpose(row.public_key), accountPrivate, reseal.to_new, sealProblem);

            if (ok && reseal.on_previous)
               ok = MailboxCrypto::SealToPublic(existing.public_key, RecoveryPurpose(row.public_key), accountPrivate, reseal.to_current, sealProblem);

            MailboxCrypto::Forget(accountPrivate);

            if (!ok)
            {
               MailboxCrypto::Forget(previousPrivate);
               MailboxCrypto::Forget(currentPrivate);
               MailboxCrypto::Forget(newPrivate);
               if (!known)
                  sealProblem = _T("it is sealed to a recovery key this domain does not have - another domain's, or one replaced before");
               problem.Format("the mailbox key of account %I64d could not be sealed to the new recovery key (%s); nothing was changed", row.account_id, Utf8(sealProblem).c_str());
               status = 409;
               return false;
            }

            reseals.push_back(reseal);
         }
      }

      MailboxCrypto::Forget(previousPrivate);

      // Both wrapped by the new passphrase: the new key, and the current one
      // kept beside it until every account is sealed to the new one.
      std::string wrapped;
      std::string previousWrapped;
      String wrapProblem;
      const bool wrappedOk = MailboxCrypto::WrapWithSecret(passphrase, DomainPurpose(newPublic), newPrivate, wrapped, wrapProblem) &&
                             MailboxCrypto::WrapWithSecret(passphrase, DomainPurpose(existing.public_key), currentPrivate, previousWrapped, wrapProblem);
      MailboxCrypto::Forget(newPrivate);
      MailboxCrypto::Forget(currentPrivate);
      if (!wrappedOk)
      {
         problem = "the recovery key could not be wrapped by the passphrase: " + Utf8(wrapProblem);
         return false;
      }

      // 0. An earlier replacement finished.
      if (!existing.previous_public.empty())
      {
         for (size_t i = 0; i < reseals.size(); i++)
         {
            if (!reseals[i].on_previous)
               continue;

            SQLCommand command("update hm_accountkeys set akrecovery = @SEALCOPY, akrecoverypublic = @SEALEDTO, akchanged = @CHANGED where akaccountid = @ACCOUNTID");
            command.AddParameter("@SEALCOPY", String(AnsiString(reseals[i].to_current.c_str())));
            command.AddParameter("@SEALEDTO", Base64Column(existing.public_key));
            command.AddParameter("@CHANGED", Now());
            command.AddParameter("@ACCOUNTID", reseals[i].account_id);
            if (!Application::Instance()->GetDBManager()->Execute(command))
            {
               problem = "an earlier replacement of this recovery key, cut short, could not be finished, so this one was not begun; every account still opens with the current passphrase - replace the key again with it";
               return false;
            }
         }

         SQLCommand clear("update hm_domainrecovery set drprevpublic = @PREVPUBLIC, drprevsealed = @PREVSEALED where drdomainid = @DOMAINID");
         clear.AddParameter("@PREVPUBLIC", String());
         clear.AddParameter("@PREVSEALED", String());
         clear.AddParameter("@DOMAINID", domainId);
         if (!Application::Instance()->GetDBManager()->Execute(clear))
         {
            problem = "an earlier replacement of this recovery key, cut short, could not be finished, so this one was not begun; every account still opens with the current passphrase - replace the key again with it";
            return false;
         }
      }

      // 1. The domain's row: the new key, and the current one beside it.
      {
         SQLCommand command("update hm_domainrecovery set drpublic = @PUBLIC, drsealed = @SEALED, drprevpublic = @PREVPUBLIC, drprevsealed = @PREVSEALED, drhint = @HINT, drcreated = @CREATED where drdomainid = @DOMAINID");
         command.AddParameter("@PUBLIC", Base64Column(newPublic));
         command.AddParameter("@SEALED", String(AnsiString(wrapped.c_str())));
         command.AddParameter("@PREVPUBLIC", Base64Column(existing.public_key));
         command.AddParameter("@PREVSEALED", String(AnsiString(previousWrapped.c_str())));
         command.AddParameter("@HINT", hint);
         command.AddParameter("@CREATED", Now());
         command.AddParameter("@DOMAINID", domainId);
         if (!Application::Instance()->GetDBManager()->Execute(command))
         {
            problem = "the new recovery key could not be stored; nothing was changed, and the current passphrase still opens every account";
            return false;
         }
      }

      // 2. Each account sealed to the new key.
      for (size_t i = 0; i < reseals.size(); i++)
      {
         SQLCommand command("update hm_accountkeys set akrecovery = @SEALCOPY, akrecoverypublic = @SEALEDTO, akchanged = @CHANGED where akaccountid = @ACCOUNTID");
         command.AddParameter("@SEALCOPY", String(AnsiString(reseals[i].to_new.c_str())));
         command.AddParameter("@SEALEDTO", Base64Column(newPublic));
         command.AddParameter("@CHANGED", Now());
         command.AddParameter("@ACCOUNTID", reseals[i].account_id);
         if (!Application::Instance()->GetDBManager()->Execute(command))
         {
            problem.Format("the replacement did not finish: %d of %d accounts' keys are sealed to the new recovery key and the rest still to the one it replaces, which the new passphrase opens as well - nothing is lost; replace the key again, giving the new passphrase as current_passphrase, to finish it",
               (int) i, (int) reseals.size());
            return false;
         }
      }

      // 3. The replaced key goes. A failure here leaves it beside the new one,
      // wrapped by the new passphrase and sealing nothing: the next
      // replacement clears it.
      {
         SQLCommand clear("update hm_domainrecovery set drprevpublic = @PREVPUBLIC, drprevsealed = @PREVSEALED where drdomainid = @DOMAINID");
         clear.AddParameter("@PREVPUBLIC", String());
         clear.AddParameter("@PREVSEALED", String());
         clear.AddParameter("@DOMAINID", domainId);
         if (!Application::Instance()->GetDBManager()->Execute(clear))
            LOG_APPLICATION("Mailbox encryption at rest: the recovery key a replacement replaced could not be removed from beside the new one; it seals nothing, and the next replacement removes it.");
      }

      // The set holds nothing of the recovery copies: nothing in it changes.
      resealed = (int) reseals.size();
      status = 200;
      return true;
   }

   bool
   MailboxKeys::RemoveRecovery(__int64 domainId, AnsiString &problem, int &status)
   {
      status = 500;

      // Not while a switch-on is sealing a key to it (RecoveryLock).
      std::lock_guard<std::mutex> recoveryGuard(RecoveryLock());

      Recovery existing;
      if (!GetRecovery(domainId, existing))
      {
         problem = "the domain has no recovery key";
         status = 404;
         return false;
      }

      const int keyed = KeyedAccountsInDomain(domainId);
      if (keyed != 0)
      {
         problem = keyed < 0 ? AnsiString("the domain's mailbox keys could not be counted") :
                   AnsiString("accounts of the domain have mailbox keys sealed to it; remove those keys first, or replace the recovery key rather than remove it");
         status = keyed < 0 ? 500 : 409;
         return false;
      }

      SQLCommand command("delete from hm_domainrecovery where drdomainid = @DOMAINID");
      command.AddParameter("@DOMAINID", domainId);
      if (!Application::Instance()->GetDBManager()->Execute(command))
      {
         problem = "the recovery key could not be removed";
         return false;
      }

      status = 200;
      return true;
   }

   // ---- what the routes answer ---------------------------------------------------------------

   AnsiString
   MailboxKeys::AccountJson(std::shared_ptr<const Account> account, bool forTheAccount)
   {
      // A row whose key is not a key is a row: keyed, and said to be damaged,
      // so that the card offers the forfeit rather than a switch-on.
      Row row;
      bool failed = false;
      bool damaged = false;
      const bool keyed = account && (ReadRowFor(account->GetID(), row, failed, damaged) || damaged);
      const bool open = keyed && !damaged && IsOpen(account->GetID());

      AnsiString json;
      json.Format("{\"enabled\":%s,\"keyed\":%s,\"open\":%s", keyed && row.enabled ? "true" : "false", keyed ? "true" : "false", open ? "true" : "false");

      if (forTheAccount)
         return json + "}";

      Recovery recovery;
      const bool hasRecovery = account && GetRecovery(account->GetDomainID(), recovery);

      // Current: the domain's passphrase opens the key's recovery copy - sealed
      // to the domain's recovery key, or to the one a replacement under way
      // keeps beside it.
      Recovery opener;
      const bool current = keyed && !damaged && hasRecovery && RecoveryFor(row, recovery, opener);

      AnsiString rest;
      rest.Format(",\"damaged\":%s,\"pending\":%s,\"fingerprint\":\"%s\",\"created\":%I64d,\"changed\":%I64d,\"zero_access\":%s,\"domain_recovery\":%s,\"recovery_current\":%s}",
         damaged ? "true" : "false",
         keyed && !row.pending.IsEmpty() ? "true" : "false",
         keyed && !damaged ? MailboxCrypto::Fingerprint(row.public_key).c_str() : "",
         keyed ? row.created : (__int64) 0,
         keyed ? row.changed : (__int64) 0,
         account && account->GetZeroAccess() ? "true" : "false",
         hasRecovery ? "true" : "false",
         current ? "true" : "false");

      return json + rest;
   }

   AnsiString
   MailboxKeys::RecoveryJson(__int64 domainId)
   {
      Recovery recovery;
      if (!GetRecovery(domainId, recovery))
         return "{\"exists\":false,\"accounts\":0,\"replacement_unfinished\":false}";

      const int keyed = KeyedAccountsInDomain(domainId);

      AnsiString json;
      json.Format("{\"exists\":true,\"fingerprint\":\"%s\",\"created\":%I64d,\"accounts\":%d,\"replacement_unfinished\":%s,\"hint\":\"",
         MailboxCrypto::Fingerprint(recovery.public_key).c_str(), recovery.created, keyed < 0 ? 0 : keyed,
         recovery.previous_public.empty() ? "false" : "true");
      json += EscapeJson(Utf8(recovery.hint));
      json += "\"}";
      return json;
   }

   // ---- the rest ------------------------------------------------------------------------------

   bool
   MailboxKeys::XMLStoreDomain(XNode *parentNode, __int64 domainId)
   {
      Recovery recovery;
      if (!GetRecovery(domainId, recovery))
         return true;

      // As stored: the public half, and the private half wrapped by the
      // passphrase, which opens on any machine - the recovery key is exactly
      // what must survive the machine. Base64 and a number, and the hint; and
      // the key a replacement cut short keeps beside it, which accounts in the
      // same archive may still be sealed to.
      XNode *node = parentNode->AppendChild(_T("MailboxRecovery"));
      node->AppendAttr(_T("Public"), Base64Column(recovery.public_key));
      node->AppendAttr(_T("Sealed"), String(recovery.sealed));
      node->AppendAttr(_T("Hint"), recovery.hint);
      node->AppendAttr(_T("Created"), StringParser::IntToString(recovery.created));
      if (!recovery.previous_public.empty())
      {
         node->AppendAttr(_T("PreviousPublic"), Base64Column(recovery.previous_public));
         node->AppendAttr(_T("PreviousSealed"), String(recovery.previous_sealed));
      }
      return true;
   }

   bool
   MailboxKeys::XMLLoadDomain(XNode *parentNode, __int64 domainId)
   {
      InvalidateAll();

      XNode *node = parentNode->GetChild(_T("MailboxRecovery"));
      if (!node)
         return true;

      const std::string publicKey = Raw(node->GetAttrValue(_T("Public")));
      const String sealed = node->GetAttrValue(_T("Sealed"));
      String hint = node->GetAttrValue(_T("Hint"));
      if (hint.GetLength() > 255)
         hint = hint.Left(255);

      std::string previousPublic = Raw(node->GetAttrValue(_T("PreviousPublic")));
      String previousSealed = node->GetAttrValue(_T("PreviousSealed"));

      // Refused when it is not a key and a wrapped form this server reads: the
      // archive is the administrator's input, and a row that is not a key is
      // one no recovery can use. Whether the passphrase opens it is not a
      // thing a restore can know.
      if (publicKey.size() != (size_t) MailboxCrypto::KeyBytes || AnsiString(sealed).Find("hmk1$") != 0)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, CodeStore, "MailboxKeys::XMLLoadDomain",
            "The mailbox recovery key of a domain in the backup is not a key this server reads, and was not restored. The accounts of the domain whose keys are sealed to it cannot be recovered until a new recovery key is made and their keys sealed to it.");
         return true;
      }

      // The key beside it is kept only when it is one too.
      if (previousPublic.size() != (size_t) MailboxCrypto::KeyBytes || AnsiString(previousSealed).Find("hmk1$") != 0)
      {
         previousPublic.clear();
         previousSealed = String();
      }

      // Not while a switch-on or a replacement reads the row (RecoveryLock).
      std::lock_guard<std::mutex> recoveryGuard(RecoveryLock());

      SQLCommand remove("delete from hm_domainrecovery where drdomainid = @DOMAINID");
      remove.AddParameter("@DOMAINID", domainId);
      Application::Instance()->GetDBManager()->Execute(remove);

      SQLStatement statement;
      statement.SetTable("hm_domainrecovery");
      statement.SetStatementType(SQLStatement::STInsert);
      statement.SetIdentityColumn("drid");
      statement.AddColumnInt64("drdomainid", domainId);
      statement.AddColumn("drpublic", Base64Column(publicKey));
      statement.AddColumn("drsealed", sealed);
      statement.AddColumn("drhint", hint);
      statement.AddColumnInt64("drcreated", _ttoi64(node->GetAttrValue(_T("Created"))));
      statement.AddColumn("drprevpublic", previousPublic.empty() ? String() : Base64Column(previousPublic));
      statement.AddColumn("drprevsealed", previousSealed);

      if (!Application::Instance()->GetDBManager()->Execute(statement))
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, CodeStore, "MailboxKeys::XMLLoadDomain",
            "The mailbox recovery key of a domain in the backup could not be restored.");
      }

      return true;
   }

   void
   MailboxKeys::ForgetIndex(__int64 accountId)
   {
      // The terms and the sort's copy of the headers were built from the text
      // of mail stored before the switch, and would keep what the files no
      // longer show. The full-text indexer finds the messages again, unindexed,
      // and marks each one to be read at every search (MessageIndexer).
      SQLCommand terms("delete from hm_messageindexterms where mitaccountid = @ACCOUNTID");
      terms.AddParameter("@ACCOUNTID", accountId);
      Application::Instance()->GetDBManager()->Execute(terms);

      SQLCommand metadata("delete from hm_message_metadata where metadata_accountid = @ACCOUNTID");
      metadata.AddParameter("@ACCOUNTID", accountId);
      Application::Instance()->GetDBManager()->Execute(metadata);
   }
}
