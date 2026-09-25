// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Every setting from the environment: the naming rule, --apply-environment, and
// the records that make an administrator's change to a setting the environment
// manages a refusal rather than an edit lost at the next start, and keep a value
// a file supplied withheld. See the header, and docs/EnvironmentConfiguration.md
// for what an operator reads.

#include "StdAfx.h"
#include "EnvironmentSettings.h"

#include "Configuration.h"
#include "IniFileSettings.h"
#include "IniSettingStore.h"
#include "IniSettingsBaseline.h"
#include "PropertySet.h"
#include "SettingsDefaults.h"
#include "../SQL/DatabaseConnectionManager.h"
#include "../SQL/DALRecordset.h"
#include "../SQL/SQLCommand.h"
#include "../Util/AuditTrail.h"
#include "../Util/FileUtilities.h"
#include "../Util/Unicode.h"
#include "../Util/RestApiSettingsRows.h"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <mutex>
#include <set>
#include <sys/stat.h>
#include <boost/asio/ip/address.hpp>

#ifdef HM_PLATFORM_POSIX
#include <unistd.h>
extern char **environ;
#else
#include <io.h>
#endif

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      const TCHAR *SettingPrefix = _T("HM_SETTING_");
      const TCHAR *PropertyPrefix = _T("HM_PROPERTY_");
      const TCHAR *FileSuffix = _T("_FILE");

      // The four variables the container took before every setting had a name
      // in the environment, and the keys they have always stored. The last two
      // are PATHS the server reads a certificate and a key from - not _FILE forms
      // of anything - and keep meaning exactly that.
      struct Alias
      {
         const TCHAR *variable;
         const TCHAR *key;
      };

      const Alias Aliases[] =
      {
         { _T("HM_REST_PORT"), _T("RestApiPort") },
         { _T("HM_REST_BIND_ADDRESS"), _T("RestApiBindAddress") },
         { _T("HM_REST_CERTIFICATE_FILE"), _T("RestApiCertificateFile") },
         { _T("HM_REST_PRIVATE_KEY_FILE"), _T("RestApiPrivateKeyFile") },
      };

      const size_t AliasCount = sizeof(Aliases) / sizeof(Aliases[0]);

      // A value file larger than this is not a setting: nothing either store holds
      // is longer than 4000 characters.
      const size_t MaximumFileBytes = 64 * 1024;

      // The widest value hm_settings.settingstring holds.
      const int MaximumPropertyText = 4000;

      bool StartsWith(const String &text, const TCHAR *prefix)
      {
         const String start(prefix);
         return text.GetLength() >= start.GetLength() && text.Left(start.GetLength()) == start;
      }

      bool EndsWith(const String &text, const TCHAR *suffix)
      {
         const String end(suffix);
         return text.GetLength() > end.GetLength() && text.Right(end.GetLength()) == end;
      }

      AnsiString Utf8(const String &text)
      {
         AnsiString result;
         Unicode::WideToMultiByte(text, result);
         return result;
      }

      // Strict: an optional minus sign and digits, within a 32-bit int - the COM
      // properties' range, and what GetPrivateProfileInt and settinginteger hold.
      bool ParseWholeNumber(const String &text, long &number)
      {
         if (text.IsEmpty() || text.GetLength() > 11)
            return false;

         int start = text.GetAt(0) == '-' ? 1 : 0;
         if (start == text.GetLength())
            return false;

         long long value = 0;
         for (int i = start; i < text.GetLength(); i++)
         {
            const wchar_t c = text.GetAt(i);
            if (c < '0' || c > '9')
               return false;
            value = value * 10 + (c - '0');
         }

         if (start == 1)
            value = -value;

         if (value < INT_MIN || value > INT_MAX)
            return false;

         number = (long) value;
         return true;
      }

      // A switch written as a word, which is how an environment says one. Taken
      // only where the setting is a switch - its REST row is one - so that
      // maxmessagesize=off is refused as the whole number it is not, rather than
      // stored as 0, which is "no limit".
      bool ParseSwitchWord(const String &text, long &number)
      {
         String word = text;
         word.MakeLower();

         if (word == _T("true") || word == _T("yes") || word == _T("on"))
         {
            number = 1;
            return true;
         }

         if (word == _T("false") || word == _T("no") || word == _T("off"))
         {
            number = 0;
            return true;
         }

         return false;
      }

      bool IsValidUtf8(const std::string &bytes)
      {
         size_t i = 0;
         while (i < bytes.size())
         {
            const unsigned char c = (unsigned char) bytes[i];
            size_t extra = 0;

            if (c < 0x80)
               extra = 0;
            else if (c >= 0xC2 && c <= 0xDF)
               extra = 1;
            else if (c >= 0xE0 && c <= 0xEF)
               extra = 2;
            else if (c >= 0xF0 && c <= 0xF4)
               extra = 3;
            else
               return false;

            for (size_t k = 1; k <= extra; k++)
            {
               if (i + k >= bytes.size())
                  return false;
               const unsigned char next = (unsigned char) bytes[i + k];
               if ((next & 0xC0) != 0x80)
                  return false;
            }

            i += extra + 1;
         }

         return true;
      }

      // The property's row in SettingsDefaults.h, by name without case.
      const SettingsDefaults::Setting *FindProperty(const String &name)
      {
         for (size_t i = 0; i < SettingsDefaults::SettingsCount; i++)
         {
            if (name.CompareNoCase(SettingsDefaults::Settings[i].Name) == 0)
               return &SettingsDefaults::Settings[i];
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

      // A property the environment may name: one this build reads by one column,
      // or backupencryptionpassphrase - read both ways - whose write-only REST row
      // names it, and whose setter keeps the two columns together.
      bool Nameable(const SettingsDefaults::Setting &property)
      {
         return property.Kind != SettingsDefaults::KindEither || RestApiSettingsRows::DeclaresProperty(property.Name);
      }

      size_t Distance(const String &left, const String &right)
      {
         String a = left;
         String b = right;
         a.MakeLower();
         b.MakeLower();

         std::vector<size_t> previous(b.GetLength() + 1);
         std::vector<size_t> current(b.GetLength() + 1);

         for (size_t j = 0; j <= (size_t) b.GetLength(); j++)
            previous[j] = j;

         for (size_t i = 1; i <= (size_t) a.GetLength(); i++)
         {
            current[0] = i;
            for (size_t j = 1; j <= (size_t) b.GetLength(); j++)
            {
               const size_t cost = a.GetAt((int) i - 1) == b.GetAt((int) j - 1) ? 0 : 1;
               current[j] = (std::min)((std::min)(previous[j] + 1, current[j - 1] + 1), previous[j - 1] + cost);
            }
            previous.swap(current);
         }

         return previous[b.GetLength()];
      }

      // The setting nearest to a name that is none, from either store - a
      // misspelled key is as likely to be meant for the other one - said with the
      // variable that sets it.
      String NearestSetting(const String &name)
      {
         std::vector<String> keys;
         for (size_t i = 0; i < IniSettingsBaseline::SettingsCount; i++)
            keys.push_back(IniSettingsBaseline::Settings[i].Name);

         std::vector<String> properties;
         for (size_t i = 0; i < SettingsDefaults::SettingsCount; i++)
         {
            if (Nameable(SettingsDefaults::Settings[i]))
               properties.push_back(SettingsDefaults::Settings[i].Name);
         }

         const String key = EnvironmentSettings::Nearest(name, keys);
         const String property = EnvironmentSettings::Nearest(name, properties);

         if (!property.IsEmpty() && (key.IsEmpty() || Distance(name, property) < Distance(name, key)))
            return Formatter::Format("the hm_settings property {0}, which HM_PROPERTY_{0} sets", property);

         return Formatter::Format("the [Settings] key {0}, which HM_SETTING_{0} sets", key);
      }

      // The container's bootstrap variables, which the entrypoint reads and the
      // verb does not: never a misspelt setting variable. HM_PROFILE and
      // HM_PROFILE_CONFIRM name a setting profile the entrypoint applies once, with
      // hmailserver --apply-profile (docs/Profiles.md).
      bool IsBootstrapVariable(const String &name)
      {
         static const TCHAR *const names[] =
         {
            _T("HM_DB_TYPE"), _T("HM_DB_HOST"), _T("HM_DB_PORT"), _T("HM_DB_NAME"), _T("HM_DB_USER"),
            _T("HM_DB_PASSWORD"), _T("HM_DB_PASSWORD_FILE"), _T("HM_DB_TLS_MODE"), _T("HM_DB_TLS_CA"),
            _T("HM_ADMIN_PASSWORD"), _T("HM_ADMIN_PASSWORD_FILE"),
            _T("HM_INI"), _T("HM_CREATE_DATABASE"), _T("HM_UPGRADE_DATABASE"), _T("HM_PROFILE"), _T("HM_PROFILE_CONFIRM"),
         };

         for (const TCHAR *candidate : names)
         {
            if (name.CompareNoCase(candidate) == 0)
               return true;
         }

         return false;
      }

      String Serialise(const std::set<String> &variables)
      {
         std::vector<String> sorted(variables.begin(), variables.end());
         return StringParser::JoinVector(sorted, _T(","));
      }

      // The records' text as last reported unresolvable, so that HM7290 is said
      // once per value rather than at every read.
      std::mutex reported_mutex;
      String reported_marker;
   }

   const TCHAR *
   EnvironmentSettings::MarkerName()
   {
      return _T("EnvironmentManagedSettings");
   }

   const TCHAR *
   EnvironmentSettings::SecretMarkerName()
   {
      return _T("EnvironmentSecretSettings");
   }

   const char *
   EnvironmentSettings::NotStorableSentence()
   {
      return "The value is longer than 4000 characters or contains a line break, so it could not be stored or read back as one setting.";
   }

   String
   EnvironmentSettings::MarkerFile()
   {
      return FileUtilities::Combine(IniFileSettings::Instance()->GetDataDirectory(), _T(".hmailserver-environment"));
   }

   const char *
   EnvironmentSettings::SourceWord(Source source)
   {
      return source == SourceFile ? "file" : "variable";
   }

   String
   EnvironmentSettings::Nearest(const String &name, const std::vector<String> &candidates)
   {
      String best;
      size_t bestDistance = (size_t) -1;

      for (const String &candidate : candidates)
      {
         const size_t distance = Distance(name, candidate);
         if (distance < bestDistance)
         {
            bestDistance = distance;
            best = candidate;
         }
      }

      return best;
   }

   // ---------------------------------------------------------------------
   // Reading the environment.
   // ---------------------------------------------------------------------

   void
   EnvironmentSettings::ReadProcessEnvironment(std::vector<Variable> &variables)
   {
      variables.clear();

#ifdef HM_PLATFORM_POSIX
      for (char **entry = environ; entry && *entry; entry++)
      {
         const std::string text(*entry);
         const std::string::size_type equals = text.find('=');
         if (equals == std::string::npos || equals == 0)
            continue;

         Variable variable;
         Unicode::MultiByteToWide(AnsiString(text.substr(0, equals).c_str()), variable.name);
         Unicode::MultiByteToWide(AnsiString(text.substr(equals + 1).c_str()), variable.value);
         variables.push_back(variable);
      }
#else
      wchar_t *block = GetEnvironmentStringsW();
      if (!block)
         return;

      for (const wchar_t *entry = block; *entry; entry += wcslen(entry) + 1)
      {
         // The per-drive current directories are kept here as "=C:=C:\..." and are
         // nobody's variable.
         const wchar_t *equals = wcschr(entry + 1, L'=');
         if (!equals || entry[0] == L'=')
            continue;

         Variable variable;
         variable.name = String(entry, (size_t) (equals - entry));
         variable.value = String(equals + 1);
         variables.push_back(variable);
      }

      FreeEnvironmentStringsW(block);
#endif
   }

   // ---------------------------------------------------------------------
   // Resolving the names. No database is needed for any of this.
   // ---------------------------------------------------------------------

   bool
   EnvironmentSettings::ResolveVariableName_(const String &variable, Request &request, String &base)
   {
      for (size_t i = 0; i < AliasCount; i++)
      {
         if (variable == Aliases[i].variable)
         {
            request.store = StoreIni;
            request.source = SourceVariable;
            request.legacy = true;
            base = Aliases[i].key;
            return true;
         }
      }

      String rest;
      if (StartsWith(variable, SettingPrefix))
      {
         request.store = StoreIni;
         rest = variable.Mid(String(SettingPrefix).GetLength());
      }
      else if (StartsWith(variable, PropertyPrefix))
      {
         request.store = StoreDatabase;
         rest = variable.Mid(String(PropertyPrefix).GetLength());
      }
      else
         return false;

      // No [Settings] key and no property this build has ends in _FILE
      // (build/check-environment-names.py holds that), so the suffix is never a
      // name's own.
      if (EndsWith(rest, FileSuffix))
      {
         request.source = SourceFile;
         rest = rest.Left(rest.GetLength() - String(FileSuffix).GetLength());
      }
      else
         request.source = SourceVariable;

      base = rest;
      return true;
   }

   bool
   EnvironmentSettings::MisspeltPrefix_(const String &variable, String &problem)
   {
      // A variable of the container's own family - HM_ in any case - that is none of
      // the bootstrap variables, no alias and no setting prefix, and whose first part
      // is one of the two prefixes misspelt or in small letters: HM_SETTINGS_,
      // HM_SETING_, HM_PROPERTIES_, hm_setting_. Left alone it would set nothing and
      // say nothing, and the container would start on a setting it was not given.
      if (variable.GetLength() < 4 || variable.Left(3).CompareNoCase(_T("HM_")) != 0 || IsBootstrapVariable(variable))
         return false;

      const String rest = variable.Mid(3);
      const int underscore = rest.Find(_T('_'));
      if (underscore <= 0)
         return false;

      const String segment = rest.Left(underscore);
      const String name = rest.Mid(underscore + 1);
      String upper = segment;
      upper.MakeUpper();

      const TCHAR *meant = nullptr;
      if (upper == _T("SETTING") || upper == _T("SETTINGS") || Distance(upper, _T("SETTING")) <= 2)
         meant = SettingPrefix;
      else if (upper == _T("PROPERTY") || upper == _T("PROPERTIES") || Distance(upper, _T("PROPERTY")) <= 2)
         meant = PropertyPrefix;

      if (!meant)
         return false;

      problem = Formatter::Format("{0} is not a setting variable: HM_{1}_ is not a prefix the server reads. A [Settings] key is named HM_SETTING_<key> and an hm_settings row HM_PROPERTY_<name>, in capitals; did you mean {2}{3}?",
         variable, segment, String(meant), name);
      return true;
   }

   void
   EnvironmentSettings::ResolveSettingName_(const String &base, Request &request)
   {
      const String prefix = request.store == StoreIni ? SettingPrefix : PropertyPrefix;

      if (base.IsEmpty())
      {
         request.problem = Formatter::Format("{0} names no setting: the name after {1} is empty.", request.variable, prefix);
         return;
      }

      if (base.CompareNoCase(MarkerName()) == 0 || base.CompareNoCase(SecretMarkerName()) == 0)
      {
         request.problem = Formatter::Format("{0} is where the server records what its environment set: it is not a setting, and --apply-environment writes it itself.", base);
         return;
      }

      if (request.store == StoreIni)
      {
         const IniSettingsBaseline::Setting *key = FindIniKey(base);
         if (key)
         {
            request.name = key->Name;
            return;
         }

         const SettingsDefaults::Setting *property = FindProperty(base);
         if (property && Nameable(*property))
         {
            request.problem = Formatter::Format("{0} is not a [Settings] key: {1} is an hm_settings property, which HM_PROPERTY_{1} sets.", base, String(property->Name));
            return;
         }

         request.problem = Formatter::Format("{0} is not one of the [Settings] keys this build reads (build/ini-settings-baseline.txt). The nearest name is {1}.", base, NearestSetting(base));
         return;
      }

      const SettingsDefaults::Setting *property = FindProperty(base);
      if (property)
      {
         // Five rows a create script still seeds for a feature that has gone, which
         // nothing reads - antispamenabled, groupcachettl, relaymode, sendstatistics
         // and smtpnooftries. The one other row no read names by one column,
         // backupencryptionpassphrase, is nameable: its REST row's setter writes
         // both columns.
         if (!Nameable(*property))
         {
            request.problem = Formatter::Format("{0} is an hm_settings row that nothing in this build reads, so storing it would change nothing.", String(property->Name));
            return;
         }

         request.name = property->Name;
         return;
      }

      const IniSettingsBaseline::Setting *key = FindIniKey(base);
      if (key)
      {
         request.problem = Formatter::Format("{0} is not an hm_settings property: {1} is a [Settings] key, which HM_SETTING_{1} sets.", base, String(key->Name));
         return;
      }

      request.problem = Formatter::Format("{0} is not one of the hm_settings properties this build reads (SettingsDefaults.h). The nearest name is {1}.", base, NearestSetting(base));
   }

   bool
   EnvironmentSettings::ReadValueFile_(const String &path, String &value, String &problem)
   {
      if (path.IsEmpty())
      {
         problem = _T("names no file: it should hold the path of the file the value is in.");
         return false;
      }

      // A regular file, and nothing else - asked before the file is opened, because
      // a directory opens on Linux and reads as nothing, and the setting would be
      // stored empty. That is exactly what a Secret mounted at the wrong path, or a
      // bind mount whose host path is missing (Docker makes it an empty directory),
      // looks like. stat() follows a symbolic link, so a projected Secret's file,
      // which is one, is taken.
#ifdef HM_PLATFORM_POSIX
      const AnsiString narrow = Utf8(path);
      struct stat information;
      const bool found = ::stat(narrow.c_str(), &information) == 0;
      const bool regular = found && S_ISREG(information.st_mode);
#else
      struct _stat64 information;
      const bool found = _wstat64(path.c_str(), &information) == 0;
      const bool regular = found && (information.st_mode & _S_IFMT) == _S_IFREG;
#endif

      // The path is not a secret - it is where a Secret is mounted - and saying it
      // is what lets somebody find the mount that is missing.
      if (!found)
      {
         problem = Formatter::Format("names {0}, which cannot be read.", path);
         return false;
      }

      if (!regular)
      {
         problem = Formatter::Format("names {0}, which is not a file - a directory, perhaps the folder a Secret is mounted in rather than the file inside it, or a mount whose source is missing.", path);
         return false;
      }

#ifdef HM_PLATFORM_POSIX
      FILE *file = std::fopen(narrow.c_str(), "rb");
#else
      FILE *file = nullptr;
      if (_wfopen_s(&file, path.c_str(), L"rb") != 0)
         file = nullptr;
#endif

      if (!file)
      {
         problem = Formatter::Format("names {0}, which cannot be read.", path);
         return false;
      }

      std::string bytes;
      char buffer[4096];
      size_t read = 0;
      while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0)
      {
         bytes.append(buffer, read);
         if (bytes.size() > MaximumFileBytes)
            break;
      }

      // A read that stopped on an error is a value cut short, and is refused rather
      // than stored as far as it got.
      const bool failed = std::ferror(file) != 0;
      std::fclose(file);

      if (failed)
      {
         std::fill(bytes.begin(), bytes.end(), '\0');
         problem = Formatter::Format("names {0}, which could not be read to its end.", path);
         return false;
      }

      if (bytes.size() > MaximumFileBytes)
      {
         std::fill(bytes.begin(), bytes.end(), '\0');
         problem = Formatter::Format("names {0}, which is larger than 64 KB; no setting holds that much.", path);
         return false;
      }

      // A byte order mark is the editor's, not the value's.
      if (bytes.size() >= 3 && (unsigned char) bytes[0] == 0xEF && (unsigned char) bytes[1] == 0xBB && (unsigned char) bytes[2] == 0xBF)
         bytes.erase(0, 3);

      // The trailing line break a file almost always ends with - echo writes one, a
      // Secret made from a file often has one - is not part of the value. Only at
      // the end: a line break inside is the value's, and a [Settings] key refuses
      // one when it is checked.
      while (!bytes.empty() && (bytes[bytes.size() - 1] == '\n' || bytes[bytes.size() - 1] == '\r'))
         bytes.erase(bytes.size() - 1);

      // A NUL would end the text where it stands, and the setting would be stored
      // cut short as though that were the value.
      if (bytes.find('\0') != std::string::npos || !IsValidUtf8(bytes))
      {
         std::fill(bytes.begin(), bytes.end(), '\0');
         problem = Formatter::Format("names {0}, which is not UTF-8 text.", path);
         return false;
      }

      Unicode::MultiByteToWide(AnsiString(bytes.c_str()), value);

      // The narrow copy held the secret; it goes before the function does.
      std::fill(bytes.begin(), bytes.end(), '\0');
      return true;
   }

   void
   EnvironmentSettings::Resolve(const std::vector<Variable> &variables, std::vector<Request> &requests)
   {
      requests.clear();

      for (const Variable &variable : variables)
      {
         Request request;
         request.variable = variable.name;

         String base;
         if (!ResolveVariableName_(variable.name, request, base))
         {
            String problem;
            if (MisspeltPrefix_(variable.name, problem))
            {
               request.problem = problem;
               requests.push_back(request);
            }
            continue;
         }

         // An empty alias is an unset one, as the entrypoint always read it
         // ([ -z "${HM_REST_PORT:-}" ] || ...). An empty HM_SETTING_ or HM_PROPERTY_
         // is a value like any other - the empty string - and a key that reads a
         // number refuses it when it is checked.
         if (request.legacy && variable.value.IsEmpty())
            continue;

         ResolveSettingName_(base, request);

         // A setting a domain or an account may hold its own value of is answered to
         // their administrators as what is in effect for them
         // (docs/SettingLevels.md), so a file cannot keep its value from being shown:
         // the _FILE form, whose whole promise is that, is refused for it.
         if (request.problem.IsEmpty() && request.source == SourceFile)
         {
            const IniSettingsBaseline::Setting *key = request.store == StoreIni ? FindIniKey(request.name) : nullptr;
            const SettingsDefaults::Setting *property = request.store == StoreDatabase ? FindProperty(request.name) : nullptr;
            const bool levels = (key && key->Levels != IniSettingsBaseline::LevelServer) ||
                                (property && property->Levels != SettingsDefaults::LevelServer);
            if (levels)
               request.problem = Formatter::Format("{0} may also be set for a domain and an account, and what is in effect is shown to their administrators, so a file cannot keep its value secret; set it with {1}{0}.",
                  request.name, String(request.store == StoreIni ? SettingPrefix : PropertyPrefix));
         }

         if (!request.name.IsEmpty())
         {
            if (request.legacy)
               request.canonical = request.variable;
            else
               request.canonical = String(request.store == StoreIni ? SettingPrefix : PropertyPrefix) + request.name +
                                   (request.source == SourceFile ? String(FileSuffix) : String());
         }

         // The file is read only for a variable nothing has refused yet - which is
         // also why no refusal above can quote what the file holds.
         if (request.problem.IsEmpty())
         {
            if (request.source == SourceFile)
            {
               String path = variable.value;
               path.Trim();

               String problem;
               if (!ReadValueFile_(path, request.value, problem))
                  request.problem = request.variable + _T(" ") + problem;
            }
            else
               request.value = variable.value;
         }

         requests.push_back(request);
      }

      // Two variables for one setting are a mistake, not a precedence question:
      // the plain form and the _FILE form, an alias and the name it stands for, or
      // two spellings of one key (a POSIX environment tells HM_SETTING_restapiport
      // from HM_SETTING_RestApiPort).
      for (size_t i = 0; i < requests.size(); i++)
      {
         if (requests[i].name.IsEmpty())
            continue;

         std::vector<size_t> same;
         for (size_t j = 0; j < requests.size(); j++)
         {
            if (!requests[j].name.IsEmpty() && requests[j].store == requests[i].store && requests[j].name.CompareNoCase(requests[i].name) == 0)
               same.push_back(j);
         }

         if (same.size() < 2)
            continue;

         std::vector<String> names;
         for (size_t k : same)
            names.push_back(requests[k].variable);
         std::sort(names.begin(), names.end());

         const String sentence = Formatter::Format("{0} all name {1}; set one of them.",
            StringParser::JoinVector(names, _T(", ")), requests[i].name);

         for (size_t k : same)
            requests[k].problem = sentence;
      }

      std::sort(requests.begin(), requests.end(),
         [](const Request &left, const Request &right) { return left.variable < right.variable; });
   }

   // ---------------------------------------------------------------------
   // Checking the values. The configuration is loaded by now.
   // ---------------------------------------------------------------------

   String
   EnvironmentSettings::CheckIniValue(const String &name, const String &value)
   {
      if (!IniSettingStore::IsStorableValue(value))
         return String(NotStorableSentence());

      String trimmed = value;
      trimmed.Trim();

      const IniSettingsBaseline::Setting *key = FindIniKey(name);

      // A key the loader reads as a whole number (GetPrivateProfileInt, which would
      // read a word as 0 and "30s" as 30) takes digits.
      long number = 0;
      if (key && key->IsInteger && !ParseWholeNumber(trimmed, number))
         return Formatter::Format("{0} is read as a whole number, and the value given is not one.", String(key->Name));

      const bool port =
         name.CompareNoCase(_T("RestApiPort")) == 0 ||
         name.CompareNoCase(_T("MetricsServerPort")) == 0 ||
         name.CompareNoCase(_T("ManageSieveServerPort")) == 0 ||
         name.CompareNoCase(_T("WebServicesHttpPort")) == 0 ||
         name.CompareNoCase(_T("WebServicesHttpsPort")) == 0 ||
         name.CompareNoCase(_T("AcmeHttpPort")) == 0;

      if (port)
      {
         if (!ParseWholeNumber(trimmed, number) || number < 0 || number > 65535 || trimmed.GetAt(0) == '-')
            return Formatter::Format("{0} is a TCP port: a whole number from 0, which leaves its listener off, to 65535, and the value given is not one.", name);
      }

      if (name.CompareNoCase(_T("RestApiBindAddress")) == 0)
      {
         // localhost is the one name HttpServer::Listen takes, as 127.0.0.1.
         const AnsiString text = Utf8(trimmed);
         boost::system::error_code error;
         if (text != "localhost")
            (void) boost::asio::ip::make_address(text.c_str(), error);

         if (text.IsEmpty() || error)
            return _T("RestApiBindAddress is the address the REST listener binds: an IP address such as 127.0.0.1, ::1 or 0.0.0.0, or localhost. The value given is not one, and the listener would not start.");
      }

      return String();
   }

   bool
   EnvironmentSettings::IsSecret_(const Request &request, const std::vector<Entry> &secrets)
   {
      if (request.source == SourceFile || IniSettingStore::IsSecretName(request.name))
         return true;

      for (const Entry &entry : secrets)
      {
         if (entry.store == request.store && entry.name.CompareNoCase(request.name) == 0)
            return true;
      }

      return false;
   }

   String
   EnvironmentSettings::CheckValue_(Request &request, const std::vector<Entry> &secrets)
   {
      request.number = 0;
      request.isNumber = false;
      request.lent = false;

      const bool ini = request.store == StoreIni;

      RestApiSettingsRows::Lent lent;
      const bool lends = RestApiSettingsRows::Find(ini, request.name, lent);

      // A property REST answers read-only is the server's own - derived or kept -
      // and is not set from the environment; the VAPID public key above all, which
      // has to be the private key's own or every push is rejected.
      if (!ini && !lends && lent.readOnly)
      {
         if (request.name.CompareNoCase(_T("WebPushVapidPublicKey")) == 0)
            return _T("WebPushVapidPublicKey is derived from the private key, which HM_PROPERTY_WebPushVapidPrivateKey sets: the server makes the public half itself, so that the two can never disagree.");

         return Formatter::Format("{0} is answered read-only over REST: the server derives it or keeps it itself, so it is not set from the environment.", request.name);
      }

      String trimmed = request.value;
      trimmed.Trim();

      if (ini)
      {
         // A key a settings row keeps as a switch takes the words a switch is said
         // with, stored as the 1 or 0 the loader reads.
         long word = 0;
         if (lends && lent.kind == RestApiSettingsRows::KindSwitch && ParseSwitchWord(trimmed, word))
            request.value = word ? _T("1") : _T("0");

         String problem = CheckIniValue(request.name, request.value);
         if (!problem.IsEmpty())
            return problem;

         if (lends)
         {
            String value = request.value;
            value.Trim();
            long number = 0;
            if (lent.kind != RestApiSettingsRows::KindText && !ParseWholeNumber(value, number))
               return Formatter::Format("{0} is read as a whole number, and the value given is not one.", request.name);

            request.number = number;
            request.isNumber = lent.kind != RestApiSettingsRows::KindText;
            request.value = value;
            request.lent = true;
         }
      }
      else
      {
         const SettingsDefaults::Setting *property = FindProperty(request.name);
         if (!property)
            return Formatter::Format("{0} is not an hm_settings property this build reads.", request.name);

         // How the value is read: the REST row's way where one lends itself, the
         // property's column otherwise.
         const bool text = lends ? lent.kind == RestApiSettingsRows::KindText : property->Kind == SettingsDefaults::KindString;

         if (text)
         {
            if (request.value.GetLength() > MaximumPropertyText)
               return Formatter::Format("{0} holds at most 4000 characters, and the value given is longer.", request.name);
         }
         else
         {
            long number = 0;
            bool parsed = ParseWholeNumber(trimmed, number);

            // Words only where the row takes words: a switch's true and false, and a
            // row of words' own words.
            if (!parsed && lends && lent.kind == RestApiSettingsRows::KindSwitch)
               parsed = ParseSwitchWord(trimmed, number);

            String words;
            if (!parsed && lends && lent.kind == RestApiSettingsRows::KindWords)
               parsed = RestApiSettingsRows::WordValue(false, request.name, trimmed, number, words);

            if (!parsed)
            {
               if (lends && lent.kind == RestApiSettingsRows::KindWords)
                  return Formatter::Format("{0} is one of {1}, or the number one of them stands for, and the value given is neither.", request.name, lent.words);

               if (lends && lent.kind == RestApiSettingsRows::KindSwitch)
                  return Formatter::Format("{0} is a switch: 1 or 0, which true and false, yes and no, on and off also say, and the value given is none of them.", request.name);

               return Formatter::Format("{0} holds a whole number, and the value given is not one.", request.name);
            }

            request.number = number;
            request.isNumber = true;
         }

         request.lent = lends;
      }

      if (!lends)
         return String();

      // The check the setting's REST settings row makes, in that row's sentence: a
      // value the server would refuse over REST is refused here the same way. A
      // sentence a row writes may quote the value it refused, so for a value from a
      // file or of a secret it is not printed: a fixed one says which check it was.
      const String refused = RestApiSettingsRows::Check(ini, request.name, request.number, request.value);
      if (refused.IsEmpty())
         return String();

      if (IsSecret_(request, secrets))
         return Formatter::Format("{0} does not take the value {1} gives it: the check {2} makes of {3} refused it, and its sentence is not shown here because it may quote the value.",
            request.name, request.variable, String(lent.path.IsEmpty() ? AnsiString("PUT /api/v1/settings") : "PUT " + lent.path), String(lent.key));

      return refused;
   }

   // ---------------------------------------------------------------------
   // The records.
   // ---------------------------------------------------------------------

   void
   EnvironmentSettings::ParseMarker_(const String &marker, std::vector<Entry> &entries, std::vector<String> &unresolved)
   {
      entries.clear();
      unresolved.clear();

      std::vector<String> variables = StringParser::SplitString(marker, _T(","));
      for (String variable : variables)
      {
         variable.Trim();
         if (variable.IsEmpty())
            continue;

         Request request;
         request.variable = variable;

         String base;
         if (!ResolveVariableName_(variable, request, base))
         {
            unresolved.push_back(variable);
            continue;
         }

         ResolveSettingName_(base, request);
         if (request.name.IsEmpty())
         {
            unresolved.push_back(variable);
            continue;
         }

         Entry entry;
         entry.variable = variable;
         entry.store = request.store;
         entry.name = request.name;
         entry.source = request.source;
         entries.push_back(entry);
      }
   }

   void
   EnvironmentSettings::ReadRecord_(const TCHAR *row, std::vector<Entry> &entries, bool report)
   {
      entries.clear();

      std::shared_ptr<PropertySet> properties = Configuration::Instance()->GetPropertySet();
      if (!properties || !properties->Exists(row))
         return;

      const String marker = properties->GetString(row);
      if (marker.IsEmpty())
         return;

      std::vector<String> unresolved;
      ParseMarker_(marker, entries, unresolved);

      if (unresolved.empty() || !report)
         return;

      std::lock_guard<std::mutex> guard(reported_mutex);
      if (reported_marker == marker)
         return;
      reported_marker = marker;

      ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7290, "EnvironmentSettings::ReadManaged",
         Formatter::Format("The record of the settings this server's environment manages ({0}) names {1}, which this build does not read; it is ignored, and the setting can be changed here. The next hmailserver --apply-environment rewrites the record.",
            String(row), StringParser::JoinVector(unresolved, _T(", "))));
   }

   void
   EnvironmentSettings::ReadManaged(std::vector<Entry> &entries)
   {
      ReadRecord_(MarkerName(), entries, true);
   }

   void
   EnvironmentSettings::ReadSecretOrigins(std::vector<Entry> &entries)
   {
      ReadRecord_(SecretMarkerName(), entries, false);

      for (Entry &entry : entries)
         entry.source = SourceFile;
   }

   bool
   EnvironmentSettings::IsSecretOrigin(Store store, const String &name)
   {
      std::vector<Entry> entries;
      ReadSecretOrigins(entries);

      for (const Entry &entry : entries)
      {
         if (entry.store == store && entry.name.CompareNoCase(name) == 0)
            return true;
      }

      return false;
   }

   void
   EnvironmentSettings::ForgetSecretOrigin(Store store, const String &name)
   {
      std::shared_ptr<PropertySet> properties = Configuration::Instance()->GetPropertySet();
      if (!properties || !properties->Exists(SecretMarkerName()))
         return;

      std::vector<Entry> entries;
      ReadSecretOrigins(entries);

      std::set<String> kept;
      bool found = false;
      for (const Entry &entry : entries)
      {
         if (entry.store == store && entry.name.CompareNoCase(name) == 0)
         {
            found = true;
            continue;
         }
         kept.insert(entry.variable);
      }

      if (found)
         properties->SetBookkeeping(SecretMarkerName(), Serialise(kept));
   }

   bool
   EnvironmentSettings::IsManaged(Store store, const String &name, Entry &entry)
   {
      std::vector<Entry> entries;
      ReadManaged(entries);

      for (const Entry &candidate : entries)
      {
         if (candidate.store == store && candidate.name.CompareNoCase(name) == 0)
         {
            entry = candidate;
            return true;
         }
      }

      return false;
   }

   bool
   EnvironmentSettings::IsWithheld(Store store, const String &name)
   {
      Entry entry;
      if (IsManaged(store, name, entry) && entry.source == SourceFile)
         return true;

      return IsSecretOrigin(store, name);
   }

   void
   EnvironmentSettings::AnnounceManaged()
   {
      std::vector<Entry> entries;
      ReadManaged(entries);

      if (entries.empty())
         return;

      std::vector<String> names;
      for (const Entry &entry : entries)
         names.push_back(entry.name + _T(" (") + entry.variable + _T(")"));

      LOG_APPLICATION(Formatter::Format("Settings set by this server's environment, which hmailserver --apply-environment writes at every start and an administrator's change to which is refused: {0}.",
         StringParser::JoinVector(names, _T(", "))));
   }

   bool
   EnvironmentSettings::IsManagedInDatabase(Store store, const String &name, Entry &entry)
   {
      SQLCommand command("select settingstring from hm_settings where settingname = @SETTINGNAME");
      command.AddParameter("@SETTINGNAME", String(MarkerName()));

      std::shared_ptr<DatabaseConnectionManager> database = Application::Instance()->GetDBManager();
      if (!database)
         return false;

      std::shared_ptr<DALRecordset> recordset = database->OpenRecordset(command);
      if (!recordset || recordset->IsEOF())
         return false;

      std::vector<Entry> entries;
      std::vector<String> unresolved;
      ParseMarker_(recordset->GetStringValue("settingstring"), entries, unresolved);

      for (const Entry &candidate : entries)
      {
         if (candidate.store == store && candidate.name.CompareNoCase(name) == 0)
         {
            entry = candidate;
            return true;
         }
      }

      return false;
   }

   String
   EnvironmentSettings::RefusalSentence(const String &what, const Entry &entry)
   {
      return Formatter::Format("{0} is set by this server's environment ({1}{2}): hmailserver --apply-environment writes it at every start, so a change made here would be undone at the next one, and it is refused. Change the variable and restart, or unset it and restart to manage the setting here.",
         what, entry.variable, String(entry.source == SourceFile ? _T(", from a file") : _T("")));
   }

   void
   EnvironmentSettings::RefuseAdministratorChange(Store store, const String &name)
   {
      // No actor, no administrator: --apply-environment itself, a restore, a
      // start's reconcile and the server's own bookkeeping write without one.
      if (!AuditTrail::HasActor())
         return;

      Entry entry;
      if (!IsManaged(store, name, entry))
         return;

      throw Refusal(RefusalSentence(name, entry));
   }

   // ---------------------------------------------------------------------
   // The verb.
   // ---------------------------------------------------------------------

   void
   EnvironmentSettings::Print_(bool error, const String &line)
   {
      AnsiString text = Utf8(line);
      text += "\n";

#ifdef HM_PLATFORM_POSIX
      std::fputs(text.c_str(), error ? stderr : stdout);
      std::fflush(error ? stderr : stdout);
#else
      // WriteFile to the handle the process was given, which is what a caller that
      // redirected the output - a script, a test - reads. hMailServer.exe is a
      // Windows-subsystem program and its C runtime streams may be attached to
      // nothing; the caller attaches a console when there is no handle at all.
      HANDLE handle = GetStdHandle(error ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
      if (handle && handle != INVALID_HANDLE_VALUE)
      {
         DWORD written = 0;
         WriteFile(handle, text.c_str(), (DWORD) text.GetLength(), &written, nullptr);
      }
#endif
   }

   int
   EnvironmentSettings::RunCommand()
   {
      std::vector<Variable> variables;
      ReadProcessEnvironment(variables);

      std::vector<Request> requests;
      Resolve(variables, requests);

      // The variables' values are no longer needed: what is stored is in the
      // requests, and the process's own environment is the process's.
      for (Variable &variable : variables)
         std::fill(variable.value.begin(), variable.value.end(), L'\0');
      variables.clear();

      IniFileSettings::Instance()->LoadSettings();
      if (!IniFileSettings::Instance()->GetDatabaseSettingsExists())
      {
         Print_(true, Formatter::Format("[Database] in {0} does not name a database, so there is nowhere to store a setting. Nothing was changed.",
            IniFileSettings::GetInitializationFile()));
         return 2;
      }

      // The whole of a start short of the listeners, as --sample-domain makes it:
      // the configuration file, the database at this build's schema, the settings
      // store reconciled and the configuration loaded - which is what a setting's
      // REST row needs to be found, and what the store's writes need.
      String error;
      if (!Application::Instance()->InitInstance(error))
      {
         if (error.IsEmpty())
            error = Application::Instance()->GetLastErrorMessage();

         Print_(true, Formatter::Format("The configuration could not be read and the database opened: {0} Nothing was changed.", error));
         Application::Instance()->ExitInstance();
         return 1;
      }

      const int result = Apply_(requests);

      Application::Instance()->ExitInstance();
      return result;
   }

   bool
   EnvironmentSettings::WriteRecord_(const TCHAR *row, const String &value, String &problem)
   {
      std::shared_ptr<PropertySet> properties = Configuration::Instance()->GetPropertySet();
      if (!properties)
      {
         problem = _T("the configuration is not loaded");
         return false;
      }

      const bool exists = properties->Exists(row);

      if (value.IsEmpty())
      {
         if (!exists)
            return true;

         // Nothing to record: no row at all, which is how a server that has never
         // been given its settings by an environment has always looked.
         SQLCommand command("delete from hm_settings where settingname = @SETTINGNAME");
         command.AddParameter("@SETTINGNAME", String(row));
         if (!Application::Instance()->GetDBManager()->Execute(command))
         {
            problem = Formatter::Format("{0} could not be removed", String(row));
            return false;
         }

         return true;
      }

      if (exists)
         properties->SetBookkeeping(row, value);
      else
         properties->EnsureString(row, value);

      return true;
   }

   int
   EnvironmentSettings::Apply_(std::vector<Request> &requests)
   {
      std::shared_ptr<PropertySet> properties = Configuration::Instance()->GetPropertySet();
      if (!properties)
      {
         Print_(true, _T("The configuration is not loaded, so nothing could be stored. Nothing was changed."));
         return 1;
      }

      // What the last run left: the settings the environment managed, and the ones
      // whose value came from a file and has not been rewritten since - which stay
      // withheld whatever this run does with them (IsSecret_).
      std::vector<Entry> previous;
      ReadManaged(previous);

      std::vector<Entry> secrets;
      ReadSecretOrigins(secrets);

      // Every value checked before anything is written. The configuration being
      // loaded is what lets a setting's REST row be found.
      for (Request &request : requests)
      {
         if (request.problem.IsEmpty())
            request.problem = CheckValue_(request, secrets);
      }

      // The file, where a [Settings] key's copy is written as well as the store: the
      // row alone would be reported at every start (HM5803) until the line could
      // be written. Refused before anything is, as --set-setting refuses.
      {
         bool anyIni = false;
         for (const Request &request : requests)
            anyIni = anyIni || (request.store == StoreIni && request.problem.IsEmpty() && !request.name.IsEmpty());

         if (anyIni)
         {
            const String file = IniFileSettings::GetInitializationFile();
#ifdef HM_PLATFORM_POSIX
            const bool writable = ::access(Utf8(file).c_str(), W_OK) == 0;
#else
            const bool writable = _waccess(file.c_str(), 2) == 0;
#endif
            if (!writable)
            {
               for (Request &request : requests)
               {
                  if (request.store == StoreIni && request.problem.IsEmpty())
                     request.problem = Formatter::Format("{0} is not writable by this user, and a [Settings] value is written there as well as into the database.", file);
               }
            }
         }
      }

      // The two records this run writes, worked out - and their length checked -
      // before anything is stored: a record the column cannot hold would leave the
      // settings stored and the old record, and so the old refusals, in force.
      std::set<String> managed;
      for (const Request &request : requests)
      {
         if (!request.canonical.IsEmpty())
            managed.insert(request.canonical);
      }

      std::set<String> secretNames;
      for (const Entry &entry : secrets)
         secretNames.insert(String(entry.store == StoreIni ? SettingPrefix : PropertyPrefix) + entry.name);
      for (const Request &request : requests)
      {
         if (request.name.IsEmpty())
            continue;

         const String plain = String(request.store == StoreIni ? SettingPrefix : PropertyPrefix) + request.name;
         for (auto held = secretNames.begin(); held != secretNames.end(); )
         {
            if (held->CompareNoCase(plain) == 0)
               held = secretNames.erase(held);
            else
               held++;
         }

         // A value this run takes from a file is the file's until an administrator
         // writes another; one it takes from a variable is the variable's, and is no
         // longer withheld.
         if (request.source == SourceFile)
            secretNames.insert(plain);
      }

      const String marker = Serialise(managed);
      const String secretMarker = Serialise(secretNames);

      if (marker.GetLength() > MaximumPropertyText || secretMarker.GetLength() > MaximumPropertyText)
      {
         for (Request &request : requests)
         {
            if (request.problem.IsEmpty())
               request.problem = Formatter::Format("the record of the {0} settings the environment names would be longer than the 4000 characters {1} holds; name fewer settings in the environment.",
                  (int) managed.size(), String(MarkerName()));
         }
      }

      size_t refused = 0;
      for (const Request &request : requests)
         refused += request.problem.IsEmpty() ? 0 : 1;

      if (refused > 0)
      {
         for (const Request &request : requests)
         {
            if (!request.problem.IsEmpty())
               Print_(true, request.variable + _T(": refused: ") + request.problem);
            else
               Print_(true, request.variable + _T(": not stored, because another variable was refused."));
         }

         Print_(true, Formatter::Format("{0} of {1} refused. Nothing was changed: the server is not started on settings it was not given.",
            (int) refused, (int) requests.size()));
         return 1;
      }

      int set = 0;
      int unchanged = 0;
      int failed = 0;
      std::vector<size_t> database;
      String stoppedAt;

      // Each line is printed when its write is done, so that what the log says was
      // stored is what was stored, whatever stops the run after it.
      try
      {
         std::map<String, std::pair<String, String> > rows;
         IniSettingStore::ReadAllRows(rows);

         for (size_t i = 0; i < requests.size(); i++)
         {
            Request &request = requests[i];
            stoppedAt = request.variable;

            const bool secret = IsSecret_(request, secrets);

            String before;
            bool had = false;
            bool same = false;

            if (request.store == StoreIni)
            {
               String value = request.value;
               value.Trim();

               const bool fileOnly = IniSettingStore::IsFileOnlyName(request.name);
               if (fileOnly)
               {
                  before = IniFileSettings::Instance()->GetSettingsValue(request.name);
                  had = !before.IsEmpty();
               }
               else
               {
                  // Opened: a secret the store protects is compared by what it
                  // holds, not by its envelope, which is new at every write - or
                  // every start would store it again and call it set.
                  auto row = rows.find(request.name);
                  had = row != rows.end();
                  before = had ? IniSettingStore::OpenStored(request.name, (*row).second.first) : String();
               }

               if (request.lent)
               {
                  // Through the row's setter, as PUT /api/v1/settings/... stores it.
                  String refusal;
                  if (!RestApiSettingsRows::Apply(true, request.name, request.number, value, refusal))
                  {
                     failed++;
                     Print_(true, request.variable + _T(": failed: ") + (secret ? String(_T("the setter refused the value")) : refusal) + _T("."));
                     continue;
                  }

                  std::map<String, std::pair<String, String> > after;
                  IniSettingStore::ReadAllRows(after);
                  auto now = after.find(request.name);
                  same = had && now != after.end() && IniSettingStore::OpenStored(request.name, (*now).second.first) == before;
               }
               else
               {
                  same = had && before == value;

                  if (!same && !IniFileSettings::Instance()->WriteSettingsValue(request.name, value))
                  {
                     failed++;
                     Print_(true, request.variable + _T(": failed: the database could not be written, so ") + request.name + _T(" was not stored."));
                     continue;
                  }
               }
            }
            else
            {
               had = properties->Exists(request.name);
               const SettingsDefaults::Setting *property = FindProperty(request.name);

               const String beforeText = had ? properties->GetString(request.name) : String();
               const long beforeNumber = had ? properties->GetLong(request.name) : 0;
               before = request.isNumber ? (had ? StringParser::IntToString((int) beforeNumber) : String()) : beforeText;

               if (request.lent)
               {
                  // A row the database does not have yet is made with its shipped
                  // value first - a write of a name the set does not hold reports
                  // HM5015 - and the value then goes through the row's setter, which
                  // derives, canonicalises and pairs as REST's does.
                  if (!had && property)
                  {
                     if (property->Kind == SettingsDefaults::KindString || property->Kind == SettingsDefaults::KindEither)
                        properties->EnsureString(request.name, property->Value);
                     else
                        properties->EnsureLong(request.name, property->Number);
                  }

                  String refusal;
                  if (!RestApiSettingsRows::Apply(false, request.name, request.number, request.value, refusal))
                  {
                     failed++;
                     Print_(true, request.variable + _T(": failed: ") + (secret ? String(_T("the setter refused the value")) : refusal) + _T("."));
                     continue;
                  }

                  same = had && properties->GetString(request.name) == beforeText && properties->GetLong(request.name) == beforeNumber;
               }
               else if (request.isNumber)
               {
                  same = had && beforeNumber == request.number;
                  if (!same)
                  {
                     if (had)
                        properties->SetLong(request.name, request.number);
                     else
                        properties->EnsureLong(request.name, request.number);
                  }
               }
               else
               {
                  same = had && beforeText == request.value;
                  if (!same)
                  {
                     if (had)
                        properties->SetString(request.name, request.value);
                     else
                        properties->EnsureString(request.name, request.value);
                  }
               }

               database.push_back(i);
            }

            if (same)
            {
               unchanged++;
               Print_(false, request.variable + _T(": unchanged"));
               continue;
            }

            set++;

            // The log says which value the environment overrode - never a secret's, a
            // file's, or one a file supplied before and nobody has rewritten.
            if (secret)
               Print_(false, request.variable + _T(": set (") + request.name + (request.source == SourceFile ? _T(", from a file") : _T("")) + _T("; the value is not shown)"));
            else if (had)
               Print_(false, request.variable + _T(": set (") + request.name + _T(" was ") + (before.IsEmpty() ? String(_T("empty")) : before) + _T(")"));
            else
               Print_(false, request.variable + _T(": set (") + request.name + _T(" had no stored value)"));
         }

         stoppedAt.Empty();

         // The property writes, read back: PropertySet's setters do not say whether the
         // row took, so a fresh read of the table does - the value the setter left,
         // both columns, and the two records below with them.
         String problem;
         if (!WriteRecord_(MarkerName(), marker, problem))
         {
            failed++;
            Print_(true, Formatter::Format("The record of what the environment manages was not written ({0}); the settings above are stored, and the other interfaces will not refuse a change to them.", problem));
         }

         if (!WriteRecord_(SecretMarkerName(), secretMarker, problem))
         {
            failed++;
            Print_(true, Formatter::Format("The record of the settings whose value came from a file was not written ({0}); such a value may be shown once the environment no longer sets it.", problem));
         }

         PropertySet stored;
         stored.Refresh();

         for (size_t i : database)
         {
            const Request &request = requests[i];
            const bool took = stored.Exists(request.name) &&
               stored.GetString(request.name) == properties->GetString(request.name) &&
               stored.GetLong(request.name) == properties->GetLong(request.name);

            if (!took)
            {
               failed++;
               Print_(true, request.variable + _T(": failed: ") + request.name + _T(" did not read back as it was written; see the error log."));
            }
         }

         const bool markerTook = marker.IsEmpty() ? !stored.Exists(MarkerName()) : stored.Exists(MarkerName()) && stored.GetString(MarkerName()) == marker;
         const bool secretTook = secretMarker.IsEmpty() ? !stored.Exists(SecretMarkerName()) : stored.Exists(SecretMarkerName()) && stored.GetString(SecretMarkerName()) == secretMarker;
         if (!markerTook || !secretTook)
         {
            failed++;
            Print_(true, Formatter::Format("The records {0} and {1} did not read back as they were written: the other interfaces may not refuse a change to a setting named here, or may show a value a file supplied.",
               String(MarkerName()), String(SecretMarkerName())));
         }

         // The file the entrypoint looks for, so that a start with no variable still
         // runs this and gives the settings back.
         const String file = MarkerFile();
         if (marker.IsEmpty())
         {
            if (FileUtilities::Exists(file) && !FileUtilities::DeleteFile(file))
            {
               failed++;
               Print_(true, Formatter::Format("{0} could not be removed; the entrypoint will go on running --apply-environment at every start, which does no harm.", file));
            }
         }
         else if (!FileUtilities::WriteToFile(file, AnsiString("Written by hmailserver --apply-environment: settings of this server are set by its environment. While this file is here the container's entrypoint runs --apply-environment at every start, so that a start without the variables gives the settings back.\n")))
         {
            failed++;
            Print_(true, Formatter::Format("{0} could not be written: a start without these variables would not give the settings back.", file));
         }
      }
      catch (...)
      {
         Print_(true, Formatter::Format("The run stopped{0}: an unexpected error, which the error log names. The lines above say what was stored; the record of what the environment manages was not updated.",
            stoppedAt.IsEmpty() ? String() : Formatter::Format(" at {0}", stoppedAt)));
         return 1;
      }

      // Names the last run managed and this one does not are the administrator's
      // again, and saying so is the only notice anybody gets of it. A value a file
      // supplied stays withheld until an administrator writes another.
      std::vector<String> released;
      for (const Entry &entry : previous)
      {
         bool still = false;
         for (const Request &request : requests)
            still = still || (request.store == entry.store && request.name.CompareNoCase(entry.name) == 0);
         if (!still)
            released.push_back(entry.name);
      }

      if (!released.empty())
         Print_(false, Formatter::Format("No longer set by the environment, and the administrator's to change again: {0}.",
            StringParser::JoinVector(released, _T(", "))));

      if (requests.empty())
      {
         Print_(false, _T("No variable names a setting (HM_SETTING_<key>, HM_PROPERTY_<name>, their _FILE forms, or HM_REST_PORT, HM_REST_BIND_ADDRESS, HM_REST_CERTIFICATE_FILE and HM_REST_PRIVATE_KEY_FILE)."));
         return failed > 0 ? 1 : 0;
      }

      Print_(false, Formatter::Format("{0} set, {1} unchanged{2}. The server takes them when it starts; one already running takes them at its next reinitialise, or restart for a key marked restart.",
         set, unchanged, failed > 0 ? Formatter::Format(", {0} failed", failed) : String()));

      return failed > 0 ? 1 : 0;
   }
}
