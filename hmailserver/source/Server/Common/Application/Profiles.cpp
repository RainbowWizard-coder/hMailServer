// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Profiles: a diff, then a snapshot, then the writes, and a way back. See
// Profiles.h, and docs/Profiles.md for what an administrator reads.
//
// THE TWO RECORDS, as hm_settings text:
//
//   ProfileSnapshot  {"profile":"office","applied":<unix>,"entries":[
//                       ["i"|"d", name, before, after, had], ...]}
//                    one entry per setting the apply changed: the value it had
//                    (for a [Settings] key, the stored text; had is 0 when the key
//                    was not stored, and the way back removes it), and the value
//                    the apply left, which is what the way back asks the setting
//                    still to hold before it writes the old one over it. Empty
//                    when there is nothing to go back to.
//   ProfileApplied   {"profile":"office","applied":<unix>,"state":"applied"|
//                       "reverted"|"kept"|"failed"|"declined","changed":<unix>,
//                       "set":["i:<key>","d:<property>",...]}
//                    the last profile applied, what became of it, and the settings
//                    its apply wrote. profile is empty, and state declined, when
//                    no profile is wanted. Empty on a server nothing was recorded
//                    for - which is what --apply-profile --once asks.
//
// Both fit hm_settings.settingstring's 4000 characters: an apply whose way back
// would not is refused before anything is written. Both are READ FROM THE
// DATABASE whenever they decide anything - not from the property set this process
// loaded - because hmailserver --apply-profile writes them from another process,
// and a server that decided from its own copy would take a way back the command
// line saved for none, and write another over it. Both are WRITTEN through
// PropertySet::WriteBookkeeping, which says when the database did not take one:
// a way back that was not stored is no way back, and nothing is applied then.

#include "StdAfx.h"
#include "Profiles.h"
#include "ProfilesData.h"

#include "Configuration.h"
#include "EffectiveSetting.h"
#include "EnvironmentSettings.h"
#include "IniFileSettings.h"
#include "IniSettingStore.h"
#include "IniSettingsBaseline.h"
#include "PropertySet.h"
#include "SettingsDefaults.h"
#include "../SQL/DatabaseConnectionManager.h"
#include "../SQL/DALRecordset.h"
#include "../SQL/SQLCommand.h"
#include "../Util/AuditTrail.h"
#include "../Util/Hashing/HashCreator.h"
#include "../Util/JsonDocument.h"
#include "../Util/SetupChecklist.h"
#include "../Util/Unicode.h"
#include "../Util/RestApiSettingsRows.h"

