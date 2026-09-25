// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"
#include "IniSettingStore.h"

#include "IniFileSettings.h"
#include "IniSettingsBaseline.h"
#include "../SQL/SQLCommand.h"
#include "../SQL/SQLStatement.h"
#include "../SQL/DALRecordset.h"
#include "../Util/XMLite.h"
#include "../Util/AuditTrail.h"
#include "../Util/StoredSecrets.h"
#include "EnvironmentSettings.h"

#include <atomic>

// The Win32 profile API - GetPrivateProfileString and the three calls beside
// it - lives in kernel32 and has no POSIX equivalent, so the POSIX build
// declares those four functions here and implements them over the INI file
// itself in Common/Util/IniFile.cpp. The Windows build never reaches this line
// and binds the same calls to <windows.h> as it always has.
#ifdef HM_PLATFORM_POSIX
#include "../Util/IniFile.h"
#endif

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   IniSettingStore::IniSettingStore()
   {
   }

   IniSettingStore::~IniSettingStore()
   {
   }

   void
   IniSettingStore::ReadIniSection_(const String &section, std::map<String, String> &values)
   {
      values.clear();

      String iniFile = IniFileSettings::GetInitializationFile();
      if (iniFile.IsEmpty())
         return;

      // Read the whole section in one call, growing the buffer while the result
      // looks truncated. Same shape as RateLimiter::LoadSettings_, which reads its
      // own overrides section: GetPrivateProfileSection reports "copied" one or two
      // short of the buffer when it has run out of room rather than failing, so the
      // only reliable test is that it came back short of the size that was offered.
      std::vector<TCHAR> buffer;
      DWORD size = 16384;
      DWORD copied = 0;

      for (int attempt = 0; attempt < 6; attempt++)
      {
         buffer.assign(size, _T('\0'));
         copied = GetPrivateProfileSection(section.c_str(), &buffer[0], size, iniFile.c_str());

         if (copied < size - 2)
            break;

         size *= 4;
      }

      if (copied == 0)
         return;

      const TCHAR *entry = &buffer[0];
      while (*entry != 0)
      {
         String line = entry;
         entry += line.GetLength() + 1;

         // A comment line has no place in the table: it is not a setting, and
         // copying it in would create a row whose name is not a key.
         String trimmed = line;
         trimmed.Trim();
         if (trimmed.IsEmpty() || trimmed.StartsWith(_T(";")) || trimmed.StartsWith(_T("#")))
            continue;

         int separator = trimmed.Find(_T("="));
         if (separator <= 0)
            continue;

         String name = trimmed.Mid(0, separator);
         String value = trimmed.Mid(separator + 1);

         name.Trim();

         // The value is NOT trimmed of trailing space here beyond what the section
         // read already did. GetPrivateProfileString, which is what every reader
         // uses, returns the value with surrounding whitespace as the file has it,
         // and a mirror that trims would disagree with the reader it mirrors.
         if (name.IsEmpty())
            continue;

         // The column is 100 characters. A longer key cannot be stored, and
         // truncating it would create a row that matches the wrong setting - so it
         // is skipped and named, and that setting simply stays ini-only.
         if (name.GetLength() > 100)
         {
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5800, "IniSettingStore::ReadIniSection_",
               Formatter::Format("The hMailServer.INI setting '{0}' in [{1}] has a name longer than 100 characters, so it cannot be stored in the database, which is the settings store. Shorten the name or remove the setting.", name, section));
            continue;
         }

         if (value.GetLength() > 4000)
         {
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5801, "IniSettingStore::ReadIniSection_",
               Formatter::Format("The hMailServer.INI setting '{0}' in [{1}] has a value longer than 4000 characters, so it cannot be stored in the database, which is the settings store.", name, section));
            continue;
         }

         values[name] = value;
      }
   }

   /// <summary>
   /// The database manager, or an empty pointer when there is none.
   ///
   /// There is none on a database this build refused - a schema version it does not
   /// know, a connection that could not be made - and the COM layer keeps answering
   /// on such a server, with the refusal, rather than disappearing. Settings are the
   /// first thing anything asks it for, so every statement below goes through this
   /// and a null one is a false return rather than an access violation. HM5011 has a
   /// fixture of its own and this is the path it walks.
   /// </summary>
   static std::shared_ptr<DatabaseConnectionManager> StoreDatabase_()
   {
      return Application::Instance()->GetDBManager();
   }

   /// <summary>
   /// Says, once a process for each name, that a protected setting's envelope does
   /// not open here. Every reader then runs as though the secret were not set, and a
   /// start, a reinitialise and every save read it again - so without the once the
   /// same sentence would be written at each of them.
   /// </summary>
   static void ReportUnopenedSecretOnce_(const String &name, const AnsiString &keyId)
   {
      static boost::mutex reportedMutex;
      static std::set<String> reported;

      {
         boost::lock_guard<boost::mutex> guard(reportedMutex);
         if (!reported.insert(name).second)
            return;
      }

      ErrorManager::Instance()->ReportError(ErrorManager::High, 7492, "IniSettingStore::OpenStored",
         Formatter::Format("The setting {0} is sealed under the stored-secrets key {1}, which this node does not hold (Util/StoredSecrets.h), so the server runs as though it were not set. "
            "Carry the key from a node that holds it - {2} there, {3} here - and restart or reinitialise; or enter the setting again. "
            "Only when no node holds that key any more, {4} - with {1} for KEYID - clears it and every other value sealed under it (docs/SecretsAtRest.md). Reported once a process.",
            name, String(keyId), StoredSecrets::ExportVerbText(), StoredSecrets::ImportVerbText(), StoredSecrets::ForgetVerbText()));
   }

   /// <summary>
   /// Says, once a process for each name, that a secret was saved as written because
   /// its sealed form would not fit the 4,000 characters a setting may have.
   /// </summary>
   static void ReportTooLongOnce_(const String &name, int length)
   {
      static boost::mutex reportedMutex;
      static std::set<String> reported;

      {
         boost::lock_guard<boost::mutex> guard(reportedMutex);
         if (!reported.insert(name).second)
            return;
      }

      ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7491, "IniSettingStore::WriteSetting",
         Formatter::Format("The setting {0} holds a secret of {1} characters, whose sealed form would be longer than the 4,000 characters a setting may have, so it was stored as written, as it was before secrets were sealed. "
            "A secret of up to about 2,900 characters is sealed (docs/SecretsAtRest.md). Reported once a process.", name, StringParser::IntToString(length)));
   }

   bool
   IniSettingStore::ReadAllRows(std::map<String, std::pair<String, String> > &rows)
   {
      rows.clear();

      std::shared_ptr<DatabaseConnectionManager> database = StoreDatabase_();

      if (!database)
         return false;

      SQLCommand command("select inisettingname, inisettingvalue, inisettingfilevalue from hm_inisettings");

      std::shared_ptr<DALRecordset> recordset = database->OpenRecordset(command);

      if (!recordset)
         return false;

      while (!recordset->IsEOF())
      {
         String name = recordset->GetStringValue("inisettingname");
         String value = recordset->GetStringValue("inisettingvalue");
         String fileValue = recordset->GetStringValue("inisettingfilevalue");

         if (!name.IsEmpty())
            rows[name] = std::make_pair(value, fileValue);

         recordset->MoveNext();
      }

      return true;
   }

   bool
   IniSettingStore::InsertRow_(const String &name, const String &value, const String &fileValue)
   {
      // Parameters rather than a formatted statement: these names and values come
      // from a file an administrator edits, so they are not trusted input even
      // though they are local.
      std::shared_ptr<DatabaseConnectionManager> database = StoreDatabase_();

      if (!database)
         return false;

      SQLStatement statement(SQLStatement::STInsert, "hm_inisettings");
      statement.AddColumn("inisettingname", name);
      statement.AddColumn("inisettingvalue", value);
      statement.AddColumn("inisettingfilevalue", fileValue);

      return database->Execute(statement);
   }

   bool
   IniSettingStore::UpdateRow_(const String &name, const String &value, const String &fileValue)
   {
      std::shared_ptr<DatabaseConnectionManager> database = StoreDatabase_();

      if (!database)
         return false;

      SQLCommand command("update hm_inisettings set inisettingvalue = @VALUE, inisettingfilevalue = @FILEVALUE where inisettingname = @NAME");
      command.AddParameter("@VALUE", value);
      command.AddParameter("@FILEVALUE", fileValue);
      command.AddParameter("@NAME", name);

      return database->Execute(command);
   }

   bool
   IniSettingStore::DeleteRow_(const String &name)
   {
      std::shared_ptr<DatabaseConnectionManager> database = StoreDatabase_();

      if (!database)
         return false;

      SQLCommand command("delete from hm_inisettings where inisettingname = @NAME");
      command.AddParameter("@NAME", name);

      return database->Execute(command);
   }

   bool
   IniSettingStore::ReadRow_(const String &name, String &value)
   {
      value = String();

      std::shared_ptr<DatabaseConnectionManager> database = StoreDatabase_();

      if (!database)
         return false;

      // The same comparison DeleteRow_ makes, so that the row read here is the row
      // the delete then removes - on every backend, PostgreSQL's case-sensitive
      // one included.
      SQLCommand command("select inisettingvalue from hm_inisettings where inisettingname = @NAME");
      command.AddParameter("@NAME", name);

      std::shared_ptr<DALRecordset> recordset = database->OpenRecordset(command);

      if (!recordset || recordset->IsEOF())
         return false;

      value = recordset->GetStringValue("inisettingvalue");
      return true;
   }

   bool
   IniSettingStore::ReadFileValue_(const String &name, String &value)
   {
      value = String();

      String iniFile = IniFileSettings::GetInitializationFile();

      if (iniFile.IsEmpty())
         return false;

      // Read the way XMLLoad reads the same key. An empty line and no line are the
      // same thing to a pepper, so either is "no value".
      const DWORD bufferSize = 4096;
      TCHAR current[bufferSize];
      GetPrivateProfileString(_T("Settings"), name.c_str(), _T(""), current, bufferSize, iniFile.c_str());

      value = String(current);
      return !value.IsEmpty();
   }

   bool
   IniSettingStore::FileLine_(const String &name, String &value)
   {
      value = String();

      String iniFile = IniFileSettings::GetInitializationFile();
      if (iniFile.IsEmpty())
         return false;

      // A default no line can hold - a control character the profile API keeps
      // as it is - tells a missing key from an empty one.
      const String absent = _T("\x01hm-no-line\x01");
      const DWORD bufferSize = 4096;
      TCHAR current[bufferSize];
      GetPrivateProfileString(_T("Settings"), name.c_str(), absent.c_str(), current, bufferSize, iniFile.c_str());

      if (absent.Compare(String(current)) == 0)
         return false;

      value = String(current);
      return true;
   }

   void
   IniSettingStore::NoteReadOnlyFile_()
   {
      static std::atomic<bool> said(false);
      if (said.exchange(true))
         return;

      LOG_APPLICATION(Formatter::Format(_T("IniSettingStore: {0} is read-only to the account the service runs as, which is how a service account of its own is meant to run - NT SERVICE\\hMailServer on Windows, the packages' hmailserver user on Linux (docs/ServiceAccount.md). Settings changed from now on are stored in the database, which is the settings store, and are in use; the [Settings] copy in the file is not refreshed, and says nothing about what the server is running on."),
         IniFileSettings::GetInitializationFile()));
   }

   bool
   IniSettingStore::IsWithheld_(const String &name)
   {
      // Asked only for an administrator's change, the one the audit trail records
      // and the one that makes a value a file supplied the administrator's.
      if (!AuditTrail::HasActor())
         return false;

      return EnvironmentSettings::IsWithheld(EnvironmentSettings::StoreIni, name);
   }

   void
   IniSettingStore::RecordChange_(const String &name, bool wasStored, const String &previousValue, bool isStored, const String &value, bool withheld)
   {
      if (!wasStored && !isStored)
         return;

      if (wasStored && isStored && previousValue.Compare(value) == 0)
         return;

      // A value a file of the environment supplied is recorded as changed, as a
      // secret's is, and the value now stored - written, or the default - is the
      // administrator's, shown as any other from here on.
      if (withheld)
         EnvironmentSettings::ForgetSecretOrigin(EnvironmentSettings::StoreIni, name);

      // A side with no row is the setting at its default, which is what an
      // administrator reading the trail needs to be told - not an empty string,
      // which is a value a setting can really hold.
      const String notStored = _T("(default)");

      AuditTrail::Instance()->RecordSettingChange(name,
         wasStored ? previousValue : notStored,
         isStored ? value : notStored,
         isStored ? String(_T("updated")) : String(_T("deleted")),
         withheld);
   }

   void
   IniSettingStore::ReadOverrides(std::map<String, String> &values)
   {
      ReadIniSection_(_T("SettingsOverride"), values);
   }

   bool
   IniSettingStore::WriteIniValue_(const String &section, const String &name, const String &value)
   {
      String iniFile = IniFileSettings::GetInitializationFile();
      if (iniFile.IsEmpty())
         return false;

      if (WritePrivateProfileString(section.c_str(), name.c_str(), value.c_str(), iniFile.c_str()) == FALSE)
         return false;

      // WritePrivateProfileString caches; flush so that anything reading the file
      // afterwards - including this process's own GetPrivateProfileString calls -
      // sees the value that was just written.
      WritePrivateProfileString(nullptr, nullptr, nullptr, iniFile.c_str());

      return true;
   }

   void
   IniSettingStore::WriteFileNotice_()
   {
      // Two keys, because the profile API writes keys and not comment lines, and it
      // is the only writer the POSIX build has. Store= is the machine-readable fact;
      // ReadMe= is the sentence an administrator opening the file needs to read
      // before they edit a value and wonder why nothing happened.
      static const String kStore = _T("database");
      static const String kReadMe =
         _T("The [Settings] section below is a CACHE of the hm_inisettings table, which is the settings store from schema 6042. ")
         _T("Editing a value here does not change it: the stored value is used, the edit is named in hMailServer_ERROR.log and the line is put back at the next start or reinitialise. ")
         _T("Settings are changed in the Control Panel, over the REST API at /api/v1/settings/ini, with hmctl, or - for a setting needed before the REST listener is on, such as RestApiPort on Linux - with hmailserver --set-setting NAME=VALUE. ")
         _T("To force a value from this file - when the database is unreachable or wrong - put it in a [SettingsOverride] section, which is applied over the store and announced in the error log at every start. ")
         _T("[Directories], [Database] and [Security] are NOT settings: they are how the database is reached and how an administrator is let in without one, and they stay here.");

      String iniFile = IniFileSettings::GetInitializationFile();
      if (iniFile.IsEmpty() || !IniFileSettings::InitializationFileWritable())
         return;

      const DWORD bufferSize = 4096;
      TCHAR current[bufferSize];

      GetPrivateProfileString(_T("SettingsStore"), _T("Store"), _T(""), current, bufferSize, iniFile.c_str());
      if (kStore.Compare(String(current)) != 0)
         WriteIniValue_(_T("SettingsStore"), _T("Store"), kStore);

      GetPrivateProfileString(_T("SettingsStore"), _T("ReadMe"), _T(""), current, bufferSize, iniFile.c_str());
      if (kReadMe.Compare(String(current)) != 0)
         WriteIniValue_(_T("SettingsStore"), _T("ReadMe"), kReadMe);
   }

   bool
   IniSettingStore::ReadStoredValues(std::map<String, String> &values)
   {
      values.clear();

      std::map<String, std::pair<String, String> > rows;
      if (!ReadAllRows(rows))
         return false;

      // Every row, a file-only name's included: a row for PasswordPepper is left
      // only when the start could not write it into the file, and the start ran on
      // the stored value then (Synchronize, step 1), so a reload must too - reading
      // the file's instead would change the pepper every hash is made with.
      for (auto iter = rows.begin(); iter != rows.end(); iter++)
         values[(*iter).first] = (*iter).second.first;

      return true;
   }

   bool
   IniSettingStore::Synchronize(std::map<String, String> &resolvedValues)
   {
      resolvedValues.clear();

      std::map<String, std::pair<String, String> > rows;
      if (!ReadAllRows(rows))
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5802, "IniSettingStore::Synchronize",
            "The hm_inisettings table could not be read, so the server is running on the copy of those settings in hMailServer.INI. That copy is written by the server and is normally correct, but the database is the settings store: a change made since the file was last written will not be in it, and nothing can be changed until the table can be read again.");
         return false;
      }

      std::map<String, String> iniValues;
      ReadIniSection_(_T("Settings"), iniValues);

      // Asked once for the whole reconcile. False under the Windows service's own
      // account, which may not write the file (IniFileSettings.h): then nothing
      // below tries, the rows are used as they are, and the file's copy is left
      // as the last account that could write it left it.
      const bool fileWritable = IniFileSettings::InitializationFileWritable();

      std::vector<String> edited;
      std::vector<String> adopted;
      std::vector<String> writtenToFile;
      std::vector<String> restoredToFile;
      std::vector<String> unwritable;
      std::vector<String> keptInFileOnly;
      std::vector<String> olderInFile;

      // 1. Everything the file has.
      //
      //    Matched case-sensitively, while Windows treats ini keys case-insensitively.
      //    The one thing that makes visible is an operator who RENAMES a key's case -
      //    AcmeEnabled to acmeenabled - which is seen here as a new key. On a
      //    case-insensitive collation (the default on SQL Server and CE, and what
      //    MySQL uses) the insert below then collides with the row that still holds
      //    the other casing, fails, and is logged; the server runs on the stored
      //    value either way.
      //
      //    Left alone deliberately. Matching case-insensitively here would mean
      //    UPDATE and DELETE statements whose WHERE clause names a different casing
      //    from the stored row, and that is backend-dependent - PostgreSQL compares
      //    case-sensitively where the others do not - so the fix would work on three
      //    backends and silently fail on the fourth.
      for (auto iter = iniValues.begin(); iter != iniValues.end(); iter++)
      {
         const String &name = (*iter).first;
         const String &fileNow = (*iter).second;

         auto row = rows.find(name);

         if (IsFileOnlyName(name))
         {
            // Read from the file by IniFileSettings, because it is not put in
            // resolvedValues. A row an earlier start adopted is taken out - once the
            // file holds the value the server has been running on since then, which
            // is the stored one: every hash made since was made with it.
            if (row != rows.end())
            {
               const String &storedValue = (*row).second.first;

               if (fileNow.Compare(storedValue) == 0 || (fileWritable && WriteIniValue_(_T("Settings"), name, storedValue)))
               {
                  if (DeleteRow_(name))
                     keptInFileOnly.push_back(name);
               }
               else
               {
                  (fileWritable ? unwritable : olderInFile).push_back(name);
                  resolvedValues[name] = storedValue;
               }
            }

            continue;
         }

         if (row == rows.end())
         {
            // No row yet. This is THE MIGRATION: on the first start at schema 6042
            // every key the file has that the table does not is taken into the store,
            // which is what moves 241 settings without moving them one at a time. It
            // is also the ordinary path for a fresh install, and for a key an
            // administrator has put in the file for a server that has never stored it.
            if (InsertRow_(name, fileNow, fileNow))
               adopted.push_back(name);

            resolvedValues[name] = fileNow;
            continue;
         }

         const String &storedValue = (*row).second.first;
         const String &storedFileValue = (*row).second.second;

         // The stored value is the answer in every branch below. What the file says
         // decides only what has to be SAID about it, and what has to be written back.
         resolvedValues[name] = storedValue;

         // Compared as the file reads them back - trimmed - so that a value stored
         // with whitespace at either end before WriteSetting trimmed is not taken
         // for an edit at every start. It is written back trimmed once, below.
         String storedTrimmed = storedValue;
         storedTrimmed.Trim();
         String storedFileTrimmed = storedFileValue;
         storedFileTrimmed.Trim();

         bool fileEdited = fileNow.Compare(storedFileTrimmed) != 0;
         bool fileAgrees = fileNow.Compare(storedTrimmed) == 0;

         if (fileEdited && !fileAgrees)
         {
            // Somebody changed the file and meant something by it. They are told, by
            // name, and the line is put back - once, because after this the file
            // agrees with the store again and the next start says nothing.
            edited.push_back(name);
         }

         if (fileAgrees)
         {
            // The file already holds the stored value; only the bookkeeping can be
            // behind, which is what a hand edit that happened to match leaves - or a
            // row that still carries the whitespace the file has always dropped.
            if (fileEdited || storedTrimmed.Compare(storedValue) != 0 || storedFileTrimmed.Compare(storedFileValue) != 0)
            {
               UpdateRow_(name, storedTrimmed, storedTrimmed);
               resolvedValues[name] = storedTrimmed;
            }

            continue;
         }

         if (!fileWritable)
         {
            // The line cannot be put back. An edit is acknowledged - filevalue
            // takes what the file holds now - so that it is named in this start's
            // HM5804 and not at every start after it; a copy that is merely older
            // than the row is left as it is and counted.
            if (fileEdited)
               UpdateRow_(name, storedValue, fileNow);
            else
               olderInFile.push_back(name);
            continue;
         }

         if (WriteIniValue_(_T("Settings"), name, storedValue))
         {
            UpdateRow_(name, storedValue, storedValue);

            if (!fileEdited)
               writtenToFile.push_back(name);
         }
         else
         {
            unwritable.push_back(name);
         }
      }

      // 2. Rows whose key is not in the file.
      //
      //    This used to DELETE the row, because while the file was the store,
      //    removing a key was how a setting was returned to its default and a mirror
      //    that would not let go of a value was worse than no mirror. With the store
      //    in the database that reasoning inverts exactly: the row IS the setting, a
      //    missing line is a cache that has lost an entry, and the line is written
      //    back. Returning a setting to its default is now DeleteIniSetting over COM,
      //    DELETE /api/v1/settings/ini/{name}, or the Control Panel - each of which
      //    drops the row and the line together.
      for (auto iter = rows.begin(); iter != rows.end(); iter++)
      {
         const String &name = (*iter).first;

         if (iniValues.find(name) != iniValues.end())
            continue;

         const String &storedValue = (*iter).second.first;

         if (IsFileOnlyName(name))
         {
            if (fileWritable && WriteIniValue_(_T("Settings"), name, storedValue) && DeleteRow_(name))
            {
               keptInFileOnly.push_back(name);
            }
            else
            {
               (fileWritable ? unwritable : olderInFile).push_back(name);
               resolvedValues[name] = storedValue;
            }

            continue;
         }

         if (!fileWritable)
         {
            olderInFile.push_back(name);
            resolvedValues[name] = storedValue;
            continue;
         }

         if (WriteIniValue_(_T("Settings"), name, storedValue))
         {
            UpdateRow_(name, storedValue, storedValue);
            restoredToFile.push_back(name);
         }
         else
         {
            unwritable.push_back(name);
         }

         resolvedValues[name] = storedValue;
      }

      // 3. The secrets, sealed or opened as the switch says.
      //
      //    Until 25 September 2026 SRSSecret, BATVSecret, OAuth2HmacSecret, the
      //    metrics listener's credentials and the other names IsProtectedName
      //    answers were stored exactly as typed, in the row and in the file's line.
      //    With protection on - ProtectStoredSecretsWithDPAPI as the DATABASE holds
      //    it, or as [SettingsOverride] forces it, never this machine's copy of the
      //    line alone - each value still as written is sealed under the
      //    installation's stored-secrets key (Util/StoredSecrets.h), when this node
      //    holds that key; with it off, each sealed value this node can open is put
      //    back as written, so that switching it off undoes it. After the reconcile
      //    above, so that a line an administrator edited is still named and put
      //    back first, and the row before the line, as a save writes them: a line
      //    that cannot be written - the packaged Linux file is root's - leaves the
      //    row as the switch says and the line as it was, which reads the same,
      //    and is named below.
      std::vector<String> sealedNow;
      std::vector<String> openedNow;
      std::vector<String> tooLong;
      std::vector<String> linesLeft;
      {
         std::map<String, std::pair<String, String> > current;
         if (ReadAllRows(current))
         {
            const bool protectionOn = IsProtectionOn_(current);
            const bool maySeal = protectionOn && StoredSecrets::MaySeal();

            for (auto iter = current.begin(); iter != current.end(); iter++)
            {
               const String &name = (*iter).first;
               const String &asStored = (*iter).second.first;

               if (asStored.IsEmpty() || !IsProtectedName(name))
                  continue;

               const bool sealed = StoredSecrets::IsEnvelope(asStored);

               String replacement;
               if (protectionOn && !sealed)
               {
                  if (!maySeal || !StoredSecrets::Seal(asStored, StoredSecrets::SettingPurpose(name), replacement))
                     continue;

                  if (!IsStorableValue(replacement))
                  {
                     tooLong.push_back(name);
                     continue;
                  }
               }
               else if (!protectionOn && sealed)
               {
                  bool opened = false;
                  replacement = StoredSecrets::Open(asStored, StoredSecrets::SettingPurpose(name), opened);
                  if (!opened)
                     continue;
               }
               else
               {
                  continue;
               }

               if (!UpdateRow_(name, replacement, (*iter).second.second))
                  continue;

               resolvedValues[name] = replacement;
               (protectionOn ? sealedNow : openedNow).push_back(name);

               if (WriteIniValue_(_T("Settings"), name, replacement))
                  UpdateRow_(name, replacement, replacement);
               else
                  linesLeft.push_back(name);
            }
         }
      }

      // Through StoredSecrets::Note: at a start this runs before the logger is
      // configured, and a line logged now would be dropped whatever logging is set
      // to; the note is logged once it is.
      if (!sealedNow.empty())
      {
         StoredSecrets::Note(Formatter::Format("IniSettingStore: these settings hold secrets and were stored as written; they are now sealed under the stored-secrets key {0}, in the database and in hMailServer.INI: {1}.",
            String(StoredSecrets::HeldKeyId()), StringParser::JoinVector(sealedNow, _T(", "))));
      }

      if (!openedNow.empty())
      {
         StoredSecrets::Note(Formatter::Format("IniSettingStore: ProtectStoredSecretsWithDPAPI is 0, so these settings, which were sealed under the stored-secrets key, are stored as written again, in the database and in hMailServer.INI: {0}.",
            StringParser::JoinVector(openedNow, _T(", "))));
      }

      if (!tooLong.empty())
      {
         // Said, not raised: the value works as it always did, and nothing an
         // administrator does at the next start changes it.
         StoredSecrets::Note(Formatter::Format("IniSettingStore: these settings hold secrets too long to seal within the 4,000 characters a setting may have, so they are kept as written: {0}. A secret of up to about 2,900 characters is sealed.",
            StringParser::JoinVector(tooLong, _T(", "))));
      }

      WriteFileNotice_();

      if (!edited.empty())
      {
         const String putBack = fileWritable
            ? String(_T("The file has been put back to what is stored."))
            : String(_T("The file is read-only to the account the service runs as, so the edit is left in it and is not named again."));

         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5804, "IniSettingStore::Synchronize",
            Formatter::Format(_T("These settings were edited in hMailServer.INI, but the database is the settings store and the stored value has been used instead: {0}. {1} Change a setting in the Control Panel, over the REST API or with hmctl; if this file has to win - because the database is unreachable or wrong - put the key in a [SettingsOverride] section, which is applied at every start and says so in this log."),
               StringParser::JoinVector(edited, _T(", ")), putBack));
      }

      if (!unwritable.empty())
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5803, "IniSettingStore::Synchronize",
            Formatter::Format("These settings could not be written into hMailServer.INI: {0}. The server is using the stored values, so the settings themselves are right; the file's copy, which is read only while the database cannot be, keeps the old ones until a later start can write it.",
               StringParser::JoinVector(unwritable, _T(", "))));
      }

      if (!fileWritable)
      {
         // Once per start, and not as an error: this is how the service's own
         // account is meant to run.
         LOG_APPLICATION(Formatter::Format(_T("IniSettingStore: {0} is read-only to the account the service runs as, which is how a service account of its own is meant to run - NT SERVICE\\hMailServer on Windows, the packages' hmailserver user on Linux (docs/ServiceAccount.md). The database is the settings store and its values are in use; the [Settings] copy in the file is not refreshed. {1} setting(s) there are older than the stored values{2}"),
            IniFileSettings::GetInitializationFile(), StringParser::IntToString((int) olderInFile.size()),
            olderInFile.empty() ? String(_T(".")) : String(_T(": ")) + StringParser::JoinVector(olderInFile, _T(", ")) + _T(".")));
      }

      if (!linesLeft.empty())
      {
         // Not "the old ones": the row and the line hold the same secret, one sealed
         // and one not, and both read the same. What is wrong is that the line still
         // holds it as written, readable by whoever can read the file.
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5803, "IniSettingStore::Synchronize",
            Formatter::Format("These settings hold secrets, and their rows were changed as ProtectStoredSecretsWithDPAPI says, but hMailServer.INI could not be written, so its lines still hold them the other way: {0}. "
               "Both read the same secret. To have the file match, store each again as root - sudo hmailserver --set-setting NAME, with the value on standard input - or give the account the service runs as write access to the file.",
               StringParser::JoinVector(linesLeft, _T(", "))));
      }

      if (!keptInFileOnly.empty())
      {
         LOG_APPLICATION(Formatter::Format("IniSettingStore: these settings are kept in hMailServer.INI only and have been taken out of the database, where an earlier start had stored them: {0}. A password pepper protects the password hashes only while it is somewhere the database is not - so it is not stored with them and not written into a backup. Keep a copy of the file, or of this one value, with the backups: a restore onto another machine needs it to sign anybody in.",
            StringParser::JoinVector(keptInFileOnly, _T(", "))));
      }

      if (!adopted.empty())
      {
         LOG_APPLICATION(Formatter::Format("IniSettingStore: {0} setting(s) from the [Settings] section of hMailServer.INI are now stored in the database, which is the settings store.",
            StringParser::IntToString((int) adopted.size())));
      }

      if (!writtenToFile.empty())
      {
         LOG_APPLICATION(Formatter::Format("IniSettingStore: these settings were changed in the database and have been written into hMailServer.INI: {0}.",
            StringParser::JoinVector(writtenToFile, _T(", "))));
      }

      if (!restoredToFile.empty())
      {
         LOG_APPLICATION(Formatter::Format("IniSettingStore: these settings are stored in the database but were missing from hMailServer.INI, and have been written back into it: {0}. Removing a line from the file no longer returns a setting to its default - delete the setting in the Control Panel, or with DELETE /api/v1/settings/ini/(name).",
            StringParser::JoinVector(restoredToFile, _T(", "))));
      }

      return true;
   }

   void
   IniSettingStore::ReadSettingNames(std::vector<String> &names)
   {
      names.clear();

      // A std::set rather than the two maps' keys concatenated: after a start the
      // table and the file hold the same names, and the caller would otherwise be
      // handed every one of them twice.
      std::set<String> united;

      std::map<String, String> values;
      ReadIniSection_(_T("Settings"), values);

      for (auto iter = values.begin(); iter != values.end(); iter++)
         united.insert((*iter).first);

      std::map<String, std::pair<String, String> > rows;

      if (ReadAllRows(rows))
      {
         for (auto iter = rows.begin(); iter != rows.end(); iter++)
            united.insert((*iter).first);
      }

      for (auto iter = united.begin(); iter != united.end(); iter++)
         names.push_back(*iter);
   }

   bool
   IniSettingStore::IsStorableName(const String &name)
   {
      if (name.IsEmpty() || name.GetLength() > 100)
         return false;

      // A name carrying any of these would not come back as the same key. '=' ends
      // the name in an ini file, '[' and ']' would make it look like a section
      // header, and a newline would split it into two lines - each of which is a
      // way to write one setting and silently create another.
      if (name.Find(_T("=")) >= 0 || name.Find(_T("[")) >= 0 || name.Find(_T("]")) >= 0)
         return false;

      if (name.Find(_T("\r")) >= 0 || name.Find(_T("\n")) >= 0)
         return false;

      // Leading or trailing whitespace does not survive a round trip through
      // GetPrivateProfileSection, so a name carrying it would never match again.
      String trimmed = name;
      trimmed.Trim();

      return trimmed.Compare(name) == 0;
   }

   bool
   IniSettingStore::IsFileOnlyName(const String &name)
   {
      return name.CompareNoCase(_T("PasswordPepper")) == 0;
   }

   bool
   IniSettingStore::IsSecretName(const String &name)
   {
      if (!AuditTrail::IsSecretName(name))
         return false;

      // The fragments matched; these are the shapes in which they match a name
      // whose value is no secret. Each is a shape rather than a key so that the
      // next key of the same shape is right without anybody remembering this.
      String lower = name;
      lower.MakeLower();

      // A path to a key or a certificate is where the secret is, not the secret:
      // RestApiPrivateKeyFile, OAuth2PublicKeyFile, UpdateLogPublicKeyFile.
      if (lower.EndsWith(_T("file")))
         return false;

      // An address: OutboundOAuth2TokenUrl.
      if (lower.EndsWith(_T("url")))
         return false;

      // The password policy is about passwords and holds none.
      if (lower.StartsWith(_T("passwordpolicy")))
         return false;

      // The choice of a hash algorithm; the full-text indexer's settings, whose
      // tokens are the words of a message - IndexerFullTextMaxTokensPerMessage and
      // IndexerFullTextMinTokenLength both carry the fragment, and an exact name
      // for the first let the second be masked and dropped from every export;
      // and the switch that says whether stored secrets are protected. The audit
      // trail now names all four of those exactly, so they never reach this far;
      // the shapes stand so that the next key of each kind is right by itself.
      if (lower.Find(_T("hashalgorithm")) >= 0)
         return false;
      if (lower.StartsWith(_T("indexerfulltext")))
         return false;
      // The work factors of a new password hash - PasswordHashIterations,
      // PasswordHashMemoryKB, PasswordHashTimeCost. Numbers an administrator
      // raises, carrying two of the fragments and holding neither a password nor
      // a hash. The shape, so that the next work factor is right without anybody
      // remembering this; the trail names these three exactly, as it does the
      // hash algorithms above.
      if (lower.StartsWith(_T("passwordhash")))
         return false;
      if (lower.Compare(_T("protectstoredsecretswithdpapi")) == 0)
         return false;

      return true;
   }

   bool
   IniSettingStore::IsProtectedName(const String &name)
   {
      if (!IsSecretName(name) || IsFileOnlyName(name))
         return false;

      // A key the loader reads as a number is never sealed: ResolveInteger_ reads
      // the text as a number, and an envelope would read as 0 without a word. No
      // key does both today; this is so the next one cannot.
      String defaultValue;
      bool isInteger = false;
      if (DefaultFor(name, defaultValue, isInteger) && isInteger)
         return false;

      return true;
   }

   bool
   IniSettingStore::IsProtectionOn_(const std::map<String, std::pair<String, String> > &rows)
   {
      const String switchName = _T("ProtectStoredSecretsWithDPAPI");

      // As the loader would resolve it, and from the same places: [SettingsOverride]
      // first, then the row, then - when neither has it - this machine's file and the
      // shipped default, which is on. Read as GetPrivateProfileInt reads it.
      std::map<String, String> overrides;
      ReadOverrides(overrides);
      for (auto iter = overrides.begin(); iter != overrides.end(); iter++)
      {
         if ((*iter).first.CompareNoCase(switchName) == 0)
            return _ttoi((*iter).second.c_str()) == 1;
      }

      for (auto iter = rows.begin(); iter != rows.end(); iter++)
      {
         if ((*iter).first.CompareNoCase(switchName) == 0)
            return _ttoi((*iter).second.first.c_str()) == 1;
      }

      return IniFileSettings::Instance()->GetProtectStoredSecretsWithDPAPI();
   }

   bool
   IniSettingStore::IsProtectionOnAsStored()
   {
      std::map<String, std::pair<String, String> > rows;
      ReadAllRows(rows);
      return IsProtectionOn_(rows);
   }

   String
   IniSettingStore::ProtectForStore(const String &name, const String &value)
   {
      if (value.IsEmpty() || !IsProtectedName(name) || StoredSecrets::IsEnvelope(value))
         return value;

      // Off, or no key of the database's on this node: the value is kept as
      // written, as it always was, and the next start seals it once the key is here.
      if (!IniFileSettings::Instance()->GetProtectStoredSecretsWithDPAPI())
         return value;

      String envelope;
      if (!StoredSecrets::Seal(value, StoredSecrets::SettingPurpose(name), envelope))
         return value;

      return envelope;
   }

   String
   IniSettingStore::OpenStored(const String &name, const String &stored)
   {
      if (!StoredSecrets::IsEnvelope(stored))
         return stored;

      bool opened = true;
      const String secret = StoredSecrets::Open(stored, StoredSecrets::SettingPurpose(name), opened);
      if (!opened)
         ReportUnopenedSecretOnce_(name, StoredSecrets::EnvelopeKeyId(stored));

      return secret;
   }

   bool
   IniSettingStore::ForgetSealedValue(const String &name, const String &asStored)
   {
      std::shared_ptr<DatabaseConnectionManager> database = StoreDatabase_();

      if (!database)
         return false;

      // The value is compared here, not in the WHERE clause: inisettingvalue is ntext
      // on SQL Server and SQL Server Compact, which refuse an ntext column in an
      // equality, so "and inisettingvalue = @WAS" failed the statement outright on
      // both. The verb runs with the service stopped, so nothing writes the row
      // between the read and the update.
      SQLCommand select("select inisettingvalue from hm_inisettings where inisettingname = @NAME");
      select.AddParameter("@NAME", name);
      std::shared_ptr<DALRecordset> recordset = database->OpenRecordset(select);
      if (!recordset || recordset->IsEOF() || recordset->GetStringValue("inisettingvalue") != asStored)
         return false;

      SQLCommand command("update hm_inisettings set inisettingvalue = @VALUE, inisettingfilevalue = @FILEVALUE where inisettingname = @NAME");
      command.AddParameter("@VALUE", String());
      command.AddParameter("@FILEVALUE", String());
      command.AddParameter("@NAME", name);

      if (!database->Execute(command))
         return false;

      WriteIniValue_(_T("Settings"), name, String());

      // Recorded as a secret's change is, whatever the name: the value it had could
      // not be opened, and was a secret.
      RecordChange_(name, true, _T("(sealed under a key this node does not hold)"), true, String(), false);
      return true;
   }

   bool
   IniSettingStore::IsSettingTheServerReads(const String &name, String &spelling)
   {
      spelling.Empty();

      for (size_t i = 0; i < IniSettingsBaseline::SettingsCount; i++)
      {
         if (name.CompareNoCase(IniSettingsBaseline::Settings[i].Name) == 0)
         {
            spelling = IniSettingsBaseline::Settings[i].Name;
            return true;
         }
      }

      return false;
   }

   bool
   IniSettingStore::DefaultFor(const String &name, String &value, bool &isInteger)
   {
      value.Empty();
      isInteger = false;

      for (size_t i = 0; i < IniSettingsBaseline::SettingsCount; i++)
      {
         if (name.CompareNoCase(IniSettingsBaseline::Settings[i].Name) != 0)
            continue;

         value = IniSettingsBaseline::Settings[i].Default;
         isInteger = IniSettingsBaseline::Settings[i].IsInteger;
         return true;
      }

      return false;
   }

   bool
   IniSettingStore::IsStorableValue(const String &value)
   {
      if (value.GetLength() > 4000)
         return false;

      // Same reasoning as the name: a newline in a value writes a second line into
      // the section, which on the next read is an entirely separate setting.
      return value.Find(_T("\r")) < 0 && value.Find(_T("\n")) < 0;
   }

   bool
   IniSettingStore::WriteSetting(const String &name, const String &requestedValue)
   {
      // Stored as the file will read it back. GetPrivateProfileString and the POSIX
      // implementation both trim a value, so " a " stored as it came would be read
      // back as "a" at every start, taken for an edit to the file, named in the error
      // log as HM5804 and written back as " a " - for ever. The trimmed value is the
      // one the setting has always really had.
      String value = requestedValue;
      value.Trim();

      if (!IsStorableName(name) || !IsStorableValue(value))
         return false;

      if (IsFileOnlyName(name))
      {
         // The file, and a row taken out if an earlier start put one there. The
         // order is the file first: if the row cannot be dropped the value is still
         // right where the server reads it, and the next start drops the row.
         //
         // Recorded like every other key, with the file's line standing in for the
         // row it does not have. The name makes the row read "(changed)" and never
         // the value; the fact is what matters, because no hash made before a new
         // pepper verifies after it.
         String previousFileValue;
         bool hadValue = ReadFileValue_(name, previousFileValue);

         // An administrator's change to a key the environment manages is refused
         // before anything is written (EnvironmentSettings); the value it already
         // has is let through.
         if (!hadValue || previousFileValue != value)
            EnvironmentSettings::RefuseAdministratorChange(EnvironmentSettings::StoreIni, name);

         const bool withheld = IsWithheld_(name);

         if (!IniFileSettings::InitializationFileWritable())
         {
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5806, "IniSettingStore::WriteSetting",
               IniFileSettings::InitializationFileRefusal(name,
                  _T("It is the one setting kept in the file and nowhere else, so that a copy of the database is not enough to attack the password hashes. An administrator sets it in the [Settings] section with an editor run as an administrator and restarts the service.")));
            return false;
         }

         if (!WriteIniValue_(_T("Settings"), name, value))
            return false;

         DeleteRow_(name);

         RecordChange_(name, hadValue, previousFileValue, true, value, withheld);
         return true;
      }

      // The store first. See the header for why the order is not an implementation
      // detail, and why it is the opposite of what it was.
      std::map<String, std::pair<String, String> > rows;

      if (!ReadAllRows(rows))
         return false;

      auto existing = rows.find(name);

      // What the file is BELIEVED to hold right now, kept for the failure path
      // below. Empty for a key that has no row, which is also what the file holds
      // for one - no line at all.
      String previousFileValue = existing == rows.end() ? String() : (*existing).second.second;

      bool wasStored = existing != rows.end();

      // The row as it is kept, and the value it holds as the server uses it: for a
      // protected secret the one is the envelope and the other what it opens to.
      // Everything below that asks what the setting WAS asks the second.
      const String previousStored = wasStored ? (*existing).second.first : String();
      const String previousValue = OpenStored(name, previousStored);

      // As above: the environment's keys are refused to an administrator, here at
      // the store every door writes through - WriteSettingsValue and the audit
      // trail's revert alike - unless the value is the one already stored.
      if (!wasStored || previousValue != value)
         EnvironmentSettings::RefuseAdministratorChange(EnvironmentSettings::StoreIni, name);

      const bool withheld = IsWithheld_(name);

      // What the row and the line are given: the value, or for a protected secret
      // its envelope (docs/SecretsAtRest.md). The value already stored, saved again
      // while protection is on, keeps the envelope it has, so that a settings page
      // writing every field it shows does not rewrite every secret on it; saved with
      // protection off it is stored as written. An empty value clears one, even one
      // whose envelope does not open here.
      String storedValue = value;
      if (IsProtectedName(name))
      {
         const bool keepEnvelope = !value.IsEmpty() && wasStored && StoredSecrets::IsEnvelope(previousStored) &&
                                   previousValue.Compare(value) == 0 && IniFileSettings::Instance()->GetProtectStoredSecretsWithDPAPI();

         storedValue = keepEnvelope ? previousStored : ProtectForStore(name, value);

         // The envelope is longer than what it protects - four characters for every
         // three, and some sixty more - and has to fit the 4,000 characters a setting
         // may have. One that does not is kept as written, as before this row, and
         // said once for the name: refusing it would refuse a value that saved before.
         if (!IsStorableValue(storedValue))
         {
            storedValue = value;
            ReportTooLongOnce_(name, value.GetLength());
         }
      }

      bool stored = existing == rows.end()
         ? InsertRow_(name, storedValue, storedValue)
         : UpdateRow_(name, storedValue, storedValue);

      if (!stored)
         return false;

      // Recorded as soon as the store has the value, which is the moment the change
      // is real: a file write that fails below does not undo it, and says so.
      RecordChange_(name, wasStored, previousValue, true, value, withheld);

      // A file this account may not write is not tried - the service's own
      // account is given no write to it (IniFileSettings.h) - and that is said
      // once, not at every save. filevalue takes what the file really holds, for
      // the same reason as on the failure path below.
      if (!IniFileSettings::InitializationFileWritable())
      {
         String fileHolds;
         FileLine_(name, fileHolds);
         UpdateRow_(name, value, fileHolds);
         NoteReadOnlyFile_();
         return true;
      }

      // The cache second, and a failure here does not fail the write: the value is
      // stored and is what the server will use. Said out loud rather than swallowed,
      // because "it saved, but /Register still sees the old service account" is
      // precisely the half-success this area keeps producing when nobody says
      // anything.
      if (!WriteIniValue_(_T("Settings"), name, storedValue))
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5806, "IniSettingStore::WriteSetting",
            Formatter::Format("The setting '{0}' was stored in the database, which is the settings store, but hMailServer.INI could not be written. The setting itself is correct and will be used; the file's copy, which is read only while the database cannot be, keeps the old value until a later start can write it.", name));

         // filevalue must go back to what the file really holds, NOT stay at the
         // value the insert or update above optimistically put there. It is the one
         // thing that tells Synchronize an edit from a stale copy, and leaving it
         // claiming this value would have the next start read the file, find the old
         // one, and accuse an administrator of editing a file nobody touched - while
         // doing exactly the right thing about it. With the previous value here the
         // next start sees no edit, writes the line again, and says so quietly.
         UpdateRow_(name, storedValue, previousFileValue);
      }

      return true;
   }

   bool
   IniSettingStore::RemoveSetting(const String &name)
   {
      if (!IsStorableName(name))
         return false;

      // Returning a key the environment manages to its default is a change the
      // next start would undo, and is refused to an administrator as a write is.
      EnvironmentSettings::RefuseAdministratorChange(EnvironmentSettings::StoreIni, name);

      const bool withheld = IsWithheld_(name);

      // THE LINE FIRST, and this is the one place where the order is the opposite of
      // WriteSetting's - for the same reason the order is what it is there, which is
      // "leave behind whichever residue the next start repairs, never the one it
      // mistakes for an instruction".
      //
      // A leftover ROW is repaired: Synchronize finds a row whose line is missing and
      // writes the line back. A leftover LINE is not - it is a key the table has never
      // seen, which is adopted as a new setting, so a delete that dropped the row and
      // failed to remove the line would put the setting straight back at the next
      // start and report success in the meantime. So the file goes first and a failure
      // there changes nothing at all.
      //
      // Passing a null value is what DELETES the line rather than leaving "Key="
      // behind - and "Key=" is not the same thing at all, because
      // GetPrivateProfileInt reads an empty value as 0 rather than falling back to
      // the caller's default. That distinction has bitten this project twice.

      // What is being removed, read before anything is: the audit trail records the
      // value the setting had, and after the delete there is nothing left to read.
      // The file-only key has no row, and its line is what is read.
      String previousValue;
      bool wasStored = IsFileOnlyName(name)
         ? ReadFileValue_(name, previousValue)
         : ReadRow_(name, previousValue);

      String iniFile = IniFileSettings::GetInitializationFile();

      // A file the service's account may not write (IniFileSettings.h). With no
      // line to take out, the row is dropped as ever. With one, dropping the row
      // would have the next start adopt the line and put the setting back, so a
      // key this build reads keeps a row holding its default - which is what a
      // removal means to the server - and anything else is refused with a
      // sentence, as the one file-only key is.
      String fileHolds;
      if (!iniFile.IsEmpty() && FileLine_(name, fileHolds) && !IniFileSettings::InitializationFileWritable())
      {
         String defaultValue;
         bool isInteger = false;
         if (IsFileOnlyName(name) || !DefaultFor(name, defaultValue, isInteger))
         {
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5806, "IniSettingStore::RemoveSetting",
               IniFileSettings::InitializationFileRefusal(Formatter::Format(_T("The line for '{0}'"), name),
                  _T("An administrator deletes the line from the [Settings] section with an editor run as an administrator, then removes the setting again.")));
            return false;
         }

         const bool kept = wasStored
            ? UpdateRow_(name, defaultValue, fileHolds)
            : InsertRow_(name, defaultValue, fileHolds);
         if (!kept)
            return false;

         NoteReadOnlyFile_();
         LOG_APPLICATION(Formatter::Format(_T("IniSettingStore: '{0}' was returned to its default, {1}. Its line in hMailServer.INI could not be taken out - the file is read-only to the account the service runs as - so the setting is stored with that value instead of being removed, which the server treats the same."),
            name, defaultValue.IsEmpty() ? String(_T("(empty)")) : defaultValue));

         RecordChange_(name, wasStored, previousValue, false, String(), withheld);
         return true;
      }

      if (!iniFile.IsEmpty() && IniFileSettings::InitializationFileWritable())
      {
         if (WritePrivateProfileString(_T("Settings"), name.c_str(), nullptr, iniFile.c_str()) == FALSE)
            return false;

         WritePrivateProfileString(nullptr, nullptr, nullptr, iniFile.c_str());
      }

      if (!DeleteRow_(name))
      {
         // The line has gone and the row has not. The setting is NOT back at its
         // default and the caller is told so; the next start finds a row with no line
         // and writes the line back, which is the state this started in.
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5806, "IniSettingStore::RemoveSetting",
            Formatter::Format("The setting '{0}' could not be removed from the database, which is the settings store, so it has not been returned to its default. Its line in hMailServer.INI was removed and will be written back from the stored value at the next start.", name));

         return false;
      }

      RecordChange_(name, wasStored, previousValue, false, String(), withheld);

      return true;
   }

   bool
   IniSettingStore::XMLStore(XNode *pBackupNode)
   {
      // Read the table rather than serialising the in-memory resolved values: a row
      // this build does not recognise - one written by a newer build, or a setting
      // since removed from the code - must still survive a backup and restore. The
      // resolved map only contains keys this build asked for.
      std::map<String, std::pair<String, String> > rows;

      if (!ReadAllRows(rows))
         return false;

      XNode *pNode = pBackupNode->AppendChild(_T("IniSettings"));

      for (auto iter = rows.begin(); iter != rows.end(); iter++)
      {
         // The name goes in an ATTRIBUTE, not in the element name. XMLite escapes
         // attribute values and writes element names raw, and these names come from
         // a file an administrator edits - so a name that is not a valid XML element
         // name would produce an archive that cannot be parsed back.
         if (IsFileOnlyName((*iter).first))
            continue;

         // A sealed secret goes into the archive opened, as the server's other stored
         // secrets go into the settings it carries (Backup.md): a restore has to put
         // it back on a server that may hold another stored-secrets key, or none, and
         // seals it again there. An encrypted archive covers it. One that does not
         // open here is carried as it is - which is what it is worth here too - so
         // that a restore where its key is held keeps it.
         String value = (*iter).second.first;
         if (StoredSecrets::IsEnvelope(value))
         {
            bool opened = true;
            const String secret = StoredSecrets::Open(value, StoredSecrets::SettingPurpose((*iter).first), opened);
            if (opened)
               value = secret;
         }

         XNode *pSetting = pNode->AppendChild(_T("Setting"));
         pSetting->AppendAttr(_T("Name"), (*iter).first);
         pSetting->AppendAttr(_T("Value"), value);
      }

      return true;
   }

   bool
   IniSettingStore::XMLLoad(XNode *pBackupNode)
   {
      XNode *pNode = pBackupNode->GetChild(_T("IniSettings"));

      // Absent means the archive was taken before this feature existed. Doing
      // nothing is the only safe reading: there is no version stamp on the element
      // to distinguish "old archive" from "no settings", and clearing the table on
      // an old archive would discard the configuration of the server being restored
      // onto.
      if (!pNode)
         return true;

      // Read once and kept up to date as rows are added, rather than re-read inside
      // the loop. A restore of a server with sixty ini settings issued sixty full
      // table reads to answer a question the first one had already answered.
      std::map<String, std::pair<String, String> > rows;

      if (!ReadAllRows(rows))
         return false;

      std::vector<String> writeFailures;
      std::vector<String> notWritten;

      // As in Synchronize: the service's own account may not write the file, and
      // is not made to try (IniFileSettings.h).
      const bool fileWritable = IniFileSettings::InitializationFileWritable();

      for (int i = 0; i < pNode->GetChildCount(); i++)
      {
         XNode *pSetting = pNode->GetChild(i);

         String name = pSetting->GetAttrValue(_T("Name"));
         String value = pSetting->GetAttrValue(_T("Value"));

         if (name.IsEmpty())
            continue;

         if (IsFileOnlyName(name))
         {
            const DWORD bufferSize = 4096;
            TCHAR current[bufferSize];
            GetPrivateProfileString(_T("Settings"), name.c_str(), _T(""), current, bufferSize, IniFileSettings::GetInitializationFile().c_str());

            if (String(current).IsEmpty() && !value.IsEmpty())
            {
               // Braces: LOG_APPLICATION expands to an if of its own, so a bare
               // else after it would pair with the macro.
               if (!fileWritable)
               {
                  ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5805, "IniSettingStore::XMLLoad",
                     IniFileSettings::InitializationFileRefusal(name,
                        _T("The archive carried a value for it and the file has none. An administrator puts the value in the [Settings] section with an editor run as an administrator and restarts the service; until then, passwords hashed with it do not verify.")));
               }
               else if (WriteIniValue_(_T("Settings"), name, value))
               {
                  LOG_APPLICATION(Formatter::Format("IniSettingStore: the archive carried {0}, which this server keeps in hMailServer.INI only; the file had none, so the archived value has been written into the file and not into the database.", name));
               }
               else
               {
                  writeFailures.push_back(name);
               }
            }

            continue;
         }

         // A secret the archive carries opened is sealed again, under this server's
         // stored-secrets key, before it is written anywhere; one it carries as an
         // envelope is written as it is. A sealed form too long for the column leaves
         // the value as the archive has it, as the start's pass would.
         const String protectedValue = ProtectForStore(name, value);
         if (IsStorableValue(protectedValue))
            value = protectedValue;

         // Restore is authoritative: the point of restoring settings is to get the
         // backed-up settings. Both the row and the file are written, and the two
         // are recorded as agreed, so the merge on the next start sees no change and
         // does not hand the restored value back to whatever the local file said.
         //
         // Only when the file write SUCCEEDED, though. filevalue means "what the file
         // is known to hold"; recording the restored value there after a write that
         // did not happen would tell the next Synchronize that the two agree, and the
         // restored setting would sit in the table for ever without ever reaching the
         // file that every direct reader - and /Register - actually reads.
         bool written = fileWritable && WriteIniValue_(_T("Settings"), name, value);

         if (!written)
            (fileWritable ? writeFailures : notWritten).push_back(name);

         // What the file really holds when it was not written - not an empty
         // string, which the next start would read as an edit of every line.
         String fileValue = value;
         if (!written)
            FileLine_(name, fileValue);

         // NOT recorded in the audit trail, and not by oversight. A restore runs as a
         // BackupTask on the maintenance work queue, and the audit actor is
         // thread-local: nothing installs one on that thread, so a RecordChange_ here
         // could never write a row, and a call that cannot record is worse than none
         // when the page describing the trail reads it as a promise. The rest of a
         // restore is in the same position - PropertySet::XMLLoad and every table the restore
         // rewrites record nothing either - so the whole of it is named as unrecorded
         // in docs/AlertsAndAuditTrail.md until somebody decides whether a restore
         // should be one row saying who restored which backup, or a row per value.
         if (rows.find(name) == rows.end())
         {
            if (InsertRow_(name, value, fileValue))
               rows[name] = std::make_pair(value, fileValue);
         }
         else
         {
            if (UpdateRow_(name, value, fileValue))
               rows[name] = std::make_pair(value, fileValue);
         }
      }

      if (!writeFailures.empty())
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5805, "IniSettingStore::XMLLoad",
            Formatter::Format("These settings were restored into the database but could not be written to hMailServer.INI: {0}. The database holds them and they are in use; the file's copy is written at a later start rather than lost.",
               StringParser::JoinVector(writeFailures, _T(", "))));
      }

      if (!notWritten.empty())
         NoteReadOnlyFile_();

      return true;
   }
}
