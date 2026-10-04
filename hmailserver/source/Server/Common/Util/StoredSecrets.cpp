// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "StoredSecrets.h"
#include "MailboxCrypto.h"
#include "Crypt.h"
#include "DataProtector.h"
#include "FileUtilities.h"
#include "Unicode.h"
#include "../Application/Application.h"
#include "../Application/IniFileSettings.h"
#include "../Application/IniSettingStore.h"
#include "../SQL/DatabaseConnectionManager.h"
#include "../SQL/SQLCommand.h"
#include "../SQL/DALRecordset.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <ctime>
#include <map>
#include <mutex>
#include <string>

#ifdef HM_PLATFORM_POSIX
#include <unistd.h>
#endif

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      const char *EnvelopeTag = "HMK1:";
      const int EnvelopeTagLength = 5;
      const int KeyIdLength = 16;

      // What the machine store holds in [Security] StoredSecretsKey: a label and
      // the key in hex, so that a line that opens to anything else - the legacy
      // Blowfish reading of a value without a prefix, above all - is known not to
      // be a key.
      const char *KeyLineLabel = "HMK1KEY ";

      // The first line of a key file, and the two after it.
      const char *KeyFileTitle = "hMailServer stored secrets key";

      // The key this node holds, and the [Security] line it was opened from: the
      // line is compared at each use, so an import followed by a reinitialise is
      // taken up without anything telling this unit, and EnsureAtStart forgets it
      // outright, so that a Linux key file put back under the same line is too.
      std::mutex held_mutex;
      bool held_loaded = false;
      String held_from;
      StoredSecrets::KeyState held_state = StoredSecrets::KeyNone;
      std::string held_key;
      AnsiString held_key_id;

      // Whether this node's key is the database's: judged by EnsureAtStart at a
      // start, and by the first seal in a process that never ran it - a verb.
      enum DatabaseCheck
      {
         CheckUnknown = 0,
         CheckOk = 1,
         CheckNotTheDatabases = 2
      };
      std::atomic<int> database_check(CheckUnknown);

      // The start's application-log lines, kept until the logger is configured.
      std::mutex notes_mutex;
      bool collecting_notes = false;
      std::vector<String> notes;

      void Wipe_(AnsiString &text)
      {
         std::fill(text.begin(), text.end(), '\0');
      }

      void Wipe_(String &text)
      {
         std::fill(text.begin(), text.end(), String::value_type());
      }

      std::string BytesOfText_(const AnsiString &text)
      {
         return std::string(text.c_str(), (size_t) text.GetLength());
      }

      std::string Utf8Of_(const String &text)
      {
         AnsiString narrow;
         Unicode::WideToMultiByte(text, narrow);
         std::string bytes = BytesOfText_(narrow);
         Wipe_(narrow);
         return bytes;
      }

      String TextOfUtf8_(const std::string &bytes)
      {
         String wide;
         AnsiString narrow(bytes.c_str());
         Unicode::MultiByteToWide(narrow, wide);
         Wipe_(narrow);
         return wide;
      }

      AnsiString HexOf_(const std::string &bytes)
      {
         static const char *digits = "0123456789abcdef";
         AnsiString hex;
         hex.reserve(bytes.size() * 2);
         for (size_t i = 0; i < bytes.size(); i++)
         {
            const unsigned char value = (unsigned char) bytes[i];
            hex += digits[value >> 4];
            hex += digits[value & 0x0F];
         }
         return hex;
      }

      int HexDigitValue_(char c)
      {
         if (c >= '0' && c <= '9')
            return c - '0';
         if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
         if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
         return -1;
      }

      // The bytes of the length characters of hex at hex, read where they stand.
      bool BytesOfHex_(const char *hex, size_t length, std::string &bytes)
      {
         bytes.clear();
         if (length % 2 != 0)
            return false;

         bytes.reserve(length / 2);
         for (size_t i = 0; i < length; i += 2)
         {
            const int high = HexDigitValue_(hex[i]);
            const int low = HexDigitValue_(hex[i + 1]);
            if (high < 0 || low < 0)
            {
               MailboxCrypto::Forget(bytes);
               return false;
            }
            bytes += (char) ((high << 4) | low);
         }
         return true;
      }

      AnsiString KeyIdOf_(const std::string &key)
      {
         std::string labelled = std::string("hMailServer stored secrets key id\n") + key;
         MailboxCrypto::ForgetOnExit forgetLabelled(labelled);
         const AnsiString keyId = MailboxCrypto::Fingerprint(labelled);
         MailboxCrypto::Forget(labelled);
         return keyId;
      }

      std::string AssociatedDataOf_(const AnsiString &keyId, const String &purpose)
      {
         return std::string("hMailServer stored secret HMK1\n") + BytesOfText_(keyId) + "\n" + Utf8Of_(purpose);
      }

      bool SealWith_(const std::string &key, const AnsiString &keyId, const String &plain, const String &purpose, String &envelope)
      {
         envelope.Empty();

         const std::string nonce = MailboxCrypto::RandomBytes(MailboxCrypto::NonceBytes);
         if (nonce.size() != MailboxCrypto::NonceBytes)
            return false;

         std::string plainBytes = Utf8Of_(plain);
         MailboxCrypto::ForgetOnExit forgetPlainBytes(plainBytes);
         std::string cipher;
         std::string tag;
         const bool sealed = MailboxCrypto::Encrypt(key, nonce, AssociatedDataOf_(keyId, purpose), plainBytes, cipher, tag);
         MailboxCrypto::Forget(plainBytes);
         if (!sealed)
            return false;

         const std::string body = MailboxCrypto::Base64(nonce + cipher + tag);
         if (body.empty())
            return false;

         envelope = String(AnsiString(EnvelopeTag)) + String(keyId) + _T(":") + String(AnsiString(body.c_str()));
         return true;
      }

      // The three parts of an envelope; false when it is not one.
      bool SplitEnvelope_(const String &stored, AnsiString &keyId, std::string &nonce, std::string &cipher, std::string &tag)
      {
         if (!StoredSecrets::IsEnvelope(stored))
            return false;

         const AnsiString narrow(stored);
         if (narrow.GetLength() < EnvelopeTagLength + KeyIdLength + 2 || narrow.c_str()[EnvelopeTagLength + KeyIdLength] != ':')
            return false;

         keyId = narrow.Mid(EnvelopeTagLength, KeyIdLength);

         std::string raw;
         if (!MailboxCrypto::FromBase64(BytesOfText_(narrow.Mid(EnvelopeTagLength + KeyIdLength + 1)), raw))
            return false;
         if (raw.size() < (size_t) MailboxCrypto::NonceBytes + (size_t) MailboxCrypto::TagBytes)
            return false;

         nonce = raw.substr(0, MailboxCrypto::NonceBytes);
         tag = raw.substr(raw.size() - MailboxCrypto::TagBytes);
         cipher = raw.substr(MailboxCrypto::NonceBytes, raw.size() - MailboxCrypto::NonceBytes - MailboxCrypto::TagBytes);
         return true;
      }

      String OpenWith_(const std::string &key, const AnsiString &keyId, const String &stored, const String &purpose, bool &opened)
      {
         opened = false;

         AnsiString envelopeKeyId;
         std::string nonce;
         std::string cipher;
         std::string tag;
         if (!SplitEnvelope_(stored, envelopeKeyId, nonce, cipher, tag))
            return String();

         if (key.size() != MailboxCrypto::KeyBytes || envelopeKeyId.CompareNoCase(keyId) != 0)
            return String();

         std::string plainBytes;
         MailboxCrypto::ForgetOnExit forgetPlainBytes(plainBytes);
         if (!MailboxCrypto::Decrypt(key, nonce, AssociatedDataOf_(keyId, purpose), cipher, tag, plainBytes))
            return String();

         const String plain = TextOfUtf8_(plainBytes);
         MailboxCrypto::Forget(plainBytes);
         opened = true;
         return plain;
      }

      // The key this node holds, read again when [Security] StoredSecretsKey is not
      // what it was read from. The machine store is asked outside the lock: on
      // Linux it reads a file, and what it reports is reported from here.
      void LoadHeld_()
      {
         const String line = IniFileSettings::Instance()->GetStoredSecretsKeyProtected();

         {
            std::lock_guard<std::mutex> guard(held_mutex);
            if (held_loaded && held_from == line)
               return;
         }

         StoredSecrets::KeyState state = StoredSecrets::KeyNone;
         std::string key;
         MailboxCrypto::ForgetOnExit forgetKey(key);
         AnsiString keyId;

         if (!line.IsEmpty())
         {
            state = StoredSecrets::KeyUnopenable;

            // Only the platform store's own envelope: anything else is not a line
            // this server wrote, and the legacy Blowfish reading of it would be noise.
            if (Crypt::IsSecretEnvelope(line))
            {
               String openedWide = Crypt::Instance()->UnprotectSecret(line);
               AnsiString opened(openedWide);
               Wipe_(openedWide);

               const AnsiString label(KeyLineLabel);
               if (opened.GetLength() == label.GetLength() + 2 * MailboxCrypto::KeyBytes &&
                   opened.Left(label.GetLength()) == label)
               {
                  // The hex read where it stands in the line, which is wiped below.
                  // Until 6.3.6 it was copied out with Mid, whose copy is made from
                  // a temporary std::string that went back to the heap as it stood:
                  // the whole key in hex, each time the key was read from its line.
                  if (BytesOfHex_(opened.c_str() + label.GetLength(), (size_t) (opened.GetLength() - label.GetLength()), key) &&
                      key.size() == MailboxCrypto::KeyBytes)
                  {
                     state = StoredSecrets::KeyHeld;
                     keyId = KeyIdOf_(key);
                  }
               }

               Wipe_(opened);
            }
         }

         std::lock_guard<std::mutex> guard(held_mutex);
         MailboxCrypto::Forget(held_key);
         held_key = key;
         held_key_id = keyId;
         held_state = state;
         held_from = line;
         held_loaded = true;
         MailboxCrypto::Forget(key);
      }

      // A copy of the key, when this node holds one.
      bool CopyHeld_(std::string &key, AnsiString &keyId)
      {
         LoadHeld_();

         std::lock_guard<std::mutex> guard(held_mutex);
         if (held_state != StoredSecrets::KeyHeld)
            return false;

         key = held_key;
         keyId = held_key_id;
         return true;
      }

      String JoinIds_(const std::set<AnsiString> &keyIds)
      {
         String joined;
         for (const AnsiString &keyId : keyIds)
         {
            if (!joined.IsEmpty())
               joined += _T(", ");
            joined += String(keyId);
         }
         return joined;
      }

      // ---- the two hm_settings rows ---------------------------------------------

      bool ReadSettingRow_(const TCHAR *name, String &value, bool &exists)
      {
         value.Empty();
         exists = false;

         std::shared_ptr<DatabaseConnectionManager> database = Application::Instance()->GetDBManager();
         if (!database)
            return false;

         SQLCommand command("select settingstring from hm_settings where settingname = @SETTINGNAME");
         command.AddParameter("@SETTINGNAME", String(name));

         std::shared_ptr<DALRecordset> recordset = database->OpenRecordset(command);
         if (!recordset)
            return false;

         if (!recordset->IsEOF())
         {
            exists = true;
            value = recordset->GetStringValue("settingstring");
         }
         return true;
      }

      bool InsertSettingRow_(const TCHAR *name, const String &value)
      {
         std::shared_ptr<DatabaseConnectionManager> database = Application::Instance()->GetDBManager();
         if (!database)
            return false;

         SQLCommand command("insert into hm_settings (settingname, settingstring, settinginteger) values (@SETTINGNAME, @SETTINGSTRING, 0)");
         command.AddParameter("@SETTINGNAME", String(name));
         command.AddParameter("@SETTINGSTRING", value);
         return database->Execute(command);
      }

      bool DeleteSettingRow_(const TCHAR *name)
      {
         std::shared_ptr<DatabaseConnectionManager> database = Application::Instance()->GetDBManager();
         if (!database)
            return false;

         SQLCommand command("delete from hm_settings where settingname = @SETTINGNAME");
         command.AddParameter("@SETTINGNAME", String(name));
         return database->Execute(command);
      }

      bool DeleteSettingRowIf_(const TCHAR *name, const String &value)
      {
         std::shared_ptr<DatabaseConnectionManager> database = Application::Instance()->GetDBManager();
         if (!database)
            return false;

         SQLCommand command("delete from hm_settings where settingname = @SETTINGNAME and settingstring = @SETTINGSTRING");
         command.AddParameter("@SETTINGNAME", String(name));
         command.AddParameter("@SETTINGSTRING", value);
         return database->Execute(command);
      }

      // The database's record made to name keyId when it names none, and read
      // back: claimed is the key the database records afterwards - keyId, or the
      // key another node recorded first. hm_settings.settingname is unique on
      // SQL Server, MySQL and PostgreSQL, so of two nodes inserting at once one
      // insert fails; SQL Server Compact is a file only one machine opens. False
      // when the database could not be read or written. inserted says whether this
      // call made the row, so that a caller that cannot go on with it - the read
      // back failed, the key line cannot stay - takes it out again rather than
      // leaving a record of a key nobody holds.
      bool ClaimDatabaseKeyId_(const AnsiString &keyId, AnsiString &claimed, bool &inserted)
      {
         claimed.Empty();
         inserted = false;

         String value;
         bool exists = false;
         if (!ReadSettingRow_(StoredSecrets::KeyIdRowName(), value, exists))
            return false;

         if (!exists)
         {
            // An insert that fails is the other node's having been first, or a
            // database that cannot be written: the row read below says which.
            inserted = InsertSettingRow_(StoredSecrets::KeyIdRowName(), String(keyId));
            if (!ReadSettingRow_(StoredSecrets::KeyIdRowName(), value, exists) || !exists)
               return false;
         }

         value.Trim();
         claimed = AnsiString(value);
         return true;
      }

      // A record this call made taken out again, only while it still names keyId.
      void UnclaimDatabaseKeyId_(const AnsiString &keyId, bool inserted)
      {
         if (inserted)
            DeleteSettingRowIf_(StoredSecrets::KeyIdRowName(), String(keyId));
      }

      // Sixteen hex digits: what a key id is.
      bool IsKeyId_(const AnsiString &keyId)
      {
         if (keyId.GetLength() != KeyIdLength)
            return false;

         for (int i = 0; i < keyId.GetLength(); i++)
         {
            if (HexDigitValue_(keyId.c_str()[i]) < 0)
               return false;
         }
         return true;
      }

      // ---- the key line ----------------------------------------------------------

      // Protects the key with this machine's store and writes it into the file, over
      // the line expected and nothing else. written is the line as written.
      bool StoreKey_(const std::string &key, const String &expected, String &written, String &problem)
      {
         written.Empty();

#ifdef HM_PLATFORM_POSIX
         // Root, and no key file yet: the file DataProtector would make now would
         // be root's, mode 0600, and the service - which runs as its own user -
         // could then open nothing. The verbs make the key file the service's
         // before they come here; anything else run as root does not make a key.
         if (::geteuid() == 0 && !FileUtilities::Exists(DataProtector::GetKeyFilePath()))
         {
            problem = _T("this runs as root and the stored-secret key file does not exist yet, so a key protected now would be one the service could not read");
            return false;
         }
#endif

         // Built a character at a time, so that no temporary copy of the key in hex
         // is left behind unwiped.
         AnsiString hex = HexOf_(key);
         String line = String(AnsiString(KeyLineLabel));
         line.reserve(line.size() + hex.size());
         for (size_t i = 0; i < hex.size(); i++)
            line += (String::value_type) hex.c_str()[i];
         Wipe_(hex);

         const String inner = Crypt::Instance()->EnCrypt(line, Crypt::ETDPAPI);
         Wipe_(line);
         if (inner.IsEmpty())
         {
            problem = _T("this machine's secret store refused to protect it");
            return false;
         }

#ifdef HM_PLATFORM_POSIX
         const String protectedLine = _T("LINUX1:") + inner;
#else
         const String protectedLine = _T("DPAPI:") + inner;
#endif

         if (!IniFileSettings::Instance()->ReplaceStoredSecretsKeyProtected(expected, protectedLine, problem))
            return false;

         written = protectedLine;
         return true;
      }

      // The line put back as it was before a verb wrote one, when the database
      // would not record the key it holds. Compare-and-set as the write was.
      void RestoreKeyLine_(const String &written, const String &before)
      {
         String ignored;
         IniFileSettings::Instance()->ReplaceStoredSecretsKeyProtected(written, before, ignored);
      }

      // ---- saying so -------------------------------------------------------------

      // An error-log entry from a verb as well as from the service. Run as root - the
      // packaged Linux verbs - it goes to standard error instead: a log file root
      // made would be one the service could not append to afterwards. On Windows a
      // verb an administrator runs writes nothing to the logs at all - the log
      // folder is the service's to write (Logger::ForbidWritesForThisProcess,
      // hMailServer.cpp's ForbidServiceFolderWrites) - so it goes to standard error
      // there too, where the verb's other lines go; until this was so, an
      // administrator's --forget-sealed-secrets and --apply-environment said HM7497
      // and HM7496 nowhere at all on Windows.
      void ReportFromAnyProcess_(ErrorManager::eSeverity severity, int code, const String &source, const String &message)
      {
#ifdef HM_PLATFORM_POSIX
         if (::geteuid() == 0)
         {
            std::fprintf(stderr, "HM%d: %s\n", code, AnsiString(message).c_str());
            return;
         }
#else
         if (Logger::WritesForbidden())
         {
            const AnsiString line = Formatter::Format(_T("HM{0}: {1}\r\n"), code, message);
            HANDLE error = GetStdHandle(STD_ERROR_HANDLE);
            DWORD written = 0;
            if (error != nullptr && error != INVALID_HANDLE_VALUE)
               WriteFile(error, line.c_str(), (DWORD) line.GetLength(), &written, nullptr);
            return;
         }
#endif
         ErrorManager::Instance()->ReportError(severity, code, source, message);
      }

      // What EnsureAtStart finds wrong, said at each start and reinitialise - which
      // is where it runs, once each - until it is mended: a node that cannot open
      // what the database holds is a node whose users are locked out.
      void ReportAtStart_(const String &message)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::High, 7495, "StoredSecrets::EnsureAtStart", message);
      }

      // A process that did not run EnsureAtStart - a verb - sealing nothing, once.
      void ReportRefusedOnce_(const String &why)
      {
         static std::atomic<bool> reported(false);
         bool expected = false;
         if (!reported.compare_exchange_strong(expected, true))
            return;

         ReportFromAnyProcess_(ErrorManager::Medium, 7496, _T("StoredSecrets::Seal"),
            Formatter::Format(_T("Nothing is sealed by this process: {0}. The secrets it saves - a verb's --set-setting, --apply-environment or --apply-profile among them - are stored as written, which every node reads, ")
               _T("and a start of the service seals them where its key is the database's. Carry the database's key here - {1} on a node that holds it, {2} here - to have this node seal them itself (docs/SecretsAtRest.md). Reported once a process."),
               why, StoredSecrets::ExportVerbText(), StoredSecrets::ImportVerbText()));
      }

      // Whether this node's key is the database's, asked once by a process that
      // did not run EnsureAtStart, the first time it seals.
      bool CheckedAgainstDatabase_(const AnsiString &held)
      {
         const int check = database_check.load();
         if (check == CheckOk)
            return true;
         if (check == CheckNotTheDatabases)
            return false;

         String why;
         AnsiString recorded;
         std::set<AnsiString> keyIds;

         if (!Application::Instance()->GetDBManager())
            why = _T("the database is not open in it, so whether its stored-secrets key is the database's could not be checked");
         else if (!StoredSecrets::ReadDatabaseKeyId(recorded) || !StoredSecrets::CollectDatabaseKeyIds(keyIds))
            why = _T("the database's record of its stored-secrets key could not be read");
         else
         {
            bool isTheDatabases = true;
            if (!recorded.IsEmpty())
               isTheDatabases = recorded == held;
            else
            {
               for (const AnsiString &keyId : keyIds)
               {
                  if (keyId != held)
                     isTheDatabases = false;
               }
            }

            database_check = isTheDatabases ? CheckOk : CheckNotTheDatabases;
            if (isTheDatabases)
               return true;

            why = Formatter::Format(_T("it holds the stored-secrets key {0}, and the database's is {1}"),
               String(held), recorded.IsEmpty() ? JoinIds_(keyIds) : String(recorded));
         }

         ReportRefusedOnce_(why);
         return false;
      }

      std::string NowText_()
      {
         return std::to_string((long long) time(nullptr));
      }
   }

   bool
   StoredSecrets::IsEnvelope(const String &stored)
   {
      return stored.Left(EnvelopeTagLength) == String(AnsiString(EnvelopeTag));
   }

   AnsiString
   StoredSecrets::EnvelopeKeyId(const String &stored)
   {
      if (!IsEnvelope(stored) || stored.GetLength() < EnvelopeTagLength + KeyIdLength + 1)
         return AnsiString();

      return AnsiString(stored.Mid(EnvelopeTagLength, KeyIdLength));
   }

   String
   StoredSecrets::TotpPurpose(__int64 accountId)
   {
      return _T("hm_accounts.accounttotpsecret#") + StringParser::IntToString(accountId);
   }

   String
   StoredSecrets::SettingPurpose(const String &name)
   {
      String lower = name;
      lower.MakeLower();
      return _T("hm_inisettings.") + lower;
   }

   bool
   StoredSecrets::Seal(const String &plain, const String &purpose, String &envelope)
   {
      envelope.Empty();

      if (plain.IsEmpty())
         return false;

      std::string key;
      MailboxCrypto::ForgetOnExit forgetKey(key);
      AnsiString keyId;
      if (!CopyHeld_(key, keyId))
         return false;

      bool sealed = CheckedAgainstDatabase_(keyId) && SealWith_(key, keyId, plain, purpose, envelope);

      // What is stored is known to open: the envelope is opened again before it
      // is handed back.
      if (sealed)
      {
         bool opened = false;
         String back = OpenWith_(key, keyId, envelope, purpose, opened);
         sealed = opened && back.Compare(plain) == 0;
         Wipe_(back);
      }

      if (!sealed)
         envelope.Empty();

      MailboxCrypto::Forget(key);
      return sealed;
   }

   String
   StoredSecrets::Open(const String &stored, const String &purpose, bool &opened)
   {
      opened = true;
      if (!IsEnvelope(stored))
         return stored;

      opened = false;

      std::string key;
      MailboxCrypto::ForgetOnExit forgetKey(key);
      AnsiString keyId;
      if (!CopyHeld_(key, keyId))
         return String();

      const String plain = OpenWith_(key, keyId, stored, purpose, opened);
      MailboxCrypto::Forget(key);
      return plain;
   }

   StoredSecrets::KeyState
   StoredSecrets::State()
   {
      LoadHeld_();

      std::lock_guard<std::mutex> guard(held_mutex);
      return held_state;
   }

   AnsiString
   StoredSecrets::HeldKeyId()
   {
      LoadHeld_();

      std::lock_guard<std::mutex> guard(held_mutex);
      return held_state == KeyHeld ? held_key_id : AnsiString();
   }

   bool
   StoredSecrets::MaySeal()
   {
      const AnsiString held = HeldKeyId();
      return !held.IsEmpty() && CheckedAgainstDatabase_(held);
   }

   const TCHAR *
   StoredSecrets::KeyIdRowName()
   {
      return _T("StoredSecretsKeyId");
   }

   const TCHAR *
   StoredSecrets::ExportRowName()
   {
      return _T("StoredSecretsKeyExported");
   }

   bool
   StoredSecrets::ReadDatabaseKeyId(AnsiString &keyId)
   {
      keyId.Empty();

      String value;
      bool exists = false;
      if (!ReadSettingRow_(KeyIdRowName(), value, exists))
         return false;

      if (exists)
      {
         value.Trim();
         keyId = AnsiString(value);
      }
      return true;
   }

   bool
   StoredSecrets::CollectDatabaseKeyIds(std::set<AnsiString> &keyIds)
   {
      keyIds.clear();

      std::shared_ptr<DatabaseConnectionManager> database = Application::Instance()->GetDBManager();
      if (!database)
         return false;

      std::map<String, std::pair<String, String> > rows;
      if (!IniSettingStore::ReadAllRows(rows))
         return false;

      for (auto iter = rows.begin(); iter != rows.end(); iter++)
      {
         if (IsEnvelope((*iter).second.first))
            keyIds.insert(EnvelopeKeyId((*iter).second.first));
      }

      SQLCommand command("select accounttotpsecret from hm_accounts where accounttotpsecret like 'HMK1:%'");
      std::shared_ptr<DALRecordset> recordset = database->OpenRecordset(command);
      if (!recordset)
         return false;

      while (!recordset->IsEOF())
      {
         const String stored = recordset->GetStringValue("accounttotpsecret");
         if (IsEnvelope(stored))
            keyIds.insert(EnvelopeKeyId(stored));
         recordset->MoveNext();
      }

      return true;
   }

   bool
   StoredSecrets::ReadExportRecord(AnsiString &keyId, __int64 &when)
   {
      keyId.Empty();
      when = 0;

      String value;
      bool exists = false;
      if (!ReadSettingRow_(ExportRowName(), value, exists) || !exists)
         return false;

      // "<key id> <seconds since the epoch>"
      value.Trim();
      const int space = value.Find(_T(" "));
      if (space <= 0)
         return false;

      keyId = AnsiString(value.Left(space));
      when = _ttoi64(value.Mid(space + 1).c_str());
      return !keyId.IsEmpty();
   }

   void
   StoredSecrets::EnsureAtStart(bool protectionOn)
   {
      {
         std::lock_guard<std::mutex> guard(notes_mutex);
         notes.clear();
         collecting_notes = true;
      }

      // The key line read again whatever its text: a Linux key file put back, or
      // its mode mended, opens the very line that did not open before, and the
      // reinitialise the error below asks for has to see it.
      {
         std::lock_guard<std::mutex> guard(held_mutex);
         held_loaded = false;
      }

      database_check = CheckUnknown;

      AnsiString recorded;
      std::set<AnsiString> keyIds;
      if (!ReadDatabaseKeyId(recorded) || !CollectDatabaseKeyIds(keyIds))
      {
         // Nothing can be judged, so the first seal judges it again, and seals
         // nothing while the database cannot be read.
         return;
      }

      const KeyState state = State();

      if (state == KeyUnopenable)
      {
         database_check = CheckNotTheDatabases;
         ReportAtStart_(Formatter::Format(
            _T("[Security] StoredSecretsKey in {0} does not open on this machine: it was copied from another machine's file, or the Linux key file it was protected under is gone or unreadable. ")
            _T("Until this node holds the stored-secrets key, an account's second factor refuses every code here and the [Settings] secrets sealed under it read as not set. ")
            _T("Put the key file back, or remove the line and carry the key from a node that holds it - {1} there, {2} here - and restart or reinitialise. Said at each start until it is mended."),
            IniFileSettings::GetInitializationFile(), ExportVerbText(), ImportVerbText()));
         return;
      }

      if (state == KeyNone)
      {
         database_check = CheckNotTheDatabases;

         std::set<AnsiString> expected = keyIds;
         if (!recorded.IsEmpty())
            expected.insert(recorded);

         if (!expected.empty())
         {
            ReportAtStart_(Formatter::Format(
               _T("The database's secrets are sealed under the stored-secrets key {0}, and this node holds no key: an account's second factor refuses every code here, ")
               _T("and SRSSecret, BATVSecret, OAuth2HmacSecret and the other [Settings] secrets read as not set. Carry the key from a node that holds it - {1} there, ")
               _T("{2} here - and restart or reinitialise; no key is made here, since a second would split the database. That is the first thing to try. Only when no node holds that key any more, {3} - with the lost key's id for KEYID - clears what it sealed: ")
               _T("the second factors enrol again and the settings are entered again, after which {4} makes a new one. Said at each start until it is mended."),
               JoinIds_(expected), ExportVerbText(), ImportVerbText(), ForgetVerbText(), CreateVerbText()));
            return;
         }

         // How every installation upgraded to this version starts, until an
         // administrator makes the key: said, not raised.
         if (protectionOn)
         {
            Note(Formatter::Format(_T("StoredSecrets: this installation has no stored-secrets key, so an account's second factor and the [Settings] secrets are stored as written, as before. ")
               _T("To seal them, make one with {0}, then export it at once with {1} and keep the file with the database backups (docs/SecretsAtRest.md); the setup checklist says the same."),
               CreateVerbText(), ExportVerbText()));
         }
         return;
      }

      // Held.
      const AnsiString held = HeldKeyId();

      if (recorded.IsEmpty())
      {
         // A database that records no key: one restored from a dump taken before the
         // key was made, or sealed by a build before the record. This node's key is
         // recorded as the database's when nothing there is sealed under another.
         const bool othersOnly = !keyIds.empty() && keyIds.find(held) == keyIds.end();
         if (!othersOnly)
         {
            AnsiString claimed;
            bool inserted = false;
            if (!ClaimDatabaseKeyId_(held, claimed, inserted))
            {
               // Could not be written or read back: the first seal judges it
               // again. A row this start made names the key this node holds, and
               // stays.
               database_check = CheckUnknown;
               return;
            }

            recorded = claimed;
            if (claimed == held)
               Note(Formatter::Format(_T("StoredSecrets: the database recorded no stored-secrets key, and this node's, {0}, is now recorded as the installation's."), String(held)));
         }
      }

      if (recorded != held)
      {
         database_check = CheckNotTheDatabases;

         std::set<AnsiString> expected = keyIds;
         expected.erase(held);
         if (!recorded.IsEmpty())
            expected.insert(recorded);

         // Formatter takes five arguments at most, so the two verbs go in as one.
         const String carry = Formatter::Format(_T("{0} there, {1} here"), ExportVerbText(), ImportVerbText());
         ReportAtStart_(Formatter::Format(
            _T("This node holds the stored-secrets key {0}, and the database's secrets are sealed under {1}: they do not open here, and nothing new is sealed here, so that the database is not sealed under two keys. ")
            _T("To make the database's key this node's, remove [Security] StoredSecretsKey from {2} and carry the key from a node that holds it - {3} - then restart or reinitialise. ")
            _T("Only when no node holds that key any more, {4} - with the lost key's id for KEYID - clears what it sealed. Said at each start until it is mended."),
            String(held), JoinIds_(expected), IniFileSettings::GetInitializationFile(), carry, ForgetVerbText()));
         return;
      }

      database_check = CheckOk;

      std::set<AnsiString> others = keyIds;
      others.erase(held);
      if (!others.empty())
      {
         ReportAtStart_(Formatter::Format(
            _T("The database holds secrets sealed under the stored-secrets key {0}, which is this node's and the database's, and under {1}, which is not: those do not open here. ")
            _T("A node made its own key where it should have imported this one. Carry that key here if a node still holds it; otherwise enter those secrets again, or re-enrol those second factors - {2}, with that key's id for KEYID, clears them. Said at each start until it is mended."),
            String(held), JoinIds_(others), ForgetVerbText()));
      }
   }

   void
   StoredSecrets::Note(const String &line)
   {
      {
         std::lock_guard<std::mutex> guard(notes_mutex);
         if (collecting_notes)
         {
            if (notes.size() < 64)
               notes.push_back(line);
            return;
         }
      }

      LOG_APPLICATION(line);
   }

   void
   StoredSecrets::LogNotes()
   {
      std::vector<String> lines;
      {
         std::lock_guard<std::mutex> guard(notes_mutex);
         lines.swap(notes);
         collecting_notes = false;
      }

      for (const String &line : lines)
      {
         LOG_APPLICATION(line);
      }
   }

   bool
   StoredSecrets::CreateKey(AnsiString &keyId, bool &alreadyHeld, String &problem)
   {
      keyId.Empty();
      alreadyHeld = false;
      problem.Empty();

      if (!Application::Instance()->GetDBManager())
      {
         problem = _T("the database is not open, and a key is recorded there as it is made");
         return false;
      }

      AnsiString recorded;
      std::set<AnsiString> keyIds;
      if (!ReadDatabaseKeyId(recorded) || !CollectDatabaseKeyIds(keyIds))
      {
         problem = _T("the database's record of its stored-secrets key could not be read");
         return false;
      }

      std::set<AnsiString> expected = keyIds;
      if (!recorded.IsEmpty())
         expected.insert(recorded);

      const String file = IniFileSettings::GetInitializationFile();
      const KeyState state = State();

      if (state == KeyUnopenable)
      {
         problem = Formatter::Format(_T("[Security] StoredSecretsKey in {0} does not open on this machine. If it is the database's key, put back what it was protected under, or import the key from a node that holds it; otherwise remove the line first"), file);
         return false;
      }

      if (state == KeyHeld)
      {
         const AnsiString held = HeldKeyId();
         const bool theDatabases = recorded.IsEmpty()
            ? (keyIds.empty() || (keyIds.size() == 1 && keyIds.find(held) != keyIds.end()))
            : recorded == held;

         if (!theDatabases)
         {
            problem = Formatter::Format(_T("this node holds the stored-secrets key {0}, and the database's is {1}. To make that key this node's, remove [Security] StoredSecretsKey from {2} and import it - {3} there, {4} here"),
               String(held), JoinIds_(expected), file, ExportVerbText(), ImportVerbText());
            return false;
         }

         if (recorded.IsEmpty())
         {
            AnsiString claimed;
            bool inserted = false;
            if (!ClaimDatabaseKeyId_(held, claimed, inserted) || claimed != held)
            {
               problem = claimed.IsEmpty()
                  ? String(_T("the database could not record this node's key"))
                  : Formatter::Format(_T("another node recorded the stored-secrets key {0} as the database's; import that one"), String(claimed));
               return false;
            }
         }

         keyId = held;
         alreadyHeld = true;
         return true;
      }

      // None held. A key the database records, or one its values are sealed under,
      // is imported, never replaced: a second would split the database.
      if (!expected.empty())
      {
         problem = Formatter::Format(_T("the database's secrets are sealed under the stored-secrets key {0}, which this node does not hold. Import it from a node that holds it - {1} there, {2} here - rather than making another. ")
            _T("Only when no node holds it any more, {3} - with the lost key's id for KEYID - clears what it sealed, after which a key can be made"),
            JoinIds_(expected), ExportVerbText(), ImportVerbText(), ForgetVerbText());
         return false;
      }

      std::string key = MailboxCrypto::RandomBytes(MailboxCrypto::KeyBytes);
      MailboxCrypto::ForgetOnExit forgetKey(key);
      if (key.size() != MailboxCrypto::KeyBytes)
      {
         MailboxCrypto::Forget(key);
         problem = _T("the random number generator failed");
         return false;
      }

      const AnsiString made = KeyIdOf_(key);

      // The file first, flushed to the disk, then the record: a crash between the
      // two leaves a key the database does not record yet, which the next start
      // records, never a record of a key nobody holds.
      const String before = IniFileSettings::Instance()->GetStoredSecretsKeyProtected();
      String written;
      const bool stored = StoreKey_(key, before, written, problem);
      MailboxCrypto::Forget(key);
      if (!stored)
         return false;

      if (HeldKeyId() != made)
      {
         RestoreKeyLine_(written, before);
         problem = _T("the key written could not be read back");
         return false;
      }

      AnsiString claimed;
      bool inserted = false;
      if (!ClaimDatabaseKeyId_(made, claimed, inserted) || claimed != made)
      {
         // The record first, then the line: a record of a key whose line is gone
         // would be a record of a key nobody holds.
         UnclaimDatabaseKeyId_(made, inserted);
         RestoreKeyLine_(written, before);
         problem = claimed.IsEmpty()
            ? String(_T("the database could not record the new key, so it was taken out of the file again"))
            : Formatter::Format(_T("another node recorded the stored-secrets key {0} as the database's while this one was being made, so it was taken out of the file again; import that one - {1} there, {2} here"),
                 String(claimed), ExportVerbText(), ImportVerbText());
         return false;
      }

      keyId = made;
      return true;
   }

   bool
   StoredSecrets::ExportText(AnsiString &text, AnsiString &keyId, String &problem)
   {
      text.Empty();
      keyId.Empty();

      std::string key;
      MailboxCrypto::ForgetOnExit forgetKey(key);
      if (!CopyHeld_(key, keyId))
      {
         problem = State() == KeyUnopenable
            ? Formatter::Format(_T("[Security] StoredSecretsKey in {0} does not open on this machine"), IniFileSettings::GetInitializationFile())
            : Formatter::Format(_T("{0} has no [Security] StoredSecretsKey; make one with {1}"), IniFileSettings::GetInitializationFile(), CreateVerbText());
         return false;
      }

      // A key the database does not record is not the one to carry: every other
      // node would then hold the wrong key.
      if (Application::Instance()->GetDBManager())
      {
         AnsiString recorded;
         if (ReadDatabaseKeyId(recorded) && !recorded.IsEmpty() && recorded != keyId)
         {
            MailboxCrypto::Forget(key);
            problem = Formatter::Format(_T("this node holds the stored-secrets key {0}, which is not the one the database records, {1}; export that one from a node that holds it"),
               String(keyId), String(recorded));
            keyId.Empty();
            return false;
         }
      }

      // Appended into one buffer reserved up front, so that no temporary copy of
      // the key in base64 is left behind; the caller wipes the text.
      std::string base64 = MailboxCrypto::Base64(key);
      MailboxCrypto::ForgetOnExit forgetBase64(base64);
      MailboxCrypto::Forget(key);

      text.reserve(128 + base64.size());
      text += KeyFileTitle;
      text += "\r\nid ";
      text += keyId;
      text += "\r\nkey ";
      text.append(base64.c_str(), base64.size());
      text += "\r\n";
      MailboxCrypto::Forget(base64);
      return true;
   }

   bool
   StoredSecrets::RecordExport(const AnsiString &keyId)
   {
      if (!Application::Instance()->GetDBManager())
         return false;

      DeleteSettingRow_(ExportRowName());
      return InsertSettingRow_(ExportRowName(), String(keyId) + _T(" ") + String(AnsiString(NowText_().c_str())));
   }

   bool
   StoredSecrets::ImportText(const AnsiString &text, AnsiString &keyId, String &problem)
   {
      keyId.Empty();
      problem.Empty();

      // Three lines, CRLF or LF, a byte order mark and trailing blanks forgiven.
      // Every line is written over however this scope ends: the key's line is
      // the key in base64, and until 6.3.6 an exception between reading the
      // lines and the Forget of each below - an allocation's, a logger's -
      // freed them as they were.
      //
      // And no buffer that held a piece of the key is given back to the heap
      // unwritten on the way: the line being read and the vector of lines are
      // each given, before the first character, room for all they can come to,
      // so that neither grows - a string that grows copies itself into a larger
      // buffer and frees the old one as it stood, which for the key's line was
      // most of its base64 (26 of its 44 characters with libstdc++, 43 with
      // MSVC's library) - and the byte order mark is stepped over rather than
      // erased, which moved the text down and left its last three bytes past
      // the end Forget writes over.
      std::vector<std::string> lines;
      MailboxCrypto::ForgetEachOnExit forgetLines(lines);
      {
         std::string all = BytesOfText_(text);
         MailboxCrypto::ForgetOnExit forgetAll(all);
         size_t first = 0;
         if (all.size() >= 3 && (unsigned char) all[0] == 0xEF && (unsigned char) all[1] == 0xBB && (unsigned char) all[2] == 0xBF)
            first = 3;

         lines.reserve((size_t) std::count(all.begin(), all.end(), '\n') + 1);

         std::string current;
         MailboxCrypto::ForgetOnExit forgetCurrent(current);
         current.reserve(all.size());
         for (size_t i = first; i <= all.size(); i++)
         {
            if (i == all.size() || all[i] == '\n')
            {
               while (!current.empty() && (current.back() == '\r' || current.back() == ' ' || current.back() == '\t'))
                  current.pop_back();
               if (!current.empty())
                  lines.push_back(current);
               MailboxCrypto::Forget(current);
               current.clear();
               continue;
            }
            current += all[i];
         }
         MailboxCrypto::Forget(all);
      }

      std::string key;
      MailboxCrypto::ForgetOnExit forgetKey(key);
      // The base64 is copied out of its line into a string that is watched too:
      // the temporary substr it was read from before was freed unwritten.
      std::string encoded;
      MailboxCrypto::ForgetOnExit forgetEncoded(encoded);
      bool wellFormed = lines.size() == 3 && lines[0] == KeyFileTitle &&
                        lines[1].size() == 3 + (size_t) KeyIdLength && lines[1].compare(0, 3, "id ") == 0 &&
                        lines[2].compare(0, 4, "key ") == 0;
      if (wellFormed)
      {
         encoded.assign(lines[2], 4, std::string::npos);
         wellFormed = MailboxCrypto::FromBase64(encoded, key) && key.size() == MailboxCrypto::KeyBytes;
      }
      MailboxCrypto::Forget(encoded);

      const AnsiString named(wellFormed ? lines[1].substr(3).c_str() : "");

      for (std::string &line : lines)
         MailboxCrypto::Forget(line);

      if (!wellFormed)
      {
         MailboxCrypto::Forget(key);
         problem = _T("the file is not a stored-secrets key that --export-stored-secrets-key wrote");
         return false;
      }

      // The id line is held to the key: a file whose key was changed, even by a
      // byte, is refused here rather than found out when nothing opens.
      const AnsiString computed = KeyIdOf_(key);
      if (named.CompareNoCase(computed) != 0)
      {
         MailboxCrypto::Forget(key);
         problem = _T("the key in the file is not the key its id line names: the file is damaged");
         return false;
      }

      // And to the database: the key it records is the one every node holds.
      if (!Application::Instance()->GetDBManager())
      {
         MailboxCrypto::Forget(key);
         problem = _T("the database is not open, and the key is checked against the one it records");
         return false;
      }

      AnsiString recorded;
      std::set<AnsiString> keyIds;
      if (!ReadDatabaseKeyId(recorded) || !CollectDatabaseKeyIds(keyIds))
      {
         MailboxCrypto::Forget(key);
         problem = _T("the database's record of its stored-secrets key could not be read");
         return false;
      }

      if (!recorded.IsEmpty() && recorded != computed)
      {
         MailboxCrypto::Forget(key);
         problem = Formatter::Format(_T("the file holds the stored-secrets key {0}, and the database records {1}: import that one, from a node that holds it"),
            String(computed), String(recorded));
         return false;
      }

      if (recorded.IsEmpty() && !keyIds.empty() && keyIds.find(computed) == keyIds.end())
      {
         MailboxCrypto::Forget(key);
         problem = Formatter::Format(_T("the file holds the stored-secrets key {0}, and the database's secrets are sealed under {1}: import that one, from a node that holds it"),
            String(computed), JoinIds_(keyIds));
         return false;
      }

      const KeyState state = State();
      if (state == KeyHeld)
      {
         const AnsiString held = HeldKeyId();
         MailboxCrypto::Forget(key);

         if (held != computed)
         {
            problem = Formatter::Format(_T("this node holds another stored-secrets key, {0}. To replace it - after which what is sealed under it no longer opens here - remove [Security] StoredSecretsKey from {1} and run this again"),
               String(held), IniFileSettings::GetInitializationFile());
            return false;
         }

         if (recorded.IsEmpty())
         {
            AnsiString claimed;
            bool inserted = false;
            if (!ClaimDatabaseKeyId_(computed, claimed, inserted) || claimed != computed)
            {
               problem = claimed.IsEmpty()
                  ? String(_T("the database could not record the key"))
                  : Formatter::Format(_T("another node recorded the stored-secrets key {0} as the database's; import that one"), String(claimed));
               return false;
            }
         }

         keyId = computed;
         return true;
      }

      // A line that does not open is written over: it was another machine's, and
      // this is how the key reaches this one.
      const String before = IniFileSettings::Instance()->GetStoredSecretsKeyProtected();
      String written;
      const bool stored = StoreKey_(key, before, written, problem);
      MailboxCrypto::Forget(key);
      if (!stored)
         return false;

      if (HeldKeyId() != computed)
      {
         RestoreKeyLine_(written, before);
         problem = _T("the key written could not be read back");
         return false;
      }

      if (recorded.IsEmpty())
      {
         AnsiString claimed;
         bool inserted = false;
         if (!ClaimDatabaseKeyId_(computed, claimed, inserted) || claimed != computed)
         {
            UnclaimDatabaseKeyId_(computed, inserted);
            RestoreKeyLine_(written, before);
            problem = claimed.IsEmpty()
               ? String(_T("the database could not record the key, so it was taken out of the file again"))
               : Formatter::Format(_T("another node recorded the stored-secrets key {0} as the database's meanwhile, so this one was taken out of the file again; import that one"), String(claimed));
            return false;
         }
      }

      keyId = computed;
      return true;
   }

   bool
   StoredSecrets::ForgetLostKey(const AnsiString &lostKeyId, std::vector<String> &forgotten, String &problem)
   {
      forgotten.clear();
      problem.Empty();

      AnsiString lost = lostKeyId;
      lost.Trim();
      lost.MakeLower();
      if (!IsKeyId_(lost))
      {
         problem = _T("--forget-sealed-secrets needs the lost key's id - the sixteen hex digits HM7495 and the setup checklist name - so that nothing sealed under another key is touched");
         return false;
      }

      std::shared_ptr<DatabaseConnectionManager> database = Application::Instance()->GetDBManager();
      if (!database)
      {
         problem = _T("the database is not open");
         return false;
      }

      const String file = IniFileSettings::GetInitializationFile();

      // A key line that does not open here - its key file missing, its mode wrong,
      // the line copied from another machine - may be the very key named: this
      // node cannot tell what it holds from what is lost.
      const KeyState state = State();
      if (state == KeyUnopenable)
      {
         problem = Formatter::Format(_T("[Security] StoredSecretsKey in {0} does not open on this machine, so whether this node holds the key {1} cannot be told. ")
            _T("Put back what the line was protected under - the key file, its mode 0600 - or, if it is another machine's line, remove it; then run this again"), file, String(lost));
         return false;
      }

      // Nothing sealed under a key this node holds is lost: it is carried.
      if (state == KeyHeld && HeldKeyId().CompareNoCase(lost) == 0)
      {
         problem = Formatter::Format(_T("this node holds the stored-secrets key {0}, so nothing sealed under it is lost. Carry it to the node that lacks it - {1} here, {2} there - instead"),
            String(lost), ExportVerbText(), ImportVerbText());
         return false;
      }

      bool complete = true;

      // The [Settings] secrets sealed under that key: emptied, in the row and in the
      // file's line.
      std::map<String, std::pair<String, String> > rows;
      if (!IniSettingStore::ReadAllRows(rows))
      {
         problem = _T("the settings could not be read");
         return false;
      }

      for (auto iter = rows.begin(); iter != rows.end(); iter++)
      {
         const String &name = (*iter).first;
         const String &stored = (*iter).second.first;
         if (!IsEnvelope(stored) || EnvelopeKeyId(stored).CompareNoCase(lost) != 0)
            continue;

         if (IniSettingStore::ForgetSealedValue(name, stored))
         {
            forgotten.push_back(Formatter::Format(_T("[Settings] {0}: emptied, in the database and in {1}. It was sealed under the stored-secrets key {2}. Enter it again."),
               name, file, String(lost)));
         }
         else
         {
            complete = false;
            forgotten.push_back(Formatter::Format(_T("[Settings] {0}: could not be emptied."), name));
         }
      }

      // The accounts' second factors sealed under that key: removed, so that each
      // account enrols again.
      struct SealedFactor
      {
         __int64 id = 0;
         String address;
         String stored;
      };
      std::vector<SealedFactor> factors;
      {
         SQLCommand select("select accountid, accountaddress, accounttotpsecret from hm_accounts where accounttotpsecret like 'HMK1:%'");
         std::shared_ptr<DALRecordset> recordset = database->OpenRecordset(select);
         if (!recordset)
         {
            problem = _T("the accounts could not be read");
            return false;
         }

         while (!recordset->IsEOF())
         {
            SealedFactor factor;
            factor.id = recordset->GetInt64Value("accountid");
            factor.address = recordset->GetStringValue("accountaddress");
            factor.stored = recordset->GetStringValue("accounttotpsecret");
            if (EnvelopeKeyId(factor.stored).CompareNoCase(lost) == 0)
               factors.push_back(factor);
            recordset->MoveNext();
         }
      }

      for (const SealedFactor &factor : factors)
      {
         // Only the row as it was read.
         SQLCommand update("update hm_accounts set accounttotpsecret = '' where accountid = @ACCOUNTID and accounttotpsecret = @WAS");
         update.AddParameter("@ACCOUNTID", factor.id);
         update.AddParameter("@WAS", factor.stored);

         if (database->Execute(update))
         {
            forgotten.push_back(Formatter::Format(_T("{0}: second factor removed. It was sealed under the stored-secrets key {1}. The account signs in with its password and enrols again."),
               factor.address, String(lost)));
         }
         else
         {
            complete = false;
            forgotten.push_back(Formatter::Format(_T("{0}: second factor could not be removed."), factor.address));
         }
      }

      // The database's record, and the export's, only where they name that key. No
      // other key is recorded here: a node that holds one and finds the database
      // recording none records it at its next start, as it would on a database
      // restored from before the key.
      AnsiString recorded;
      if (!ReadDatabaseKeyId(recorded))
      {
         problem = _T("the database's record of its stored-secrets key could not be read");
         return false;
      }

      if (recorded.CompareNoCase(lost) == 0)
      {
         if (DeleteSettingRowIf_(KeyIdRowName(), String(recorded)))
            forgotten.push_back(Formatter::Format(_T("The database's record of the stored-secrets key {0} was removed; a key can be made, or imported, now."), String(lost)));
         else
         {
            complete = false;
            forgotten.push_back(Formatter::Format(_T("The database's record of the stored-secrets key {0} could not be removed."), String(lost)));
         }
      }

      AnsiString exportedId;
      __int64 exportedAt = 0;
      if (ReadExportRecord(exportedId, exportedAt) && exportedId.CompareNoCase(lost) == 0)
         DeleteSettingRow_(ExportRowName());

      // Logged where the errors that led here were: the error log, whatever the
      // application log is set to, since a verb's process has no logger configured
      // - or standard error, where a verb writes no log (ReportFromAnyProcess_).
      // Every item, in one entry. An administrator asked for it, and it is the
      // record that the values are gone.
      if (!forgotten.empty())
      {
         ReportFromAnyProcess_(ErrorManager::Medium, 7497, _T("StoredSecrets::ForgetLostKey"),
            Formatter::Format(_T("--forget-sealed-secrets {0} cleared what was sealed under that lost stored-secrets key (docs/SecretsAtRest.md): {1}"),
               String(lost), StringParser::JoinVector(forgotten, _T(" "))));
      }

      if (!complete)
         problem = _T("some of the values could not be cleared; run it again");

      return complete;
   }

   String
   StoredSecrets::CreateVerbText()
   {
#ifdef HM_PLATFORM_POSIX
      return _T("sudo hmailserver --create-stored-secrets-key");
#else
      return _T("hMailServer.exe --create-stored-secrets-key, from an elevated prompt");
#endif
   }

   String
   StoredSecrets::ExportVerbText()
   {
#ifdef HM_PLATFORM_POSIX
      return _T("sudo hmailserver --export-stored-secrets-key FILE");
#else
      return _T("hMailServer.exe --export-stored-secrets-key FILE, from an elevated prompt");
#endif
   }

   String
   StoredSecrets::ImportVerbText()
   {
#ifdef HM_PLATFORM_POSIX
      return _T("sudo hmailserver --import-stored-secrets-key FILE");
#else
      return _T("hMailServer.exe --import-stored-secrets-key FILE, from an elevated prompt");
#endif
   }

   String
   StoredSecrets::ForgetVerbText()
   {
#ifdef HM_PLATFORM_POSIX
      return _T("sudo hmailserver --forget-sealed-secrets KEYID, with the service stopped");
#else
      return _T("hMailServer.exe --forget-sealed-secrets KEYID, from an elevated prompt with the service stopped");
#endif
   }

   void
   StoredSecrets::Test()
   {
      const std::string key = MailboxCrypto::RandomBytes(MailboxCrypto::KeyBytes);
      if (key.size() != MailboxCrypto::KeyBytes)
         throw 0;
      const AnsiString keyId = KeyIdOf_(key);
      const String purpose = SettingPurpose(_T("SRSSecret"));
      const String plain = _T("a secret, 2FA and all");

      String envelope;
      if (!SealWith_(key, keyId, plain, purpose, envelope))
         throw 0;
      if (!IsEnvelope(envelope) || EnvelopeKeyId(envelope) != keyId || envelope.Find(plain) >= 0)
         throw 0;

      bool opened = false;
      if (OpenWith_(key, keyId, envelope, purpose, opened) != plain || !opened)
         throw 0;

      // Two seals of one value differ: the nonce is fresh.
      String second;
      if (!SealWith_(key, keyId, plain, purpose, second) || second == envelope)
         throw 0;

      // Another purpose - the same value copied into another setting's place.
      OpenWith_(key, keyId, envelope, SettingPurpose(_T("BATVSecret")), opened);
      if (opened)
         throw 0;

      // One account's second factor copied into another account's row.
      String factor;
      if (!SealWith_(key, keyId, plain, TotpPurpose(1), factor))
         throw 0;
      if (OpenWith_(key, keyId, factor, TotpPurpose(1), opened) != plain || !opened)
         throw 0;
      OpenWith_(key, keyId, factor, TotpPurpose(2), opened);
      if (opened)
         throw 0;

      // Another key under the same id, and the right key under another id.
      const std::string other = MailboxCrypto::RandomBytes(MailboxCrypto::KeyBytes);
      OpenWith_(other, keyId, envelope, purpose, opened);
      if (opened)
         throw 0;
      OpenWith_(key, KeyIdOf_(other), envelope, purpose, opened);
      if (opened)
         throw 0;

      // A changed byte in the body.
      String changed = envelope;
      const int at = changed.GetLength() - 6;
      changed.SetAt(at, changed.GetAt(at) == 'A' ? 'B' : 'A');
      OpenWith_(key, keyId, changed, purpose, opened);
      if (opened)
         throw 0;

      // Not an envelope: returned as it is, opened.
      if (Open(_T("stored as written"), purpose, opened) != _T("stored as written") || !opened)
         throw 0;
      if (IsEnvelope(_T("DPAPI:AQAAAN")) || IsEnvelope(_T("")))
         throw 0;
   }
}