#include <climits>
#include <cstdio>
#include <ctime>
#include <functional>
#include <mutex>
#include <thread>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // One apply, revert, discard or decline at a time: each reads the records,
      // decides, and writes them, and two at once would each decide on what the
      // other is about to replace.
      std::mutex profiles_mutex;

      // Whether this thread holds profiles_mutex. An apply and a revert write
      // settings through their rows while they hold it, and a write can run an event
      // script on this same thread: an error reported on the way fires OnError
      // (ErrorManager::ReportError), and a write of the scripting switch runs the
      // script's top level (Configuration::SetUseScriptServer, ScriptServer::
      // SwitchOn) - no shipped profile sets that switch, but a revert writes back
      // whatever its way back holds. A script that calls back into an apply, a
      // revert, a discard or a decline over COM - an in-process call, on this
      // thread - would lock the mutex this thread already holds, which std::mutex
      // does not allow: MSVC's throws resource_deadlock_would_occur (measured with
      // the toolset the server is built with, 1 October 2026), which COM answered as
      // an unknown error, and glibc's waits for ever, with the script, the profile's
      // writes and every other profile verb behind it. So a call made while this
      // thread holds it is refused, with the reason, before it locks (Reentered_).
      // A call that arrives on another thread - a script asking over REST - is not
      // refused: it waits for the lock as any other caller does. Found by the
      // re-check of 29 September 2026; refused from 6.3.6.
      thread_local bool holds_profiles_mutex = false;

      // profiles_mutex, held and marked as this thread's for as long as it lives.
      class ProfilesLock
      {
      public:
         ProfilesLock() : guard_(profiles_mutex) { holds_profiles_mutex = true; }
         ~ProfilesLock() { holds_profiles_mutex = false; }

         ProfilesLock(const ProfilesLock &) = delete;
         ProfilesLock &operator=(const ProfilesLock &) = delete;

      private:
         // Released after the destructor's body has cleared the mark.
         std::lock_guard<std::mutex> guard_;
      };

      // A verb called on the thread that holds profiles_mutex: refused, with the
      // sentence and a JSON answer, instead of locking it again.
      bool Reentered_(const TCHAR *verb, String &sentence, AnsiString &json)
      {
         if (!holds_profiles_mutex)
            return false;

         sentence = Formatter::Format("A profile could not be {0}: this thread is applying or reverting one, and wrote a setting that ran an event script, which asked for this. An event script cannot apply, revert, discard or decline a profile while one is being applied or reverted; nothing was changed by this request.",
            String(verb));
         json = "{\"reentered\":true}";
         return true;
      }

      // The widest value hm_settings.settingstring holds.
      const int MaximumRecord = 4000;

      // The error codes of this unit (7300-7309).
      const int ErrorSnapshotUnreadable = 7300;
      const int ErrorRecordNotWritten = 7301;

      AnsiString Utf8(const String &text)
      {
         AnsiString result;
         Unicode::WideToMultiByte(text, result);
         return result;
      }

      String Wide(const std::string &text)
      {
         String result;
         Unicode::MultiByteToWide(AnsiString(text.c_str()), result);
         return result;
      }

      AnsiString JsonEscape(const AnsiString &value)
      {
         AnsiString out;
         const char *chars = value.c_str();
         for (size_t i = 0; i < value.size(); i++)
         {
            const unsigned char c = (unsigned char) chars[i];
            switch (c)
            {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
               if (c < 0x20)
               {
                  AnsiString escaped;
                  escaped.Format("\\u%04x", (int) c);
                  out += escaped;
               }
               else
                  out += (char) c;
            }
         }
         return out;
      }

      AnsiString JsonText(const String &value)
      {
         return "\"" + JsonEscape(Utf8(value)) + "\"";
      }

      AnsiString JsonTextOrNull(const String &value)
      {
         return value.IsEmpty() ? AnsiString("null") : JsonText(value);
      }

      AnsiString JsonNumber(__int64 value)
      {
         AnsiString text;
         text.Format("%I64d", value);
         return text;
      }

      AnsiString JsonBool(bool value)
      {
         return value ? "true" : "false";
      }

      String UtcText(__int64 seconds)
      {
         struct tm parts = {};
#ifdef HM_PLATFORM_POSIX
         time_t value = (time_t) seconds;
         if (::gmtime_r(&value, &parts) == nullptr)
            return String();
#else
         __time64_t value = seconds;
         if (_gmtime64_s(&parts, &value) != 0)
            return String();
#endif
         String text;
         text.Format(_T("%04d-%02d-%02d %02d:%02d UTC"), parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday, parts.tm_hour, parts.tm_min);
         return text;
      }

      __int64 Now()
      {
         return (__int64) time(nullptr);
      }

      // As GetPrivateProfileInt reads a key: an optional sign and the digits that
      // follow, and 0 for anything else - so "0009" is 9, which is how the server
      // reads a [Settings] key that is a number.
      long long LeadingNumber(const String &text)
      {
         String trimmed = text;
         trimmed.Trim();

         long long value = 0;
         int i = 0;
         bool negative = false;
         if (i < trimmed.GetLength() && (trimmed.GetAt(i) == '-' || trimmed.GetAt(i) == '+'))
         {
            negative = trimmed.GetAt(i) == '-';
            i++;
         }

         for (; i < trimmed.GetLength(); i++)
         {
            const wchar_t c = trimmed.GetAt(i);
            if (c < '0' || c > '9')
               break;
            value = value * 10 + (c - '0');
            if (value > (long long) INT_MAX + 1)
               break;
         }

         return negative ? -value : value;
      }

      const ProfilesData::Profile *FindProfile(const String &name)
      {
         for (size_t i = 0; i < ProfilesData::ProfileCount; i++)
         {
            if (name.CompareNoCase(ProfilesData::Profiles[i].name) == 0)
               return &ProfilesData::Profiles[i];
         }
         return nullptr;
      }

      const IniSettingsBaseline::Setting *FindIniKey(const String &name)
      {
         for (size_t i = 0; i < IniSettingsBaseline::SettingsCount; i++)
         {
            if (name.CompareNoCase(IniSettingsBaseline::Settings[i].Name) == 0)
               return &IniSettingsBaseline::Settings[i];
         }
         return nullptr;
      }

      const SettingsDefaults::Setting *FindProperty(const String &name)
      {
         for (size_t i = 0; i < SettingsDefaults::SettingsCount; i++)
         {
            if (name.CompareNoCase(SettingsDefaults::Settings[i].Name) == 0)
               return &SettingsDefaults::Settings[i];
         }
         return nullptr;
      }

      const char *StoreWord(bool ini)
      {
         return ini ? "ini" : "database";
      }

      String StoreKey(bool ini, const String &name)
      {
         return String(ini ? _T("i:") : _T("d:")) + name;
      }

      const char *CautionWord(ProfilesData::Caution caution)
      {
         switch (caution)
         {
         case ProfilesData::CautionClosesListener: return "closes_listener";
         case ProfilesData::CautionTrustsAddresses: return "trusts_addresses";
         case ProfilesData::CautionTurnsFilterOff: return "turns_filter_off";
         case ProfilesData::CautionPurges: return "purges";
         default: return nullptr;
         }
      }

      const TCHAR *CautionSentence(ProfilesData::Caution caution)
      {
         switch (caution)
         {
         case ProfilesData::CautionClosesListener: return _T("closes a listener");
         case ProfilesData::CautionTrustsAddresses: return _T("trusts an address range");
         case ProfilesData::CautionTurnsFilterOff: return _T("turns a filter or a check off, or weakens it");
         case ProfilesData::CautionPurges: return _T("shortens how long something is kept, and the next sweep deletes what it no longer keeps");
         default: return _T("");
         }
      }

      // The kind a setting with levels is (docs/SettingLevels.md), for the one whose
      // server level this setting is: a limit's server value is the ceiling every
      // domain and account below it is held to.
      String LevelsKind(bool ini, const String &name)
      {
         size_t count = 0;
         const EffectiveSetting::Definition *definitions = EffectiveSetting::Definitions(count);
         for (size_t i = 0; i < count; i++)
         {
            const EffectiveSetting::Definition &definition = definitions[i];
            const bool storeMatches = ini ? definition.server_kind == EffectiveSetting::ServerIniKey
                                          : definition.server_kind == EffectiveSetting::ServerProperty;
            if (!storeMatches || !definition.server_key || name.CompareNoCase(String(definition.server_key)) != 0)
               continue;

            switch (definition.klass)
            {
            case EffectiveSetting::ClassLimit: return _T("limit");
            case EffectiveSetting::ClassGate: return _T("gate");
            default: return _T("preference");
            }
         }
         return String();
      }

      // When a value written through this route reaches the running server: the
      // REST row's own mark, or the settings store's for a [Settings] key no row
      // keeps.
      String AppliesOf(bool ini, const String &name)
      {
         RestApiSettingsRows::Lent lent;
         if (RestApiSettingsRows::Find(ini, name, lent))
            return String(lent.applies);

         if (ini)
         {
            String applies;
            String spelling;
            return IniFileSettings::GetApplies(name, applies, spelling) ? applies : String(_T("now"));
         }

         return String();
      }

      // One entry, as the server stands now: what it holds, whether the profile
      // would change it, how it would be written, and anything that stops it.
      struct Evaluated
      {
         Evaluated() : entry(nullptr), ini(false), number(0), stored(false), integer(false), changes(false), lent(false), confirm(false) {}

         const ProfilesData::Entry *entry;
         bool ini;
         String name;
         String value;
         long number;
         String current;
         String storedText;   // a [Settings] key's stored text, what the way back restores
         bool stored;
         bool integer;
         bool changes;
         bool lent;
         String route;
         String applies;
         String levels;
         String environment;  // the variable, when the server's environment sets it
         String refused;      // the sentence, when the entry's setting would refuse it
         bool confirm;
      };

      bool SameValue(const Evaluated &evaluated, const String &left, const String &right)
      {
         if (evaluated.integer)
            return LeadingNumber(left) == LeadingNumber(right);
         return left == right;
      }

      // For a window of days: how long a value keeps, in days, with 0 meaning what
      // the setting says it means - for ever, or nothing.
      long long WindowDays(ProfilesData::Zero zero, long long days)
      {
         if (days > 0)
            return days;
         return zero == ProfilesData::ZeroKeepsForever ? LLONG_MAX : 0;
      }

      // Whether writing value over current shortens the window: the next sweep
      // deletes what the shorter window no longer keeps.
      bool Shortens(const ProfilesData::Entry &entry, const String &current, const String &value)
      {
         return WindowDays(entry.zero, LeadingNumber(value)) < WindowDays(entry.zero, LeadingNumber(current));
      }

      typedef std::map<String, std::pair<String, String> > StoredRows;

      // What the setting holds now, read as the server reads it. rows, when given, is
      // the settings store read once for a whole profile rather than once an entry.
      // False when the settings store could not be read, which for a [Settings] key
      // leaves whether it is stored - what the way back removes or restores -
      // unknown.
      bool ReadCurrent(Evaluated &evaluated, const StoredRows *rows = nullptr)
      {
         if (evaluated.ini)
         {
            StoredRows read;
            if (!rows)
            {
               if (!IniSettingStore::ReadAllRows(read))
                  return false;
               rows = &read;
            }

            evaluated.stored = false;
            evaluated.storedText.Empty();
            for (auto row = rows->begin(); row != rows->end(); row++)
            {
               if ((*row).first.CompareNoCase(evaluated.name) == 0)
               {
                  evaluated.stored = true;
                  evaluated.storedText = (*row).second.first;
                  break;
               }
            }

            // The value in force: the stored one, or the [SettingsOverride] one where
            // the file forces one, and the shipped default where neither is.
            String value = IniFileSettings::Instance()->GetSettingsValue(evaluated.name);
            if (value.IsEmpty() && !evaluated.stored)
            {
               const IniSettingsBaseline::Setting *key = FindIniKey(evaluated.name);
               value = key ? String(key->Default) : String();
            }
            evaluated.current = value;
            return true;
         }

         std::shared_ptr<PropertySet> properties = Configuration::Instance()->GetPropertySet();
         evaluated.stored = properties && properties->Exists(evaluated.name);

         const SettingsDefaults::Setting *property = FindProperty(evaluated.name);
         if (!evaluated.stored)
         {
            evaluated.current = property ? (property->Kind == SettingsDefaults::KindString ? String(property->Value) : StringParser::IntToString((int) property->Number)) : String();
            return true;
         }

         if (property && property->Kind == SettingsDefaults::KindString)
            evaluated.current = properties->GetString(evaluated.name);
         else
            evaluated.current = StringParser::IntToString((int) properties->GetLong(evaluated.name));
         return true;
      }

      Evaluated Evaluate(const ProfilesData::Entry &entry, const StoredRows &rows)
      {
         Evaluated evaluated;
         evaluated.entry = &entry;
         evaluated.ini = entry.store == ProfilesData::StoreIni;
         evaluated.name = entry.name;
         evaluated.value = entry.value;

         if (evaluated.ini)
         {
            const IniSettingsBaseline::Setting *key = FindIniKey(evaluated.name);
            evaluated.integer = key && key->IsInteger;
            if (!key)
               evaluated.refused = Formatter::Format("{0} is not a [Settings] key this build reads.", evaluated.name);
         }
         else
         {
            const SettingsDefaults::Setting *property = FindProperty(evaluated.name);
            evaluated.integer = property && property->Kind == SettingsDefaults::KindNumber;
            if (!property)
               evaluated.refused = Formatter::Format("{0} is not an hm_settings property this build reads.", evaluated.name);
         }

         evaluated.number = (long) LeadingNumber(evaluated.value);

         ReadCurrent(evaluated, &rows);
         evaluated.changes = !SameValue(evaluated, evaluated.current, evaluated.value);
         evaluated.levels = LevelsKind(evaluated.ini, evaluated.name);

         // The environment wins at every start, so a setting it manages is left as
         // it is and said to be (docs/EnvironmentConfiguration.md).
         EnvironmentSettings::Entry managed;
         if (EnvironmentSettings::IsManaged(evaluated.ini ? EnvironmentSettings::StoreIni : EnvironmentSettings::StoreDatabase, evaluated.name, managed))
            evaluated.environment = managed.variable;

         RestApiSettingsRows::Lent lent;
         evaluated.lent = RestApiSettingsRows::Find(evaluated.ini, evaluated.name, lent);

         if (evaluated.lent)
         {
            evaluated.route = Formatter::Format("PUT {0} {1}", String(lent.path), String(lent.key));
            evaluated.applies = String(lent.applies);
         }
         else if (evaluated.ini)
         {
            evaluated.route = _T("PUT /api/v1/settings/ini/") + evaluated.name;
            evaluated.applies = AppliesOf(true, evaluated.name);
         }
         else if (evaluated.refused.IsEmpty())
         {
            // Never straight into the property set: the setter a REST row runs is
            // what derives, canonicalises and refuses as every interface does.
            evaluated.refused = Formatter::Format("No REST settings row writes {0}, and a profile writes a property only through the setter a row runs; the profile's entry has to be corrected.", evaluated.name);
         }

         if (evaluated.refused.IsEmpty())
         {
            // The checks every interface makes of the value, in their own sentences:
            // the settings store's for a [Settings] key, and the REST row's own.
            if (evaluated.ini)
               evaluated.refused = EnvironmentSettings::CheckIniValue(evaluated.name, evaluated.value);

            if (evaluated.refused.IsEmpty() && evaluated.lent)
               evaluated.refused = RestApiSettingsRows::Check(evaluated.ini, evaluated.name, evaluated.number, evaluated.value);
         }

         // A caution asks for the confirmation wherever the entry would change
         // something; a window of days only where it would shorten the one in
         // force, because a longer window deletes nothing.
         bool earned = entry.caution != ProfilesData::CautionNone && evaluated.changes;
         if (entry.caution == ProfilesData::CautionPurges)
            earned = evaluated.changes && Shortens(entry, evaluated.current, evaluated.value);

         evaluated.confirm = earned && evaluated.environment.IsEmpty() && evaluated.refused.IsEmpty();

         return evaluated;
      }

      // Every entry of the profile, as the server stands now. False when the
      // settings store could not be read: the diff would then say a stored key was
      // not, and a way back taken from it would remove it.
      bool EvaluateAll(const ProfilesData::Profile &profile, std::vector<Evaluated> &all)
      {
         StoredRows rows;
         const bool read = IniSettingStore::ReadAllRows(rows);

         all.clear();
         for (size_t i = 0; i < profile.entryCount; i++)
            all.push_back(Evaluate(profile.entries[i], rows));
         return read;
      }

      // What the diff showed, as a token: the profile and, for every entry, the
      // value in force, whether it would change, whether it asks for the
      // confirmation, and what would skip or refuse it. An apply that brings a token
      // is refused when the server no longer answers the same diff.
      String DiffToken(const ProfilesData::Profile &profile, const std::vector<Evaluated> &all)
      {
         AnsiString canonical = Utf8(String(profile.name)) + "\n";
         for (const Evaluated &evaluated : all)
         {
            canonical += AnsiString(StoreWord(evaluated.ini)) + "\t" + Utf8(evaluated.name) + "\t" + Utf8(evaluated.value) + "\t" +
                         Utf8(evaluated.current) + "\t" + (evaluated.changes ? "1" : "0") + "\t" + (evaluated.confirm ? "1" : "0") + "\t" +
                         Utf8(evaluated.environment) + "\t" + Utf8(evaluated.refused) + "\n";
         }

         HashCreator hasher(HashCreator::SHA256);
         AnsiString hash = hasher.GenerateHashNoSalt(canonical, HashCreator::hex);
         hash.MakeLower();
         return String(hash.Left(16));
      }

      // ---- the records ---------------------------------------------------------

      // The record as the database holds it now. False when it could not be read;
      // a missing row is an empty record.
      bool ReadRecord(const TCHAR *row, String &value)
      {
         value.Empty();

         std::shared_ptr<DatabaseConnectionManager> database = Application::Instance()->GetDBManager();
         if (!database)
            return false;

         SQLCommand command("select settingstring from hm_settings where settingname = @SETTINGNAME");
         command.AddParameter("@SETTINGNAME", String(row));

         std::shared_ptr<DALRecordset> recordset = database->OpenRecordset(command);
         if (!recordset)
            return false;

         if (!recordset->IsEOF())
            value = recordset->GetStringValue("settingstring");
         return true;
      }

      // The server's own bookkeeping, written as it is: no refusal, no audit row of its
      // own (the apply or revert records itself as one event) and no change event.
      // False, reported, when the database did not take it.
      bool WriteRecord(const TCHAR *row, const String &value)
      {
         std::shared_ptr<PropertySet> properties = Configuration::Instance()->GetPropertySet();
         if (properties && properties->WriteBookkeeping(row, value))
            return true;

         ErrorManager::Instance()->ReportError(ErrorManager::Medium, ErrorRecordNotWritten, "Profiles::WriteRecord",
            Formatter::Format("The setting profile's record {0} could not be written to hm_settings.", String(row)));
         return false;
      }

      struct SnapshotEntry
      {
         SnapshotEntry() : ini(false), had(true) {}

         bool ini;
         String name;
         String before;
         String after;
         bool had;
      };

      struct Snapshot
      {
         Snapshot() : applied(0) {}

         String profile;
         __int64 applied;
         std::vector<SnapshotEntry> entries;
      };

      AnsiString SnapshotJson(const Snapshot &snapshot)
      {
         AnsiString json = "{\"profile\":" + JsonText(snapshot.profile) + ",\"applied\":" + JsonNumber(snapshot.applied) + ",\"entries\":[";
         for (size_t i = 0; i < snapshot.entries.size(); i++)
         {
            const SnapshotEntry &entry = snapshot.entries[i];
            if (i > 0)
               json += ",";
            json += "[\"" + AnsiString(entry.ini ? "i" : "d") + "\"," + JsonText(entry.name) + "," + JsonText(entry.before) + "," +
                    JsonText(entry.after) + "," + (entry.had ? "1" : "0") + "]";
         }
         json += "]}";
         return json;
      }

      bool ParseSnapshot(const String &text, Snapshot &snapshot)
      {
         JsonValue document;
         std::string error;
         if (!JsonValue::Parse(std::string(Utf8(text).c_str()), document, error) || !document.IsObject())
            return false;

         snapshot.profile = Wide(document.GetString("profile"));
         snapshot.applied = document.GetInt64("applied");

         const JsonValue *entries = document.Get("entries");
         if (!entries || !entries->IsArray() || snapshot.profile.IsEmpty())
            return false;

         for (const JsonValue &item : entries->Items())
         {
            if (!item.IsArray() || item.Size() != 5)
               return false;

            const JsonValue *store = item.At(0);
            const JsonValue *name = item.At(1);
            const JsonValue *before = item.At(2);
            const JsonValue *after = item.At(3);
            const JsonValue *had = item.At(4);
            if (!store || !store->IsString() || !name || !name->IsString() || !before || !before->IsString() ||
                !after || !after->IsString() || !had || !had->IsNumber())
               return false;

            SnapshotEntry entry;
            entry.ini = store->AsString() == "i";
            entry.name = Wide(name->AsString());
            entry.before = Wide(before->AsString());
            entry.after = Wide(after->AsString());
            entry.had = had->AsInt64() != 0;
            snapshot.entries.push_back(entry);
         }

         return true;
      }

      String AppliedRecord(const String &profile, __int64 applied, const String &state, __int64 changed, const std::vector<String> &set)
      {
         AnsiString json = "{\"profile\":" + JsonText(profile) + ",\"applied\":" + JsonNumber(applied) +
                           ",\"state\":" + JsonText(state) + ",\"changed\":" + JsonNumber(changed) + ",\"set\":[";
         for (size_t i = 0; i < set.size(); i++)
         {
            if (i > 0)
               json += ",";
            json += JsonText(set[i]);
         }
         json += "]}";
         return Wide(std::string(json.c_str()));
      }

      Profiles::Applied ParseApplied(const String &text)
      {
         Profiles::Applied record;
         if (text.IsEmpty())
            return record;

         JsonValue document;
         std::string error;
         if (!JsonValue::Parse(std::string(Utf8(text).c_str()), document, error) || !document.IsObject())
         {
            // Something is recorded even if it cannot be read: --once must not take an
            // unreadable record for none, and apply the profile a second time.
            record.profile = _T("(unreadable)");
            record.state = _T("applied");
            record.readable = false;
            return record;
         }

         record.profile = Wide(document.GetString("profile"));
         record.state = Wide(document.GetString("state", "applied"));
         record.applied = document.GetInt64("applied");
         record.changed = document.GetInt64("changed", record.applied);

         const JsonValue *set = document.Get("set");
         if (set && set->IsArray())
         {
            for (const JsonValue &item : set->Items())
            {
               if (item.IsString())
                  record.set.push_back(Wide(item.AsString()));
            }
         }
         return record;
      }

      // ---- the answers ---------------------------------------------------------

      AnsiString AppliedJson(const Profiles::Applied &record)
      {
         if (!record.Recorded())
            return "null";

         const ProfilesData::Profile *definition = FindProfile(record.profile);
         return "{\"profile\":" + JsonText(record.profile) + ",\"title\":" + JsonText(definition ? String(definition->title) : record.profile) +
                ",\"applied\":" + JsonNumber(record.applied) + ",\"state\":" + JsonText(record.state) + ",\"changed\":" + JsonNumber(record.changed) + "}";
      }

      // The way back as the list and the diff answer it: null when none is held,
      // {readable:false} when one is held and cannot be read - which a page offers
      // to discard, since nothing else can be done with it.
      AnsiString SnapshotSummaryJson(bool recordRead, const String &text)
      {
         if (!recordRead)
            return "{\"readable\":false}";
         if (text.IsEmpty())
            return "null";

         Snapshot snapshot;
         if (!ParseSnapshot(text, snapshot))
            return "{\"readable\":false}";

         return "{\"readable\":true,\"profile\":" + JsonText(snapshot.profile) + ",\"applied\":" + JsonNumber(snapshot.applied) +
                ",\"entries\":" + JsonNumber((__int64) snapshot.entries.size()) + "}";
      }

      const TCHAR *PurgeNote()
      {
         return _T("The next sweep deletes what the shorter window no longer keeps; a revert puts the number back, not what was deleted.");
      }

      AnsiString EntryJson(const Evaluated &evaluated)
      {
         const ProfilesData::Entry &entry = *evaluated.entry;
         const char *caution = CautionWord(entry.caution);

         AnsiString json = "{\"store\":\"" + AnsiString(StoreWord(evaluated.ini)) + "\"";
         json += ",\"name\":" + JsonText(evaluated.name);
         json += ",\"value\":" + JsonText(evaluated.value);
         json += ",\"current\":" + JsonText(evaluated.current);
         json += ",\"stored\":" + JsonBool(evaluated.stored);
         json += ",\"changes\":" + JsonBool(evaluated.changes);
         json += ",\"why\":" + JsonText(entry.why);
         json += ",\"caution\":" + (caution ? "\"" + AnsiString(caution) + "\"" : AnsiString("null"));
         json += ",\"confirm\":" + JsonBool(evaluated.confirm);
         json += ",\"note\":" + (entry.caution == ProfilesData::CautionPurges && evaluated.confirm ? JsonText(PurgeNote()) : AnsiString("null"));
         json += ",\"route\":" + JsonTextOrNull(evaluated.route);
         json += ",\"applies\":" + JsonTextOrNull(evaluated.applies);
         json += ",\"levels\":" + JsonTextOrNull(evaluated.levels);
         json += ",\"environment\":" + JsonTextOrNull(evaluated.environment);
         json += ",\"refused\":" + JsonTextOrNull(evaluated.refused);
         json += "}";
         return json;
      }

      // An entry that asks for the confirmation, as the diff and a refused apply
      // both list it.
      AnsiString ConfirmationJson(const Evaluated &evaluated)
      {
         return "{\"store\":\"" + AnsiString(StoreWord(evaluated.ini)) + "\",\"name\":" + JsonText(evaluated.name) +
                ",\"caution\":\"" + AnsiString(CautionWord(evaluated.entry->caution)) + "\",\"current\":" + JsonText(evaluated.current) +
                ",\"value\":" + JsonText(evaluated.value) + "}";
      }

      struct Counts
      {
         Counts() : changes(0), unchanged(0), skipped(0), refused(0), confirm(0) {}

         int changes;
         int unchanged;
         int skipped;
         int refused;
         int confirm;
      };

      Counts Count(const std::vector<Evaluated> &all)
      {
         Counts counts;
         for (const Evaluated &evaluated : all)
         {
            if (!evaluated.refused.IsEmpty())
               counts.refused++;
            else if (!evaluated.environment.IsEmpty())
               counts.skipped++;
            else if (evaluated.changes)
               counts.changes++;
            else
               counts.unchanged++;

            if (evaluated.confirm)
               counts.confirm++;
         }
         return counts;
      }

      AnsiString StepsJson(const ProfilesData::Profile &profile)
      {
         AnsiString json = "[";
         for (size_t i = 0; i < profile.stepCount; i++)
         {
            if (i > 0)
               json += ",";
            json += JsonText(profile.steps[i]);
         }
         return json + "]";
      }

      // ---- the writes ----------------------------------------------------------

      struct Written
      {
         Written() : ini(false) {}

         bool ini;
         String name;
         String before;
         String after;
         String result;   // set, unchanged, skipped or failed; restored or left
         String reason;
         String applies;
      };

      AnsiString WrittenJson(const Written &written)
      {
         return "{\"store\":\"" + AnsiString(StoreWord(written.ini)) + "\",\"name\":" + JsonText(written.name) +
                ",\"before\":" + JsonText(written.before) + ",\"after\":" + JsonText(written.after) +
                ",\"result\":" + JsonText(written.result) + ",\"reason\":" + JsonTextOrNull(written.reason) +
                ",\"applies\":" + JsonTextOrNull(written.applies) + "}";
      }

      // Writes one value through the route the diff named: the REST row's setter where
      // a row keeps the setting, the settings store's locked write for a [Settings]
      // key no row keeps. The environment's refusal - a record written since the diff
      // was read - is a skip with its sentence, not a failure.
      bool WriteValue(bool ini, const String &name, const String &value, String &problem, bool &refusedByEnvironment)
      {
         problem.Empty();
         refusedByEnvironment = false;

         try
         {
            RestApiSettingsRows::Lent lent;
            if (RestApiSettingsRows::Find(ini, name, lent))
            {
               String refusal;
               if (!RestApiSettingsRows::Apply(ini, name, (long) LeadingNumber(value), value, refusal))
               {
                  problem = refusal;
                  return false;
               }
               return true;
            }

            if (!ini)
            {
               problem = Formatter::Format("No REST settings row writes {0}.", name);
               return false;
            }

            if (!IniFileSettings::Instance()->WriteSettingsValue(name, value))
            {
               problem = Formatter::Format("The database could not be written, so {0} was not stored.", name);
               return false;
            }

            return true;
         }
         catch (const EnvironmentSettings::Refusal &refusal)
         {
            refusedByEnvironment = true;
            problem = refusal.Sentence();
            return false;
         }
      }

      String Plural(int count, const TCHAR *one, const TCHAR *many)
      {
         return StringParser::IntToString(count) + _T(" ") + (count == 1 ? one : many);
      }

      // What a refused apply says about the entries that stop it.
      String NamesOf(const std::vector<const Evaluated *> &entries, bool withCaution)
      {
         std::vector<String> names;
         for (const Evaluated *evaluated : entries)
         {
            if (withCaution)
               names.push_back(Formatter::Format("{0} ({1}, {2} to {3})", evaluated->name, String(CautionSentence(evaluated->entry->caution)),
                  evaluated->current, evaluated->value));
            else
               names.push_back(evaluated->name + _T(": ") + evaluated->refused);
         }
         return StringParser::JoinVector(names, _T("; "));
      }

      // The result of an apply, a revert or a discard, for the answer and the lines
      // the verb prints.
      struct Report
      {
         Report() : time(0), set(0), unchanged(0), skipped(0), failed(0), waitReinitialize(0), waitRestart(0), stale(false) {}

         String profile;
         __int64 time;
         std::vector<Written> written;
         int set;
         int unchanged;
         int skipped;
         int failed;
         int waitReinitialize;
         int waitRestart;
         // The diff the caller brought is not the one the server answers now.
         bool stale;
         std::vector<const Evaluated *> needConfirmation;
         std::vector<const Evaluated *> refused;
      };

      void CountWait(Report &report, const String &applies)
      {
         if (applies == _T("reinitialize"))
            report.waitReinitialize++;
         else if (applies == _T("restart"))
            report.waitRestart++;
      }

      Profiles::Outcome ApplyLocked(const String &name, bool confirmed, const String &token, Report &report, std::vector<Evaluated> &all, String &sentence)
      {
         const ProfilesData::Profile *profile = FindProfile(name);
         if (!profile)
         {
            sentence = Formatter::Format("There is no profile named {0}. The profiles are office, gateway, relay and sender.", name);
            return Profiles::OutcomeUnknown;
         }

         report.profile = profile->name;

         std::shared_ptr<PropertySet> properties = Configuration::Instance()->GetPropertySet();
         if (!properties)
         {
            sentence = _T("The server's configuration is not loaded, so nothing was applied.");
            return Profiles::OutcomeFailed;
         }

         // One snapshot, the latest: a second apply would replace the way back to the
         // settings before the first, which is the one thing the snapshot is for.
         String held;
         if (!ReadRecord(Profiles::SnapshotName(), held))
         {
            sentence = _T("The profile's records could not be read from the database, so nothing was applied.");
            return Profiles::OutcomeFailed;
         }

         if (!held.IsEmpty())
         {
            Snapshot snapshot;
            if (ParseSnapshot(held, snapshot))
               sentence = Formatter::Format("The {0} profile, applied {1}, is held as the way back to the settings before it. Revert it (POST /api/v1/settings/profiles/revert) or discard its way back (POST /api/v1/settings/profiles/discard) before applying another; nothing was applied.",
                  snapshot.profile, UtcText(snapshot.applied));
            else
               sentence = _T("A way back is held from an earlier profile, and it cannot be read; discard it (POST /api/v1/settings/profiles/discard) before applying another. Nothing was applied.");
            return Profiles::OutcomeRefused;
         }

         if (!EvaluateAll(*profile, all))
         {
            sentence = _T("The settings store could not be read, so whether each [Settings] key is stored - which the way back needs - is not known; nothing was applied.");
            return Profiles::OutcomeFailed;
         }

         // The confirmation is of the diff the caller showed: when the server no
         // longer answers the same one, what was confirmed is not what would be done.
         if (!token.IsEmpty() && token.CompareNoCase(DiffToken(*profile, all)) != 0)
         {
            report.stale = true;
            sentence = Formatter::Format("The settings changed since the diff of the {0} profile was read, so it is not the one this apply would make. Look at the diff again, and apply from it; nothing was applied.", String(profile->name));
            return Profiles::OutcomeRefused;
         }

         for (const Evaluated &evaluated : all)
         {
            if (!evaluated.refused.IsEmpty())
               report.refused.push_back(&evaluated);
            if (evaluated.confirm)
               report.needConfirmation.push_back(&evaluated);
         }

         // All or nothing: a profile applied in part is a shape nobody chose.
         if (!report.refused.empty())
         {
            sentence = Formatter::Format("{0} of the {1} profile would be refused by the setting it writes, so nothing was applied: {2}.",
               Plural((int) report.refused.size(), _T("entry"), _T("entries")), name, NamesOf(report.refused, false));
            return Profiles::OutcomeRefused;
         }

         // A listener closed, an address range trusted, a filter turned off or data
         // deleted is never done silently to a server already in service.
         if (!report.needConfirmation.empty() && !confirmed)
         {
            sentence = Formatter::Format("Applying the {0} profile here {1}: {2}. Nothing was applied; apply it again with the confirmation to go ahead.",
               String(profile->name),
               report.needConfirmation.size() == 1 ? String(_T("changes one entry that asks for a confirmation"))
                                                   : Formatter::Format("changes {0} entries that ask for a confirmation", (int) report.needConfirmation.size()),
               NamesOf(report.needConfirmation, true));
            return Profiles::OutcomeRefused;
         }

         // The way back, written before any setting is: whatever stops the writes part
         // way, what was changed can be put back.
         Snapshot snapshot;
         snapshot.profile = profile->name;
         snapshot.applied = Now();
         for (const Evaluated &evaluated : all)
         {
            if (!evaluated.changes || !evaluated.environment.IsEmpty())
               continue;

            SnapshotEntry entry;
            entry.ini = evaluated.ini;
            entry.name = evaluated.name;
            entry.before = evaluated.ini ? (evaluated.stored ? evaluated.storedText : String()) : evaluated.current;
            entry.had = evaluated.ini ? evaluated.stored : true;
            entry.after = evaluated.value;
            snapshot.entries.push_back(entry);
         }

         // Characters, as the column counts them.
         if (Wide(std::string(SnapshotJson(snapshot).c_str())).GetLength() > MaximumRecord)
         {
            sentence = Formatter::Format("The way back from the {0} profile would be longer than the 4000 characters its record holds - some of the values it would replace are very long - so nothing was applied.", name);
            return Profiles::OutcomeRefused;
         }

         report.time = snapshot.applied;

         if (!snapshot.entries.empty())
         {
            if (!WriteRecord(Profiles::SnapshotName(), Wide(std::string(SnapshotJson(snapshot).c_str()))))
            {
               sentence = _T("The way back could not be written to the database (HM7301 in the error log), so nothing was applied.");
               report.time = 0;
               return Profiles::OutcomeFailed;
            }
         }

         std::vector<String> setNames;
         for (Evaluated &evaluated : all)
         {
            Written written;
            written.ini = evaluated.ini;
            written.name = evaluated.name;
            written.before = evaluated.current;
            written.after = evaluated.current;
            written.applies = evaluated.applies;

            if (!evaluated.environment.IsEmpty())
            {
               EnvironmentSettings::Entry managed;
               EnvironmentSettings::IsManaged(evaluated.ini ? EnvironmentSettings::StoreIni : EnvironmentSettings::StoreDatabase, evaluated.name, managed);
               written.result = _T("skipped");
               written.reason = EnvironmentSettings::RefusalSentence(evaluated.name, managed);
               report.skipped++;
               report.written.push_back(written);
               continue;
            }

            if (!evaluated.changes)
            {
               written.result = _T("unchanged");
               report.unchanged++;
               report.written.push_back(written);
               continue;
            }

            String problem;
            bool byEnvironment = false;
            if (!WriteValue(evaluated.ini, evaluated.name, evaluated.value, problem, byEnvironment))
            {
               written.result = byEnvironment ? _T("skipped") : _T("failed");
               written.reason = problem;
               if (byEnvironment)
                  report.skipped++;
               else
                  report.failed++;

               // Nothing changed, so there is nothing to put back for it.
               for (auto entry = snapshot.entries.begin(); entry != snapshot.entries.end(); entry++)
               {
                  if (entry->ini == evaluated.ini && entry->name.CompareNoCase(evaluated.name) == 0)
                  {
                     snapshot.entries.erase(entry);
                     break;
                  }
               }

               report.written.push_back(written);
               LOG_APPLICATION(Formatter::Format("Profile {0}: {1} was not written: {2}", String(profile->name), evaluated.name, problem));
               continue;
            }

            ReadCurrent(evaluated);
            written.after = evaluated.current;
            written.result = _T("set");
            report.set++;
            setNames.push_back(StoreKey(evaluated.ini, evaluated.name));
            CountWait(report, evaluated.applies);

            // Read back: a value that is not the profile's - a [SettingsOverride]
            // forcing the key, or a setter that canonicalises - is said, and the
            // diff answers it as changing still.
            if (!SameValue(evaluated, evaluated.current, evaluated.value))
               written.reason = Formatter::Format("Stored, and {0} is in force: the settings file's [SettingsOverride] or the setting's own rule decides it.",
                  evaluated.current.IsEmpty() ? String(_T("an empty value")) : evaluated.current);

            // What the setter left, which is what the way back asks the setting still
            // to hold before it puts the old value back.
            for (SnapshotEntry &entry : snapshot.entries)
            {
               if (entry.ini == evaluated.ini && entry.name.CompareNoCase(evaluated.name) == 0)
                  entry.after = evaluated.current;
            }

            report.written.push_back(written);
            LOG_APPLICATION(Formatter::Format("Profile {0}: {1} ({2}) set to {3}, was {4}.", String(profile->name), evaluated.name,
               String(StoreWord(evaluated.ini)), written.after.IsEmpty() ? String(_T("empty")) : written.after,
               written.before.IsEmpty() ? String(_T("empty")) : written.before));
         }

         // The way back as the writes left it; nothing to go back to is no way back.
         bool recordsWritten = true;
         if (!WriteRecord(Profiles::SnapshotName(), snapshot.entries.empty() ? String() : Wide(std::string(SnapshotJson(snapshot).c_str()))))
            recordsWritten = false;

         // The record after the writes, so that it says what they did: failed when
         // any write failed - which --once does not take for applied, so a container
         // that stopped on it tries again at its next start, and is told of the way
         // back it has to revert or discard first.
         const String state = report.failed > 0 ? _T("failed") : _T("applied");
         if (!WriteRecord(Profiles::AppliedName(), AppliedRecord(profile->name, snapshot.applied, state, snapshot.applied, setNames)))
            recordsWritten = false;

         AuditTrail::Instance()->RecordEvent(_T("profile"), profile->name, _T("applied"),
            Formatter::Format("{0} entries: {1} set, {2} unchanged, {3} skipped because the environment sets them, {4} failed",
               (int) all.size(), report.set, report.unchanged, report.skipped, report.failed));

         LOG_APPLICATION(Formatter::Format("Profile {0} applied: {1} of its {2} entries set, {3} unchanged, {4} skipped",
            String(profile->name), report.set, (int) all.size(), report.unchanged, report.skipped) +
            Formatter::Format(", {0} failed.", report.failed));

         if (report.failed > 0)
         {
            sentence = Formatter::Format("{0} of the {1} profile could not be written; the rest were, and the way back holds what was changed.",
               Plural(report.failed, _T("entry"), _T("entries")), String(profile->name));
            if (!recordsWritten)
               sentence += _T(" The profile's records could not be written either (HM7301 in the error log).");
            return Profiles::OutcomeFailed;
         }

         if (!recordsWritten)
         {
            sentence = Formatter::Format("The {0} profile's settings were written, but its records could not be updated (HM7301 in the error log): the way back saved before the writes is the one held.",
               String(profile->name));
            return Profiles::OutcomeFailed;
         }

         return Profiles::OutcomeDone;
      }

      AnsiString ReportJson(const Report &report, const char *timeField, const String &message)
      {
         AnsiString json = "{\"profile\":" + JsonText(report.profile) + ",\"" + AnsiString(timeField) + "\":" + JsonNumber(report.time) + ",\"entries\":[";
         for (size_t i = 0; i < report.written.size(); i++)
         {
            if (i > 0)
               json += ",";
            json += WrittenJson(report.written[i]);
         }
         json += "]";
         json += ",\"set\":" + JsonNumber(report.set);
         json += ",\"unchanged\":" + JsonNumber(report.unchanged);
         json += ",\"skipped\":" + JsonNumber(report.skipped);
         json += ",\"failed\":" + JsonNumber(report.failed);
         json += ",\"waiting\":{\"reinitialize\":" + JsonNumber(report.waitReinitialize) + ",\"restart\":" + JsonNumber(report.waitRestart) + "}";
         json += ",\"message\":" + JsonText(message);
         json += "}";
         return json;
      }

      String WaitingSentence(const Report &report, const TCHAR *what)
      {
         if (report.waitReinitialize == 0 && report.waitRestart == 0)
            return Formatter::Format("Every value {0} is in effect now.", String(what));

         std::vector<String> parts;
         if (report.waitReinitialize > 0)
            parts.push_back(Formatter::Format("{0} until the server is reinitialised (Apply now)", Plural(report.waitReinitialize, _T("waits"), _T("wait"))));
         if (report.waitRestart > 0)
            parts.push_back(Formatter::Format("{0} until the service is restarted", Plural(report.waitRestart, _T("waits"), _T("wait"))));
         return Formatter::Format("Of the values {0}, ", String(what)) + StringParser::JoinVector(parts, _T(", and ")) + _T(".");
      }

      // The lines the command line prints for a report.
      void PrintWritten(const Report &report, void (*print)(bool, const String &))
      {
         for (const Written &written : report.written)
         {
            const String label = written.name + _T(" (") + String(StoreWord(written.ini)) + _T(")");
            if (written.result == _T("set") || written.result == _T("restored"))
               print(false, Formatter::Format("{0}: {1} to {2} (was {3}){4}", label, String(written.result == _T("set") ? _T("set") : _T("put back")),
                  written.after.IsEmpty() ? String(_T("empty")) : written.after,
                  written.before.IsEmpty() ? String(_T("empty")) : written.before,
                  written.reason.IsEmpty() ? String() : String(_T(" - ")) + written.reason));
            else if (written.result == _T("unchanged"))
               print(false, label + _T(": unchanged"));
            else
               print(written.result == _T("failed"), label + _T(": ") + written.result + _T(": ") + written.reason);
         }
      }
   }

   const TCHAR *
   Profiles::SnapshotName()
   {
      return _T("ProfileSnapshot");
   }

   const TCHAR *
   Profiles::AppliedName()
   {
      return _T("ProfileApplied");
   }

   void
   Profiles::EnsureRecords(PropertySet &properties)
   {
      properties.EnsureString(SnapshotName(), String());
      properties.EnsureString(AppliedName(), String());
   }

   Profiles::Applied
   Profiles::ReadAppliedRecord()
   {
      String text;
      if (!ReadRecord(AppliedName(), text))
      {
         // Unread is not none: --once must not apply a profile a second time because
         // the database did not answer.
         Applied record;
         record.profile = _T("(unreadable)");
         record.state = _T("applied");
         record.readable = false;
         return record;
      }

      return ParseApplied(text);
   }

   void
   Profiles::ReadApplied(String &profile, __int64 &applied, String &state, __int64 &changed)
   {
      const Applied record = ReadAppliedRecord();
      profile = record.profile;
      state = record.state;
      applied = record.applied;
      changed = record.changed;
   }

   String
   Profiles::AppliedProfile()
   {
      const Applied record = ReadAppliedRecord();
      if (record.profile.IsEmpty() || !FindProfile(record.profile))
         return String();

      // In effect: applied and not reverted, or kept. A failed apply is in effect
      // in part, and the way back it holds says which part.
      if (record.state == _T("applied") || record.state == _T("kept") || record.state == _T("failed"))
         return record.profile;

      return String();
   }

   bool
   Profiles::SourceOf(const Applied &record, const String &store, const String &name, const String &value, String &profile)
   {
      profile.Empty();
      if (record.profile.IsEmpty() || !(record.state == _T("applied") || record.state == _T("kept") || record.state == _T("failed")))
         return false;

      const ProfilesData::Profile *definition = FindProfile(record.profile);
      if (!definition)
         return false;

      const bool ini = store == _T("ini");

      // Only a setting the apply wrote: one it found already holding the value was
      // not set by it, whatever the value is.
      const String key = StoreKey(ini, name);
      bool written = false;
      for (const String &candidate : record.set)
      {
         if (candidate.CompareNoCase(key) == 0)
         {
            written = true;
            break;
         }
      }
      if (!written)
         return false;

      for (size_t i = 0; i < definition->entryCount; i++)
      {
         const ProfilesData::Entry &entry = definition->entries[i];
         if ((entry.store == ProfilesData::StoreIni) != ini || name.CompareNoCase(entry.name) != 0)
            continue;

         Evaluated shape;
         shape.integer = ini ? (FindIniKey(name) && FindIniKey(name)->IsInteger)
                             : (FindProperty(name) && FindProperty(name)->Kind == SettingsDefaults::KindNumber);
         if (!SameValue(shape, value, entry.value))
            return false;

         profile = String(definition->name);
         return true;
      }

      return false;
   }

   AnsiString
   Profiles::ListJson()
   {
      String snapshotText;
      const bool snapshotRead = ReadRecord(SnapshotName(), snapshotText);
      const Applied record = ReadAppliedRecord();

      AnsiString json = "{\"profiles\":[";
      for (size_t i = 0; i < ProfilesData::ProfileCount; i++)
      {
         const ProfilesData::Profile &profile = ProfilesData::Profiles[i];
         std::vector<Evaluated> all;
         EvaluateAll(profile, all);
         const Counts counts = Count(all);

         if (i > 0)
            json += ",";
         json += "{\"name\":" + JsonText(profile.name) + ",\"title\":" + JsonText(profile.title) + ",\"summary\":" + JsonText(profile.summary);
         json += ",\"includes\":" + JsonTextOrNull(profile.includes);
         json += ",\"entries\":" + JsonNumber((__int64) profile.entryCount);
         json += ",\"changes\":" + JsonNumber(counts.changes);
         json += ",\"confirmations\":" + JsonNumber(counts.confirm);
         json += ",\"skipped\":" + JsonNumber(counts.skipped);
         json += ",\"refused\":" + JsonNumber(counts.refused) + "}";
      }
      json += "],\"applied\":" + AppliedJson(record) + ",\"snapshot\":" + SnapshotSummaryJson(snapshotRead, snapshotText) + "}";
      return json;
   }

   bool
   Profiles::DiffJson(const String &name, AnsiString &json)
   {
      const ProfilesData::Profile *profile = FindProfile(name);
      if (!profile)
         return false;

      std::vector<Evaluated> all;
      const bool storeRead = EvaluateAll(*profile, all);
      const Counts counts = Count(all);

      String held;
      const bool snapshotRead = ReadRecord(SnapshotName(), held);
      const Applied record = ReadAppliedRecord();

      json = "{\"name\":" + JsonText(profile->name) + ",\"title\":" + JsonText(profile->title) + ",\"summary\":" + JsonText(profile->summary);
      json += ",\"includes\":" + JsonTextOrNull(profile->includes);
      json += ",\"steps\":" + StepsJson(*profile);
      json += ",\"entries\":[";
      for (size_t i = 0; i < all.size(); i++)
      {
         if (i > 0)
            json += ",";
         json += EntryJson(all[i]);
      }
      json += "]";
      json += ",\"changes\":" + JsonNumber(counts.changes);
      json += ",\"unchanged\":" + JsonNumber(counts.unchanged);
      json += ",\"skipped\":" + JsonNumber(counts.skipped);
      json += ",\"refused\":" + JsonNumber(counts.refused);
      json += ",\"confirmation_needed\":[";
      bool first = true;
      for (const Evaluated &evaluated : all)
      {
         if (!evaluated.confirm)
            continue;
         if (!first)
            json += ",";
         first = false;
         json += ConfirmationJson(evaluated);
      }
      json += "]";
      json += ",\"diff_token\":" + JsonText(DiffToken(*profile, all));

      // Whether an apply would be taken now, and why not when it would not.
      String blocked;
      if (!snapshotRead || !held.IsEmpty())
         blocked = _T("A way back from an earlier profile is held: revert it or discard it before applying another.");
      else if (!storeRead)
         blocked = _T("The settings store could not be read, so this diff cannot say which [Settings] keys are stored; an apply would be refused.");
      else if (counts.refused > 0)
         blocked = _T("An entry would be refused by the setting it writes, so the profile cannot be applied as it is.");

      json += ",\"applicable\":" + JsonBool(blocked.IsEmpty());
      json += ",\"blocked\":" + JsonTextOrNull(blocked);
      json += ",\"applied\":" + AppliedJson(record) + ",\"snapshot\":" + SnapshotSummaryJson(snapshotRead, held) + "}";
      return true;
   }

   Profiles::Outcome
   Profiles::Apply(const String &name, bool confirmed, const String &token, AnsiString &json, String &sentence)
   {
      sentence.Empty();
      if (Reentered_(_T("applied"), sentence, json))
         return OutcomeRefused;

      ProfilesLock guard;

      Report report;
      std::vector<Evaluated> all;

      const Outcome outcome = ApplyLocked(name, confirmed, token, report, all, sentence);

      if (outcome == OutcomeDone || (outcome == OutcomeFailed && report.time != 0))
      {
         const String message = outcome == OutcomeDone
            ? Formatter::Format("The {0} profile: {1} set, {2} unchanged, {3} skipped. {4}", report.profile,
                 report.set, report.unchanged, report.skipped, WaitingSentence(report, _T("set")))
            : sentence;
         json = ReportJson(report, "applied", message);
         return outcome;
      }

      // Refused: what stops it, beside the sentence. confirmation is true when a
      // confirmation is all that stands in the way; stale when the diff the caller
      // brought is not the one the server answers now.
      const bool confirmationOnly = outcome == OutcomeRefused && !report.needConfirmation.empty() && report.refused.empty() && !confirmed && !report.stale;
      json = "{\"confirmation\":" + JsonBool(confirmationOnly) + ",\"stale\":" + JsonBool(report.stale) + ",\"confirmation_needed\":[";
      for (size_t i = 0; i < report.needConfirmation.size(); i++)
      {
         if (i > 0)
            json += ",";
         json += ConfirmationJson(*report.needConfirmation[i]);
      }
      json += "],\"refused\":[";
      for (size_t i = 0; i < report.refused.size(); i++)
      {
         const Evaluated &evaluated = *report.refused[i];
         if (i > 0)
            json += ",";
         json += "{\"store\":\"" + AnsiString(StoreWord(evaluated.ini)) + "\",\"name\":" + JsonText(evaluated.name) +
                 ",\"reason\":" + JsonText(evaluated.refused) + "}";
      }
      String held;
      const bool snapshotRead = ReadRecord(SnapshotName(), held);
      json += "],\"snapshot\":" + SnapshotSummaryJson(snapshotRead, held) + "}";

      return outcome;
   }

   Profiles::Outcome
   Profiles::Revert(AnsiString &json, String &sentence)
   {
      if (Reentered_(_T("reverted"), sentence, json))
         return OutcomeRefused;

      ProfilesLock guard;

      sentence.Empty();
      String held;
      if (!ReadRecord(SnapshotName(), held))
      {
         sentence = _T("The profile's records could not be read from the database, so nothing was put back.");
         json = "{\"snapshot\":{\"readable\":false}}";
         return OutcomeFailed;
      }

      if (held.IsEmpty())
      {
         sentence = _T("There is nothing to revert: no profile has been applied since the last revert or discard.");
         json = "{\"snapshot\":null}";
         return OutcomeRefused;
      }

      Snapshot snapshot;
      if (!ParseSnapshot(held, snapshot))
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, ErrorSnapshotUnreadable, "Profiles::Revert",
            _T("The way back from the last profile (the hm_settings row ProfileSnapshot) cannot be read, so it cannot be put back. Discard it and set the settings by hand; the audit trail names each value the profile replaced."));
         sentence = _T("The way back cannot be read (HM7300 in the error log), so nothing was put back. Discard it and set the settings by hand; the audit trail names each value the profile replaced.");
         json = "{\"snapshot\":{\"readable\":false}}";
         return OutcomeFailed;
      }

      Report report;
      report.profile = snapshot.profile;
      report.time = Now();

      for (const SnapshotEntry &entry : snapshot.entries)
      {
         Evaluated now;
         now.ini = entry.ini;
         now.name = entry.name;
         now.integer = entry.ini ? (FindIniKey(entry.name) && FindIniKey(entry.name)->IsInteger)
                                 : (FindProperty(entry.name) && FindProperty(entry.name)->Kind == SettingsDefaults::KindNumber);

         Written written;
         written.ini = entry.ini;
         written.name = entry.name;
         written.applies = AppliesOf(entry.ini, entry.name);

         if (!ReadCurrent(now))
         {
            written.result = _T("failed");
            written.reason = Formatter::Format("The settings store could not be read, so {0} was not put back.", entry.name);
            report.failed++;
            report.written.push_back(written);
            continue;
         }

         written.before = now.current;
         written.after = now.current;

         EnvironmentSettings::Entry managed;
         if (EnvironmentSettings::IsManaged(entry.ini ? EnvironmentSettings::StoreIni : EnvironmentSettings::StoreDatabase, entry.name, managed))
         {
            written.result = _T("left");
            written.reason = EnvironmentSettings::RefusalSentence(entry.name, managed);
            report.skipped++;
            report.written.push_back(written);
            continue;
         }

         // Only a value the profile left: one an administrator has changed since is
         // theirs, and the way back does not undo it.
         if (!SameValue(now, now.current, entry.after))
         {
            written.result = _T("left");
            written.reason = Formatter::Format("{0} has been changed since the profile set it ({1}, now {2}), so it is left as it is.",
               entry.name, entry.after, now.current);
            report.skipped++;
            report.written.push_back(written);
            continue;
         }

         bool ok = true;
         String problem;
         bool byEnvironment = false;

         if (entry.ini)
         {
            // A [Settings] key goes back byte for byte - the text that was stored, or
            // no row at all when there was none - through the settings store's locked
            // write, which is what every door to the store takes.
            try
            {
               if (!entry.had)
                  ok = IniFileSettings::Instance()->RemoveSettingsValue(entry.name);
               else
                  ok = IniFileSettings::Instance()->WriteSettingsValue(entry.name, entry.before);
               if (!ok)
                  problem = Formatter::Format("The settings store could not be written, so {0} was not put back.", entry.name);
            }
            catch (const EnvironmentSettings::Refusal &refusal)
            {
               ok = false;
               byEnvironment = true;
               problem = refusal.Sentence();
            }
         }
         else
            ok = WriteValue(false, entry.name, entry.before, problem, byEnvironment);

         if (!ok)
         {
            written.result = byEnvironment ? _T("left") : _T("failed");
            written.reason = problem;
            if (byEnvironment)
               report.skipped++;
            else
               report.failed++;
            report.written.push_back(written);
            continue;
         }

         ReadCurrent(now);
         written.after = now.current;
         written.result = _T("restored");
         report.set++;
         CountWait(report, written.applies);
         report.written.push_back(written);

         LOG_APPLICATION(Formatter::Format("Profile {0} reverted: {1} ({2}) put back to {3}.", snapshot.profile, entry.name,
            String(StoreWord(entry.ini)), entry.had ? (entry.before.IsEmpty() ? String(_T("empty")) : entry.before) : String(_T("its default"))));
      }

      bool recordsWritten = true;
      if (report.failed > 0)
      {
         // What failed can be tried again: the way back is kept, less what went back.
         Snapshot remaining = snapshot;
         remaining.entries.clear();
         for (const SnapshotEntry &entry : snapshot.entries)
         {
            for (const Written &written : report.written)
            {
               if (written.result == _T("failed") && written.ini == entry.ini && written.name.CompareNoCase(entry.name) == 0)
                  remaining.entries.push_back(entry);
            }
         }
         recordsWritten = WriteRecord(SnapshotName(), Wide(std::string(SnapshotJson(remaining).c_str())));
         sentence = Formatter::Format("{0} could not be put back; the rest were, and the way back now holds what is left.",
            Plural(report.failed, _T("setting"), _T("settings")));
         if (!recordsWritten)
            sentence += _T(" The way back could not be rewritten either (HM7301 in the error log), so it still names every setting.");
      }
      else
      {
         const Applied record = ReadAppliedRecord();

         // The record first: a revert whose record could not be written leaves the
         // way back where it was, so that nothing claims a revert that is not
         // recorded.
         recordsWritten = WriteRecord(AppliedName(), AppliedRecord(snapshot.profile, record.applied != 0 ? record.applied : snapshot.applied, _T("reverted"), report.time, std::vector<String>())) &&
                          WriteRecord(SnapshotName(), String());
         if (!recordsWritten)
            sentence = _T("Every setting was put back, but the profile's records could not be updated (HM7301 in the error log); discard the way back, which now names values already restored.");
      }

      AuditTrail::Instance()->RecordEvent(_T("profile"), snapshot.profile, _T("reverted"),
         Formatter::Format("{0} entries put back, {1} left as they were, {2} failed", report.set, report.skipped, report.failed));

      LOG_APPLICATION(Formatter::Format("Profile {0} reverted: {1} put back, {2} left as they were, {3} failed.", snapshot.profile, report.set, report.skipped, report.failed));

      const bool failed = report.failed > 0 || !recordsWritten;
      const String message = failed ? sentence
         : Formatter::Format("The {0} profile is reverted: {1} put back, {2} left as they were. {3}", snapshot.profile,
              Plural(report.set, _T("setting"), _T("settings")), report.skipped, WaitingSentence(report, _T("put back")));

      json = ReportJson(report, "reverted", message);
      return failed ? OutcomeFailed : OutcomeDone;
   }

   Profiles::Outcome
   Profiles::Discard(AnsiString &json, String &sentence)
   {
      if (Reentered_(_T("discarded"), sentence, json))
         return OutcomeRefused;

      ProfilesLock guard;

      sentence.Empty();
      String held;
      if (!ReadRecord(SnapshotName(), held))
      {
         sentence = _T("The profile's records could not be read from the database, so nothing was discarded.");
         json = "{\"snapshot\":{\"readable\":false}}";
         return OutcomeFailed;
      }

      if (held.IsEmpty())
      {
         sentence = _T("There is no way back to discard: no profile has been applied since the last revert or discard.");
         json = "{\"snapshot\":null}";
         return OutcomeRefused;
      }

      Snapshot snapshot;
      const bool readable = ParseSnapshot(held, snapshot);
      const Applied record = ReadAppliedRecord();

      const __int64 now = Now();
      const String name = readable ? snapshot.profile : (record.readable ? record.profile : String());

      if (!WriteRecord(SnapshotName(), String()))
      {
         sentence = _T("The way back could not be discarded: the database did not take the change (HM7301 in the error log).");
         json = "{\"snapshot\":{\"readable\":" + JsonBool(readable) + "}}";
         return OutcomeFailed;
      }

      if (!name.IsEmpty() && record.readable)
         WriteRecord(AppliedName(), AppliedRecord(name, record.applied != 0 ? record.applied : snapshot.applied, _T("kept"), now, record.set));

      AuditTrail::Instance()->RecordEvent(_T("profile"), name.IsEmpty() ? String(_T("(unknown)")) : name, _T("discarded"),
         Formatter::Format("the way back from the profile was discarded, {0} settings kept as it set them", (int) snapshot.entries.size()));

      LOG_APPLICATION(Formatter::Format("Profile {0}: the way back was discarded; the settings it set are kept.", name.IsEmpty() ? String(_T("(unknown)")) : name));

      json = "{\"profile\":" + JsonTextOrNull(name) + ",\"discarded\":" + JsonNumber(now) + ",\"entries\":" + JsonNumber((__int64) snapshot.entries.size()) +
             ",\"message\":" + JsonText(name.IsEmpty()
                ? String(_T("The way back, which could not be read, is discarded; the settings are as they are, and a profile may be applied."))
                : Formatter::Format("The way back from the {0} profile is discarded; its settings are kept, and another profile may be applied.", name)) + "}";
      return OutcomeDone;
   }

   Profiles::Outcome
   Profiles::Decline(AnsiString &json, String &sentence)
   {
      if (Reentered_(_T("declined"), sentence, json))
         return OutcomeRefused;

      ProfilesLock guard;

      sentence.Empty();
      String held;
      if (!ReadRecord(SnapshotName(), held))
      {
         sentence = _T("The profile's records could not be read from the database, so nothing was recorded.");
         json = "{}";
         return OutcomeFailed;
      }

      if (!held.IsEmpty())
      {
         sentence = _T("A way back from a profile is held: revert it or discard it before recording that no profile is wanted. Nothing was recorded.");
         json = "{}";
         return OutcomeRefused;
      }

      const Applied record = ReadAppliedRecord();
      if (!record.readable)
      {
         sentence = _T("The record of the last profile cannot be read, so it is not replaced; nothing was recorded.");
         json = "{}";
         return OutcomeFailed;
      }

      if (!record.profile.IsEmpty() && (record.state == _T("applied") || record.state == _T("kept")))
      {
         sentence = Formatter::Format("The {0} profile is in effect, so a profile was chosen; revert it first to go back to the settings this version ships with. Nothing was recorded.", record.profile);
         json = "{}";
         return OutcomeRefused;
      }

      const __int64 now = Now();
      if (!WriteRecord(AppliedName(), AppliedRecord(String(), now, _T("declined"), now, std::vector<String>())))
      {
         sentence = _T("The choice could not be recorded: the database did not take it (HM7301 in the error log).");
         json = "{}";
         return OutcomeFailed;
      }

      AuditTrail::Instance()->RecordEvent(_T("profile"), _T("(none)"), _T("declined"),
         _T("no profile is wanted: the settings this version ships with are kept"));

      LOG_APPLICATION(_T("Profiles: recorded that no profile is wanted; the settings this version ships with are kept."));

      json = "{\"declined\":" + JsonNumber(now) + ",\"message\":" +
             JsonText(_T("Recorded: this server keeps the settings this version ships with, and the checklist no longer asks for a profile. Any profile can still be compared and applied.")) + "}";
      return OutcomeDone;
   }

   void
   Profiles::Print_(bool error, const String &line)
   {
      AnsiString text = Utf8(line);
      text += "\n";

#ifdef HM_PLATFORM_POSIX
      std::fputs(text.c_str(), error ? stderr : stdout);
      std::fflush(error ? stderr : stdout);
#else
      HANDLE handle = GetStdHandle(error ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
      if (handle && handle != INVALID_HANDLE_VALUE)
      {
         DWORD written = 0;
         WriteFile(handle, text.c_str(), (DWORD) text.GetLength(), &written, nullptr);
      }
#endif
   }

   int
   Profiles::OpenForCommand_(const TCHAR *verb)
   {
      IniFileSettings::Instance()->LoadSettings();
      if (!IniFileSettings::Instance()->GetDatabaseSettingsExists())
      {
         Print_(true, Formatter::Format("[Database] in {0} does not name a database, so there is nothing to {1}. Nothing was changed.",
            IniFileSettings::GetInitializationFile(), String(verb)));
         return 2;
      }

      // The whole of a start short of the listeners, as --apply-environment makes it.
      String error;
      if (!Application::Instance()->InitInstance(error))
      {
         if (error.IsEmpty())
            error = Application::Instance()->GetLastErrorMessage();

         Print_(true, Formatter::Format("The configuration could not be read and the database opened: {0} Nothing was changed.", error));
         Application::Instance()->ExitInstance();
         return 1;
      }

      return 0;
   }

   int
   Profiles::RunCommand(const String &name, bool confirmed, bool once, bool ifNew)
   {
      const bool none = name.CompareNoCase(_T("none")) == 0;
      if (!none && !FindProfile(name))
      {
         Print_(true, Formatter::Format("There is no profile named {0}. The profiles are office, gateway, relay and sender, and none records that no profile is wanted; docs/Profiles.md says what each sets.", name));
         return 2;
      }

      const int opened = OpenForCommand_(_T("apply a profile to"));
      if (opened != 0)
         return opened;

      // The command line is an administrator of its own: the apply, and every
      // setting it writes, go on the audit trail under this name.
      AuditTrail::Actor actor;
      actor.present = true;
      actor.name = _T("hmailserver --apply-profile");
      actor.kind = _T("system");
      actor.interface_name = _T("command line");
      AuditScope scope(actor);

      int result = 0;

      do
      {
         if (once)
         {
            const Applied record = ReadAppliedRecord();
            // A failed apply is not an applied one: the next start tries again, and
            // is told of the way back it holds.
            if (record.Recorded() && record.state != _T("failed"))
            {
               Print_(false, Formatter::Format("A choice was recorded for this server before ({0}, {1}, now {2}), so --once leaves the settings as they are.",
                  record.profile.IsEmpty() ? String(_T("no profile")) : record.profile, UtcText(record.applied), record.state));
               result = 0;
               break;
            }
         }

         if (ifNew)
         {
            long long accounts = 0;
            bool counted = false;
            if (!SetupChecklist::IsYoungServer(accounts, counted))
            {
               Print_(true, counted
                  ? Formatter::Format("This server holds {0} accounts, so it is in service, and --if-new leaves it as it is: nothing was applied. Compare the profile with this server in the Control Deck or the Control Panel, which show every change first.", (int) accounts)
                  : String(_T("The accounts could not be counted, so whether this server is new is not known, and --if-new leaves it as it is: nothing was applied.")));
               result = 5;
               break;
            }
         }

         if (none)
         {
            AnsiString json;
            String sentence;
            const Outcome outcome = Decline(json, sentence);
            if (outcome == OutcomeDone)
            {
               Print_(false, _T("Recorded: this server keeps the settings this version ships with, and the checklist no longer asks for a profile."));
               result = 0;
            }
            else
            {
               Print_(true, sentence);
               result = 1;
            }
            break;
         }

         String sentence;
         Report report;
         std::vector<Evaluated> all;

         Outcome outcome;
         {
            ProfilesLock guard;
            outcome = ApplyLocked(name, confirmed, String(), report, all, sentence);
         }

         PrintWritten(report, &Profiles::Print_);

         switch (outcome)
         {
         case OutcomeDone:
            // Written by this process with the service stopped - the Windows entry
            // point refuses while it runs, and the container runs this before the
            // server starts - so the server takes every value when it starts.
            Print_(false, Formatter::Format("The {0} profile: {1} set, {2} unchanged, {3} skipped{4}. The server takes them when it starts; a server already running, which the command line cannot see on Linux, takes them at its next reinitialise (Apply now), and a setting marked restart at its next start.",
               report.profile, report.set, report.unchanged, report.skipped,
               report.waitRestart > 0 ? Formatter::Format(", {0} of them marked restart", report.waitRestart) : String()));
            result = 0;
            break;
         case OutcomeRefused:
            Print_(true, sentence);
            if (!report.needConfirmation.empty() && report.refused.empty() && !report.stale)
            {
               Print_(true, _T("Run it again with --confirm to apply it."));
               result = 3;
            }
            else
               result = 1;
            break;
         case OutcomeUnknown:
            Print_(true, sentence);
            result = 2;
            break;
         default:
            Print_(true, sentence);
            result = 1;
            break;
         }
      } while (false);

      Application::Instance()->ExitInstance();
      return result;
   }

   int
   Profiles::RunRevertCommand()
   {
      const int opened = OpenForCommand_(_T("revert"));
      if (opened != 0)
         return opened;

      int result = 0;
      {
         AuditTrail::Actor actor;
         actor.present = true;
         actor.name = _T("hmailserver --revert-profile");
         actor.kind = _T("system");
         actor.interface_name = _T("command line");
         AuditScope scope(actor);

         AnsiString json;
         String sentence;
         const Outcome outcome = Revert(json, sentence);

         JsonValue document;
         std::string error;
         if (JsonValue::Parse(std::string(json.c_str()), document, error) && document.IsObject())
         {
            const JsonValue *entries = document.Get("entries");
            if (entries && entries->IsArray())
            {
               for (const JsonValue &entry : entries->Items())
               {
                  const String line = Formatter::Format("{0} ({1}): {2}{3}", Wide(entry.GetString("name")), Wide(entry.GetString("store")),
                     Wide(entry.GetString("result")), entry.GetString("reason").empty() ? String() : String(_T(" - ")) + Wide(entry.GetString("reason")));
                  Print_(entry.GetString("result") == "failed", line);
               }
            }
            if (outcome == OutcomeDone)
               Print_(false, Wide(document.GetString("message")));
         }

         if (outcome != OutcomeDone)
         {
            Print_(true, sentence);
            result = 1;
         }
      }

      Application::Instance()->ExitInstance();
      return result;
   }

   void
   ProfilesTester::Test()
   {
      struct Verb
      {
         const char *name;
         std::function<Profiles::Outcome(AnsiString &, String &)> call;
      };

      const Verb verbs[] =
      {
         { "apply", [](AnsiString &json, String &sentence) { return Profiles::Apply(_T("office"), true, String(), json, sentence); } },
         { "revert", [](AnsiString &json, String &sentence) { return Profiles::Revert(json, sentence); } },
         { "discard", [](AnsiString &json, String &sentence) { return Profiles::Discard(json, sentence); } },
         { "decline", [](AnsiString &json, String &sentence) { return Profiles::Decline(json, sentence); } },
      };

      {
         // What an event script run by an apply's or a revert's write meets: this
         // thread holds the lock.
         ProfilesLock held;

         // Another thread meanwhile is not refused: it would wait for the lock, as it
         // always has, since the mark is this thread's alone.
         bool otherRefused = true;
         std::thread other([&otherRefused]()
         {
            String sentence;
            AnsiString json;
            otherRefused = Reentered_(_T("applied"), sentence, json);
         });
         other.join();
         if (otherRefused)
            throw std::logic_error("ProfilesTester: a thread that does not hold the profiles' lock was refused as re-entered");

         for (const Verb &verb : verbs)
         {
            AnsiString json;
            String sentence;
            const Profiles::Outcome outcome = verb.call(json, sentence);
            if (outcome != Profiles::OutcomeRefused || sentence.Find(_T("this thread is applying or reverting one")) < 0 ||
                json != "{\"reentered\":true}")
            {
               throw std::logic_error(std::string("ProfilesTester: ") + verb.name +
                  " on the thread holding the profiles' lock was not refused as re-entered");
            }
         }
      }

      // Let go: this thread is no longer refused, and the lock is free.
      String sentence;
      AnsiString json;
      if (holds_profiles_mutex || Reentered_(_T("applied"), sentence, json))
         throw std::logic_error("ProfilesTester: the thread is still marked as holding the profiles' lock after it let go");
      if (!profiles_mutex.try_lock())
         throw std::logic_error("ProfilesTester: the profiles' lock is still held after the thread let go");
      profiles_mutex.unlock();
   }
}
