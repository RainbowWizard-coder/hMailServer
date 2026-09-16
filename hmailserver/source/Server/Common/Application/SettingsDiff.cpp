// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"
#include "SettingsDiff.h"

#include "Configuration.h"
#include "IniFileSettings.h"
#include "IniSettingStore.h"
#include "PropertySet.h"
#include "IniSettingsBaseline.h"
#include "SettingsDefaults.h"
#include "../Util/Unicode.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      const String StoreIni = _T("ini");
      const String StoreDatabase = _T("database");

      // What a secret's value is shown as. The same two answers GET
      // /api/v1/settings/ini gives, so a reader who has seen one list recognises
      // the other: it has one, or it has not.
      const String SecretSet = _T("(set)");

      String Masked(const String &value, bool secret)
      {
         if (!secret)
            return value;

         return value.IsEmpty() ? String() : SecretSet;
      }

      AnsiString Utf8(const String &value)
      {
         AnsiString utf8;
         Unicode::WideToMultiByte(value, utf8);
         return utf8;
      }

      // A JSON string body, from wide text, as UTF-8.
      AnsiString JsonText(const String &value)
      {
         const AnsiString utf8 = Utf8(value);
         AnsiString result;

         for (int i = 0; i < utf8.GetLength(); i++)
         {
            const char c = utf8.GetAt(i);

            switch (c)
            {
            case '"':  result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
               if ((unsigned char) c < 0x20)
               {
                  char escaped[8];
                  snprintf(escaped, sizeof(escaped), "\\u%04x", (unsigned char) c);
                  result += escaped;
               }
               else
                  result += c;
               break;
            }
         }

         return result;
      }

      AnsiString JsonField(const char *name, const String &value)
      {
         return AnsiString("\"") + name + "\":\"" + JsonText(value) + "\"";
      }

      AnsiString JsonBool(const char *name, bool value)
      {
         return AnsiString("\"") + name + "\":" + (value ? "true" : "false");
      }

      AnsiString JsonNumber(const char *name, int value)
      {
         AnsiString text;
         text.Format("\"%s\":%d", name, value);
         return text;
      }

      // The shipped values of hm_settings, by name, without case - the property
      // names of this server are spelled both ways (maxpop3connections and
      // BounceReportWindowDays are both real) and a row is matched with case, so
      // a lookup that insisted on case would call a perfectly ordinary row
      // unknown.
      bool DatabaseDefaultFor(const String &name, String &value, long &number, SettingsDefaults::SettingKind &kind)
      {
         for (size_t i = 0; i < SettingsDefaults::SettingsCount; i++)
         {
            if (name.CompareNoCase(SettingsDefaults::Settings[i].Name) != 0)
               continue;

            value = SettingsDefaults::Settings[i].Value;
            number = SettingsDefaults::Settings[i].Number;
            kind = SettingsDefaults::Settings[i].Kind;
            return true;
         }

         return false;
      }

      // Whether this build reads an hm_settings row at all, without case for the
      // reason above. A row it does not read is a fossil an upgrade script left
      // behind - useordb and eleven more were inserted for features that have gone,
      // and relaymode is still seeded by the create scripts with nothing to read it.
      // Every one of them differs from a database made today and none of them can
      // explain anything a server is doing, so none of them belongs in a list whose
      // whole worth is that it is short enough to read.
      bool ThisBuildReads(const String &name)
      {
         for (size_t i = 0; i < SettingsDefaults::PropertyNameCount; i++)
         {
            if (name.CompareNoCase(SettingsDefaults::PropertyNames[i]) == 0)
               return true;
         }

         return false;
      }

      // The number of settings the two stores between them have, which is what a
      // count of the ones that have been changed is read against: "3" means
      // nothing, "3 of 391" means something.
      //
      // The settings this build KNOWS, not the rows it happens to hold, and the
      // difference matters. A [Settings] key with no row is at its default and
      // holds no row precisely because nothing has changed it, so counting rows
      // would make the denominator smaller on the servers that have changed least
      // - and a fresh server would read "0 of 12". Every name the server holds
      // that this build ships no default for is added, since it is a setting this
      // server has that the count would otherwise leave out of both halves.
      int Considered(int unknownNames)
      {
         return (int) IniSettingsBaseline::SettingsCount + (int) SettingsDefaults::SettingsCount + unknownNames;
      }

      void ReadIniSettings(std::vector<SettingsDiff::Entry> &entries, int &unknownNames)
      {
         std::vector<String> stored;
         IniFileSettings::Instance()->GetSettingsNames(stored);

         // What the file's [SettingsOverride] section is forcing. GetSettingsNames
         // unites the table and the [Settings] section, and a key forced from
         // [SettingsOverride] need have neither a row nor a line: the section is the
         // door for a database that is unreachable or holds a value the server will
         // not start on, so a server CAN be running on a setting nothing has stored.
         // Leaving those out would make this list wrong in exactly the case it was
         // built for, so they are united in - by their own spelling, because the
         // overlay is read with case and an override spelled differently from the
         // row it means to shadow is a second entry to the server as well.
         std::set<String> overridden;
         IniFileSettings::Instance()->GetOverriddenNames(overridden);

         std::set<String> names(stored.begin(), stored.end());
         names.insert(overridden.begin(), overridden.end());

         for (auto iterName = names.begin(); iterName != names.end(); iterName++)
         {
            const String &name = *iterName;

            SettingsDiff::Entry entry;
            entry.store = StoreIni;
            entry.name = name;
            entry.secret = IniSettingStore::IsSecretName(name);
            entry.overridden = overridden.find(name) != overridden.end();

            const String value = IniFileSettings::Instance()->GetSettingsValue(name);

            String shipped;
            bool isInteger = false;
            entry.known = IniSettingStore::DefaultFor(name, shipped, isInteger);

            if (entry.known)
            {
               // Compared the way the server reads it. The integer read is
               // GetPrivateProfileInt, which stops at the first character that is
               // not part of a number, so a stored "0009" and a default of 9 are
               // the same setting and reporting them as a difference would be a
               // difference nobody could see.
               const bool differs = isInteger
                  ? _ttoi(value.c_str()) != _ttoi(shipped.c_str())
                  : value.Compare(shipped.c_str()) != 0;

               if (!differs)
                  continue;
            }
            else
               unknownNames++;

            entry.value = Masked(value, entry.secret);
            entry.shipped = Masked(shipped, entry.secret);
            entries.push_back(entry);
         }
      }

      void ReadDatabaseSettings(std::vector<SettingsDiff::Entry> &entries, int &unknownNames)
      {
         std::shared_ptr<PropertySet> properties = Configuration::Instance()->GetPropertySet();
         if (!properties)
            return;

         std::map<String, std::pair<long, String> > rows;
         properties->GetAll(rows);

         for (auto iterRow = rows.begin(); iterRow != rows.end(); iterRow++)
         {
            const String &name = (*iterRow).first;
            const long number = (*iterRow).second.first;
            const String &text = (*iterRow).second.second;

            // A row this build does not read at all is not reported. Unlike the ini
            // store, whose names an administrator may store anything under on
            // purpose, an hm_settings row can only have been made by a script of
            // this server's own - and the ones nothing reads were made by upgrade
            // scripts for features that have gone. Reporting them would put a dozen
            // entries nobody can act on at the top of every upgraded installation's
            // list.
            if (!ThisBuildReads(name))
               continue;

            SettingsDiff::Entry entry;
            entry.store = StoreDatabase;
            entry.name = name;
            entry.secret = IniSettingStore::IsSecretName(name);

            String shippedText;
            long shippedNumber = 0;
            SettingsDefaults::SettingKind kind = SettingsDefaults::KindEither;
            entry.known = DatabaseDefaultFor(name, shippedText, shippedNumber, kind);

            // hm_settings has both columns and the row does not say which of them
            // the property uses; SettingsDefaults.h does, read from the calls that
            // read the property. Only that column is compared, so that a string
            // property whose unused number column somehow moved is not reported as
            // a change to a value nothing reads. KindEither is a property Constants.h
            // names and no Get call in this build reads through PropertySet - it is
            // read some other way, or only written: both columns are compared, and
            // the string wins when both moved, because that is the column a value
            // with meaning in it lives in.
            if (entry.known)
            {
               const bool textMoved = kind != SettingsDefaults::KindNumber && text.Compare(shippedText.c_str()) != 0;
               const bool numberMoved = kind != SettingsDefaults::KindString && number != shippedNumber;

               if (textMoved)
               {
                  entry.value = Masked(text, entry.secret);
                  entry.shipped = Masked(shippedText, entry.secret);
               }
               else if (numberMoved)
               {
                  // Masked as the string column is. No property whose value is a
                  // secret is read as a number today, so nothing reaches this
                  // masked; writing it unmasked anyway would make "a secret is
                  // named and not shown" true of three branches out of four, and
                  // the one that was not would be found by whoever added the first
                  // numeric setting with "token" or "secret" in its name.
                  entry.value = Masked(StringParser::IntToString((int) number), entry.secret);
                  entry.shipped = Masked(StringParser::IntToString((int) shippedNumber), entry.secret);
               }
               else
                  continue;
            }
            else
            {
               unknownNames++;
               entry.value = Masked(text.IsEmpty() ? StringParser::IntToString((int) number) : text, entry.secret);
            }

            entries.push_back(entry);
         }
      }
   }

   void
   SettingsDiff::Read(std::vector<Entry> &entries, int &considered)
   {
      int unknownNames = 0;
      entries.clear();

      ReadIniSettings(entries, unknownNames);
      ReadDatabaseSettings(entries, unknownNames);

      considered = Considered(unknownNames);
   }

   AnsiString
   SettingsDiff::Json()
   {
      std::vector<Entry> entries;
      int considered = 0;

      Read(entries, considered);

      AnsiString json = "{" + JsonNumber("changed", (int) entries.size()) + "," +
                        JsonNumber("considered", considered) + ",\"settings\":[";

      for (size_t i = 0; i < entries.size(); i++)
      {
         const Entry &entry = entries[i];

         if (i > 0)
            json += ",";

         json += "{" + JsonField("store", entry.store) + "," +
                 JsonField("name", entry.name) + "," +
                 JsonField("value", entry.value) + "," +
                 JsonField("default", entry.shipped) + "," +
                 JsonBool("secret", entry.secret) + "," +
                 JsonBool("known", entry.known) + "," +
                 JsonBool("overridden", entry.overridden) + "}";
      }

      json += "]}";
      return json;
   }

   bool
   SettingsDiff::ShippedValue(const String &name, String &value)
   {
      value.Empty();

      bool isInteger = false;
      if (IniSettingStore::DefaultFor(name, value, isInteger))
         return true;

      String shippedText;
      long shippedNumber = 0;
      SettingsDefaults::SettingKind kind = SettingsDefaults::KindEither;
      if (!DatabaseDefaultFor(name, shippedText, shippedNumber, kind))
         return false;

      // The column the property uses. Without the kind this would have to guess
      // from the values, and would answer "0" for every string property whose
      // shipped value is empty - SyslogHost among them.
      if (kind == SettingsDefaults::KindString)
         value = shippedText;
      else if (kind == SettingsDefaults::KindNumber)
         value = StringParser::IntToString((int) shippedNumber);
      else
         value = shippedText.IsEmpty() ? StringParser::IntToString((int) shippedNumber) : shippedText;

      return true;
   }
}
