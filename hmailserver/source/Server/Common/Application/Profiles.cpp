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
//                       "reverted"|"kept","changed":<unix>}
//                    the last profile applied, and what became of it. Empty on a
//                    server no profile was ever applied to - which is what
//                    --apply-profile --once asks.
//
// Both fit hm_settings.settingstring's 4000 characters: an apply whose way back
// would not is refused before anything is written.

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
#include "../Util/AuditTrail.h"
#include "../Util/JsonDocument.h"
#include "../Util/Unicode.h"
#include "../Util/RestApiSettingsRows.h"

#include <climits>
#include <cstdio>
#include <ctime>
#include <mutex>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // One apply, revert or discard at a time: each reads the records, decides, and
      // writes them, and two at once would each decide on what the other is about
      // to replace.
      std::mutex profiles_mutex;

      // The widest value hm_settings.settingstring holds.
      const int MaximumRecord = 4000;

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
         struct tm parts;
         memset(&parts, 0, sizeof(parts));
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

      const char *CautionWord(ProfilesData::Caution caution)
      {
         switch (caution)
         {
         case ProfilesData::CautionClosesListener: return "closes_listener";
         case ProfilesData::CautionTrustsAddresses: return "trusts_addresses";
         case ProfilesData::CautionTurnsFilterOff: return "turns_filter_off";
         default: return nullptr;
         }
      }

      const TCHAR *CautionSentence(ProfilesData::Caution caution)
      {
         switch (caution)
         {
         case ProfilesData::CautionClosesListener: return _T("closes a listener");
         case ProfilesData::CautionTrustsAddresses: return _T("trusts an address range");
         case ProfilesData::CautionTurnsFilterOff: return _T("turns a filter off");
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

      typedef std::map<String, std::pair<String, String> > StoredRows;

      // What the setting holds now, read as the server reads it. rows, when given, is
      // the settings store read once for a whole profile rather than once an entry.
      void ReadCurrent(Evaluated &evaluated, const StoredRows *rows = nullptr)
      {
         if (evaluated.ini)
         {
            StoredRows read;
            if (!rows)
            {
               IniSettingStore::ReadAllRows(read);
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
            return;
         }

         std::shared_ptr<PropertySet> properties = Configuration::Instance()->GetPropertySet();
         evaluated.stored = properties && properties->Exists(evaluated.name);

         const SettingsDefaults::Setting *property = FindProperty(evaluated.name);
         if (!evaluated.stored)
         {
            evaluated.current = property ? (property->Kind == SettingsDefaults::KindString ? String(property->Value) : StringParser::IntToString((int) property->Number)) : String();
            return;
         }

         if (property && property->Kind == SettingsDefaults::KindString)
            evaluated.current = properties->GetString(evaluated.name);
         else
            evaluated.current = StringParser::IntToString((int) properties->GetLong(evaluated.name));
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

            String applies;
            String spelling;
            evaluated.applies = IniFileSettings::GetApplies(evaluated.name, applies, spelling) ? applies : String(_T("now"));
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

         evaluated.confirm = entry.caution != ProfilesData::CautionNone && evaluated.changes &&
                             evaluated.environment.IsEmpty() && evaluated.refused.IsEmpty();

         return evaluated;
      }

      std::vector<Evaluated> EvaluateAll(const ProfilesData::Profile &profile)
      {
         StoredRows rows;
         IniSettingStore::ReadAllRows(rows);

         std::vector<Evaluated> all;
         for (size_t i = 0; i < profile.entryCount; i++)
            all.push_back(Evaluate(profile.entries[i], rows));
         return all;
      }

      // ---- the records ---------------------------------------------------------

      String ReadRecord(const TCHAR *row)
      {
         std::shared_ptr<PropertySet> properties = Configuration::Instance()->GetPropertySet();
         if (!properties || !properties->Exists(row))
            return String();
         return properties->GetString(row);
      }

      // The server's own bookkeeping, written as it is: no refusal, no audit row of its
      // own (the apply or revert records itself as one event) and no change event.
      bool WriteRecord(const TCHAR *row, const String &value)
      {
         std::shared_ptr<PropertySet> properties = Configuration::Instance()->GetPropertySet();
         if (!properties)
            return false;

         if (properties->Exists(row))
            properties->SetBookkeeping(row, value);
         else
            properties->EnsureString(row, value);

         return true;
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

      String AppliedRecord(const String &profile, __int64 applied, const String &state, __int64 changed)
      {
         const AnsiString json = "{\"profile\":" + JsonText(profile) + ",\"applied\":" + JsonNumber(applied) +
                                 ",\"state\":" + JsonText(state) + ",\"changed\":" + JsonNumber(changed) + "}";
         return Wide(std::string(json.c_str()));
      }

      // ---- the answers ---------------------------------------------------------

      AnsiString AppliedJson()
      {
         String profile;
         __int64 applied = 0;
         String state;
         __int64 changed = 0;
         Profiles::ReadApplied(profile, applied, state, changed);
         if (profile.IsEmpty())
            return "null";

         const ProfilesData::Profile *definition = FindProfile(profile);
         return "{\"profile\":" + JsonText(profile) + ",\"title\":" + JsonText(definition ? String(definition->title) : profile) +
                ",\"applied\":" + JsonNumber(applied) + ",\"state\":" + JsonText(state) + ",\"changed\":" + JsonNumber(changed) + "}";
      }

      AnsiString SnapshotSummaryJson()
      {
         const String text = ReadRecord(Profiles::SnapshotName());
         if (text.IsEmpty())
            return "null";

         Snapshot snapshot;
         if (!ParseSnapshot(text, snapshot))
            return "{\"readable\":false}";

         return "{\"readable\":true,\"profile\":" + JsonText(snapshot.profile) + ",\"applied\":" + JsonNumber(snapshot.applied) +
                ",\"entries\":" + JsonNumber((__int64) snapshot.entries.size()) + "}";
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
         json += ",\"route\":" + JsonTextOrNull(evaluated.route);
         json += ",\"applies\":" + JsonTextOrNull(evaluated.applies);
         json += ",\"levels\":" + JsonTextOrNull(evaluated.levels);
         json += ",\"environment\":" + JsonTextOrNull(evaluated.environment);
         json += ",\"refused\":" + JsonTextOrNull(evaluated.refused);
         json += "}";
         return json;
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
         Report() : time(0), set(0), unchanged(0), skipped(0), failed(0), waitReinitialize(0), waitRestart(0) {}

         String profile;
         __int64 time;
         std::vector<Written> written;
         int set;
         int unchanged;
         int skipped;
         int failed;
         int waitReinitialize;
         int waitRestart;
         std::vector<const Evaluated *> needConfirmation;
         std::vector<const Evaluated *> refused;
      };

      Profiles::Outcome ApplyLocked(const String &name, bool confirmed, Report &report, std::vector<Evaluated> &all, String &sentence)
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
         const String held = ReadRecord(Profiles::SnapshotName());
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

         all = EvaluateAll(*profile);

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

         // A listener closed, an address range trusted or a filter turned off is
         // never done silently to a server already in service.
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
               sentence = _T("The way back could not be written, so nothing was applied.");
               return Profiles::OutcomeFailed;
            }
         }

         WriteRecord(Profiles::AppliedName(), AppliedRecord(profile->name, snapshot.applied, _T("applied"), snapshot.applied));

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

            if (evaluated.applies == _T("reinitialize"))
               report.waitReinitialize++;
            else if (evaluated.applies == _T("restart"))
               report.waitRestart++;

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
         WriteRecord(Profiles::SnapshotName(), snapshot.entries.empty() ? String() : Wide(std::string(SnapshotJson(snapshot).c_str())));

         AuditTrail::Instance()->RecordEvent(_T("profile"), profile->name, _T("applied"),
            Formatter::Format("{0} entries: {1} set, {2} unchanged, {3} skipped because the environment sets them, {4} failed",
               (int) all.size(), report.set, report.unchanged, report.skipped, report.failed));

         LOG_APPLICATION(Formatter::Format("Profile {0} applied: {1} of its {2} entries set, {3} unchanged, {4} skipped.",
            String(profile->name), report.set, (int) all.size(), report.unchanged, report.skipped));

         if (report.failed > 0)
         {
            sentence = Formatter::Format("{0} of the {1} profile could not be written; the rest were, and the way back holds what was changed.",
               Plural(report.failed, _T("entry"), _T("entries")), String(profile->name));
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

      String WaitingSentence(const Report &report)
      {
         if (report.waitReinitialize == 0 && report.waitRestart == 0)
            return _T("Every value set is in effect now.");

         std::vector<String> parts;
         if (report.waitReinitialize > 0)
            parts.push_back(Formatter::Format("{0} until the server is reinitialised (Apply now)", Plural(report.waitReinitialize, _T("waits"), _T("wait"))));
         if (report.waitRestart > 0)
            parts.push_back(Formatter::Format("{0} until the service is restarted", Plural(report.waitRestart, _T("waits"), _T("wait"))));
         return _T("Of the values set, ") + StringParser::JoinVector(parts, _T(", and ")) + _T(".");
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

   void
   Profiles::ReadApplied(String &profile, __int64 &applied, String &state, __int64 &changed)
   {
      profile.Empty();
      state.Empty();
      applied = 0;
      changed = 0;

      const String text = ReadRecord(AppliedName());
      if (text.IsEmpty())
         return;

      JsonValue document;
      std::string error;
      if (!JsonValue::Parse(std::string(Utf8(text).c_str()), document, error) || !document.IsObject())
      {
         // Something is recorded even if it cannot be read: --once must not take an
         // unreadable record for none, and apply the profile a second time.
         profile = _T("(unreadable)");
         state = _T("applied");
         return;
      }

      profile = Wide(document.GetString("profile"));
      state = Wide(document.GetString("state", "applied"));
      applied = document.GetInt64("applied");
      changed = document.GetInt64("changed", applied);
   }

   String
   Profiles::AppliedProfile()
   {
      String profile;
      __int64 applied = 0;
      String state;
      __int64 changed = 0;
      ReadApplied(profile, applied, state, changed);

      if (profile.IsEmpty() || state == _T("reverted") || !FindProfile(profile))
         return String();

      return profile;
   }

   bool
   Profiles::SourceOf(const String &store, const String &name, const String &value, String &profile)
   {
      profile = AppliedProfile();
      if (profile.IsEmpty())
         return false;

      const ProfilesData::Profile *definition = FindProfile(profile);
      if (!definition)
         return false;

      const bool ini = store == _T("ini");
      for (size_t i = 0; i < definition->entryCount; i++)
      {
         const ProfilesData::Entry &entry = definition->entries[i];
         if ((entry.store == ProfilesData::StoreIni) != ini || name.CompareNoCase(entry.name) != 0)
            continue;

         Evaluated shape;
         shape.integer = ini ? (FindIniKey(name) && FindIniKey(name)->IsInteger)
                             : (FindProperty(name) && FindProperty(name)->Kind == SettingsDefaults::KindNumber);
         return SameValue(shape, value, entry.value);
      }

      return false;
   }

   AnsiString
   Profiles::ListJson()
   {
      AnsiString json = "{\"profiles\":[";
      for (size_t i = 0; i < ProfilesData::ProfileCount; i++)
      {
         const ProfilesData::Profile &profile = ProfilesData::Profiles[i];
         const std::vector<Evaluated> all = EvaluateAll(profile);
         const Counts counts = Count(all);

         if (i > 0)
            json += ",";
         json += "{\"name\":" + JsonText(profile.name) + ",\"title\":" + JsonText(profile.title) + ",\"summary\":" + JsonText(profile.summary);
         json += ",\"includes\":" + JsonTextOrNull(profile.includes);
         json += ",\"entries\":" + JsonNumber((__int64) profile.entryCount);
         json += ",\"changes\":" + JsonNumber(counts.changes);
         json += ",\"confirmation_needed\":" + JsonNumber(counts.confirm);
         json += ",\"skipped\":" + JsonNumber(counts.skipped);
         json += ",\"refused\":" + JsonNumber(counts.refused) + "}";
      }
      json += "],\"applied\":" + AppliedJson() + ",\"snapshot\":" + SnapshotSummaryJson() + "}";
      return json;
   }

   bool
   Profiles::DiffJson(const String &name, AnsiString &json)
   {
      const ProfilesData::Profile *profile = FindProfile(name);
      if (!profile)
         return false;

      const std::vector<Evaluated> all = EvaluateAll(*profile);
      const Counts counts = Count(all);

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
         json += JsonText(evaluated.name);
      }
      json += "]";

      // Whether an apply would be taken now, and why not when it would not.
      const String held = ReadRecord(SnapshotName());
      String blocked;
      if (!held.IsEmpty())
         blocked = _T("A way back from an earlier profile is held: revert it or discard it before applying another.");
      else if (counts.refused > 0)
         blocked = _T("An entry would be refused by the setting it writes, so the profile cannot be applied as it is.");

      json += ",\"applicable\":" + JsonBool(blocked.IsEmpty());
      json += ",\"blocked\":" + JsonTextOrNull(blocked);
      json += ",\"applied\":" + AppliedJson() + ",\"snapshot\":" + SnapshotSummaryJson() + "}";
      return true;
   }

   Profiles::Outcome
   Profiles::Apply(const String &name, bool confirmed, AnsiString &json, String &sentence)
   {
      std::lock_guard<std::mutex> guard(profiles_mutex);

      Report report;
      std::vector<Evaluated> all;
      sentence.Empty();

      const Outcome outcome = ApplyLocked(name, confirmed, report, all, sentence);

      if (outcome == OutcomeDone || (outcome == OutcomeFailed && report.time != 0))
      {
         const String message = outcome == OutcomeDone
            ? Formatter::Format("The {0} profile: {1} set, {2} unchanged, {3} skipped. {4}", report.profile,
                 report.set, report.unchanged, report.skipped, WaitingSentence(report))
            : sentence;
         json = ReportJson(report, "applied", message);
         return outcome;
      }

      // Refused: what stops it, beside the sentence. confirmation is true when a
      // confirmation is all that stands in the way.
      const bool confirmationOnly = outcome == OutcomeRefused && !report.needConfirmation.empty() && report.refused.empty() && !confirmed;
      json = "{\"confirmation\":" + JsonBool(confirmationOnly) + ",\"confirmation_needed\":[";
      for (size_t i = 0; i < report.needConfirmation.size(); i++)
      {
         const Evaluated &evaluated = *report.needConfirmation[i];
         if (i > 0)
            json += ",";
         json += "{\"store\":\"" + AnsiString(StoreWord(evaluated.ini)) + "\",\"name\":" + JsonText(evaluated.name) +
                 ",\"caution\":\"" + AnsiString(CautionWord(evaluated.entry->caution)) + "\",\"current\":" + JsonText(evaluated.current) +
                 ",\"value\":" + JsonText(evaluated.value) + "}";
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
      json += "],\"snapshot\":" + SnapshotSummaryJson() + "}";

      return outcome;
   }

   Profiles::Outcome
   Profiles::Revert(AnsiString &json, String &sentence)
   {
      std::lock_guard<std::mutex> guard(profiles_mutex);

      sentence.Empty();
      const String held = ReadRecord(SnapshotName());
      if (held.IsEmpty())
      {
         sentence = _T("There is nothing to revert: no profile has been applied since the last revert or discard.");
         json = "{\"snapshot\":null}";
         return OutcomeRefused;
      }

      Snapshot snapshot;
      if (!ParseSnapshot(held, snapshot))
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7300, "Profiles::Revert",
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
         ReadCurrent(now);

         Written written;
         written.ini = entry.ini;
         written.name = entry.name;
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
         report.written.push_back(written);

         LOG_APPLICATION(Formatter::Format("Profile {0} reverted: {1} ({2}) put back to {3}.", snapshot.profile, entry.name,
            String(StoreWord(entry.ini)), entry.had ? (entry.before.IsEmpty() ? String(_T("empty")) : entry.before) : String(_T("its default"))));
      }

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
         WriteRecord(SnapshotName(), Wide(std::string(SnapshotJson(remaining).c_str())));
         sentence = Formatter::Format("{0} could not be put back; the rest were, and the way back now holds what is left.",
            Plural(report.failed, _T("setting"), _T("settings")));
      }
      else
      {
         String profile;
         __int64 applied = 0;
         String state;
         __int64 changed = 0;
         ReadApplied(profile, applied, state, changed);

         WriteRecord(SnapshotName(), String());
         WriteRecord(AppliedName(), AppliedRecord(snapshot.profile, applied != 0 ? applied : snapshot.applied, _T("reverted"), report.time));
      }

      AuditTrail::Instance()->RecordEvent(_T("profile"), snapshot.profile, _T("reverted"),
         Formatter::Format("{0} entries put back, {1} left as they were, {2} failed", report.set, report.skipped, report.failed));

      LOG_APPLICATION(Formatter::Format("Profile {0} reverted: {1} put back, {2} left as they were.", snapshot.profile, report.set, report.skipped));

      const String message = report.failed > 0 ? sentence
         : Formatter::Format("The {0} profile is reverted: {1} put back, {2} left as they were.", snapshot.profile,
              Plural(report.set, _T("setting"), _T("settings")), report.skipped);

      AnsiString body = ReportJson(report, "reverted", message);
      json = body;
      return report.failed > 0 ? OutcomeFailed : OutcomeDone;
   }

   Profiles::Outcome
   Profiles::Discard(AnsiString &json, String &sentence)
   {
      std::lock_guard<std::mutex> guard(profiles_mutex);

      sentence.Empty();
      const String held = ReadRecord(SnapshotName());
      if (held.IsEmpty())
      {
         sentence = _T("There is no way back to discard: no profile has been applied since the last revert or discard.");
         json = "{\"snapshot\":null}";
         return OutcomeRefused;
      }

      Snapshot snapshot;
      const bool readable = ParseSnapshot(held, snapshot);

      String profile;
      __int64 applied = 0;
      String state;
      __int64 changed = 0;
      ReadApplied(profile, applied, state, changed);

      const __int64 now = Now();
      WriteRecord(SnapshotName(), String());
      if (!profile.IsEmpty())
         WriteRecord(AppliedName(), AppliedRecord(readable ? snapshot.profile : profile, applied, _T("kept"), now));

      const String name = readable ? snapshot.profile : profile;
      AuditTrail::Instance()->RecordEvent(_T("profile"), name.IsEmpty() ? String(_T("(unknown)")) : name, _T("discarded"),
         Formatter::Format("the way back from the profile was discarded, {0} settings kept as it set them", (int) snapshot.entries.size()));

      LOG_APPLICATION(Formatter::Format("Profile {0}: the way back was discarded; the settings it set are kept.", name));

      json = "{\"profile\":" + JsonText(name) + ",\"discarded\":" + JsonNumber(now) + ",\"entries\":" + JsonNumber((__int64) snapshot.entries.size()) +
             ",\"message\":" + JsonText(Formatter::Format("The way back from the {0} profile is discarded; its settings are kept, and another profile may be applied.", name)) + "}";
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
   Profiles::RunCommand(const String &name, bool confirmed, bool once)
   {
      if (!FindProfile(name))
      {
         Print_(true, Formatter::Format("There is no profile named {0}. The profiles are office, gateway, relay and sender; docs/Profiles.md says what each sets.", name));
         return 2;
      }

      IniFileSettings::Instance()->LoadSettings();
      if (!IniFileSettings::Instance()->GetDatabaseSettingsExists())
      {
         Print_(true, Formatter::Format("[Database] in {0} does not name a database, so there is nothing to apply a profile to. Nothing was changed.",
            IniFileSettings::GetInitializationFile()));
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

      int result = 0;

      if (once)
      {
         String profile;
         __int64 applied = 0;
         String state;
         __int64 changed = 0;
         ReadApplied(profile, applied, state, changed);
         if (!profile.IsEmpty())
         {
            Print_(false, Formatter::Format("A profile was applied to this server before ({0}, {1}, now {2}), so --once leaves the settings as they are.",
               profile, UtcText(applied), state));
            Application::Instance()->ExitInstance();
            return 0;
         }
      }

      {
         // The command line is an administrator of its own: the apply, and every
         // setting it writes, go on the audit trail under this name.
         AuditTrail::Actor actor;
         actor.present = true;
         actor.name = _T("hmailserver --apply-profile");
         actor.kind = _T("system");
         actor.interface_name = _T("command line");
         AuditScope scope(actor);

         AnsiString json;
         String sentence;
         Report report;
         std::vector<Evaluated> all;

         Outcome outcome;
         {
            std::lock_guard<std::mutex> guard(profiles_mutex);
            outcome = ApplyLocked(name, confirmed, report, all, sentence);
         }

         for (const Written &written : report.written)
         {
            const String label = written.name + _T(" (") + String(StoreWord(written.ini)) + _T(")");
            if (written.result == _T("set"))
               Print_(false, Formatter::Format("{0}: set to {1} (was {2})", label, written.after.IsEmpty() ? String(_T("empty")) : written.after,
                  written.before.IsEmpty() ? String(_T("empty")) : written.before));
            else if (written.result == _T("unchanged"))
               Print_(false, label + _T(": unchanged"));
            else
               Print_(written.result == _T("failed"), label + _T(": ") + written.result + _T(": ") + written.reason);
         }

         switch (outcome)
         {
         case OutcomeDone:
            // Written by this process, so a server already running takes them at its
            // next reinitialise - as a value --apply-environment writes - and one that
            // is stopped when it starts; the ones marked restart wait for the start.
            Print_(false, Formatter::Format("The {0} profile: {1} set, {2} unchanged, {3} skipped{4}. The server takes them when it starts; one already running takes them at its next reinitialise (Apply now), and a setting marked restart at its next start.",
               report.profile, report.set, report.unchanged, report.skipped,
               report.waitRestart > 0 ? Formatter::Format(", {0} of them marked restart", report.waitRestart) : String()));
            result = 0;
            break;
         case OutcomeRefused:
            Print_(true, sentence);
            if (!report.needConfirmation.empty() && report.refused.empty())
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
      }

      Application::Instance()->ExitInstance();
      return result;
   }
}
