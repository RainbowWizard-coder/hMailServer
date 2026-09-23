// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// One value, three levels over REST. See RestApiSettingLevels.h.

#include "StdAfx.h"

#include "RestApiSettingLevels.h"
#include "RestApiServer.h"
#include "JsonDocument.h"
#include "Unicode.h"
#include "../Application/EffectiveSetting.h"
#include "../BO/Account.h"
#include "../BO/Domain.h"
#include "../Persistence/PersistentSettingOverride.h"

#include <cmath>
#include <cstring>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      bool EndsWith(const std::string &text, const std::string &suffix)
      {
         return text.size() > suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
      }

      // Whether the entry writes the level's own value under the name already:
      // a dedicated column keeps the key it has always had.
      bool EntryHasKey(const EffectiveSetting::Definition &definition, int scope)
      {
         if (scope == PersistentSettingOverride::ScopeDomain)
            return definition.domain_column != nullptr && !definition.domain_overlay;

         return definition.account_column != nullptr && !definition.account_overlay;
      }

      // The member's value as the text the overlay takes, or false with the
      // sentence for a member of a type no setting takes.
      bool MemberText(const std::string &key, const JsonValue &member, String &text, AnsiString &error)
      {
         if (member.IsBool())
         {
            text = member.AsBool() ? _T("1") : _T("0");
            return true;
         }

         if (member.IsNumber())
         {
            const double number = member.AsNumber();
            if (std::floor(number) != number || number < -2147483648.0 || number > 2147483647.0)
            {
               error = AnsiString(key.substr(0, 48).c_str()) + " must be a whole number";
               return false;
            }

            text = StringParser::IntToString((__int64) number);
            return true;
         }

         if (member.IsString())
         {
            Unicode::MultiByteToWide(AnsiString(member.AsString().c_str()), text);
            return true;
         }

         error = AnsiString(key.substr(0, 48).c_str()) + " must be a value, or null to take the level above";
         return false;
      }
   }

   namespace SettingLevelsRest
   {
      AnsiString
      EntryFields(std::shared_ptr<const Domain> domain, std::shared_ptr<const Account> account)
      {
         const int scope = account ? PersistentSettingOverride::ScopeAccount : PersistentSettingOverride::ScopeDomain;

         size_t count = 0;
         const EffectiveSetting::Definition *table = EffectiveSetting::Definitions(count);

         AnsiString fields;

         for (size_t i = 0; i < count; i++)
         {
            const EffectiveSetting::Definition &definition = table[i];

            if (!EntryHasKey(definition, scope))
            {
               String own;
               const bool has = account
                  ? EffectiveSetting::AccountValue(definition, account, own)
                  : EffectiveSetting::DomainValue(definition, domain, own);

               fields += ",\"";
               fields += definition.name;
               fields += "\":";
               fields += has ? EffectiveSetting::JsonText(definition, own) : AnsiString("null");
            }

            const EffectiveSetting::Answer answer = EffectiveSetting::Resolve(String(definition.name), domain, account);

            fields += ",\"";
            fields += definition.name;
            fields += "_effective\":";
            fields += EffectiveSetting::JsonText(definition, answer.value);
            fields += ",\"";
            fields += definition.name;
            fields += "_source\":\"";
            fields += EffectiveSetting::SourceWord(answer.source);
            fields += "\"";
         }

         return fields;
      }

      bool
      IsOverlayKey(const std::string &key, int scope)
      {
         const EffectiveSetting::Definition *definition = EffectiveSetting::Find(String(key.c_str()));
         if (!definition || key != definition->name)
            return false;

         return EffectiveSetting::OverlayHolds(*definition, scope);
      }

      bool
      IsReadOnlyKey(const std::string &key)
      {
         for (const char *suffix : { "_effective", "_source" })
         {
            if (!EndsWith(key, suffix))
               continue;

            const std::string base = key.substr(0, key.size() - strlen(suffix));
            const EffectiveSetting::Definition *definition = EffectiveSetting::Find(String(base.c_str()));
            if (definition && base == definition->name)
               return true;
         }

         return false;
      }

      bool
      Read(const JsonValue &body, int scope, bool serverAuthority, std::vector<Change> &changes, AnsiString &error, bool &forbidden)
      {
         forbidden = false;
         changes.clear();

         for (const std::pair<std::string, JsonValue> &member : body.Members())
         {
            if (!IsOverlayKey(member.first, scope))
               continue;

            const EffectiveSetting::Definition *definition = EffectiveSetting::Find(String(member.first.c_str()));

            if (definition->server_administrators_only && !serverAuthority)
            {
               error = AnsiString(definition->name) + " is the server administrator's to set for a " +
                       (scope == PersistentSettingOverride::ScopeDomain ? "domain" : "account") +
                       "; a key restricted to named domains cannot lift its own limits";
               forbidden = true;
               return false;
            }

            Change change;
            change.name = definition->name;

            if (member.second.IsNull())
            {
               change.clear = true;
               changes.push_back(change);
               continue;
            }

            String text;
            if (!MemberText(member.first, member.second, text, error))
               return false;

            String canonical;
            String refusal;
            if (!definition->canonicalise(text, canonical, refusal))
            {
               Unicode::WideToMultiByte(refusal, error);
               return false;
            }

            change.value = canonical;
            changes.push_back(change);
         }

         return true;
      }

      bool
      Apply(const std::vector<Change> &changes, int scope, __int64 ownerId, AnsiString &error)
      {
         for (const Change &change : changes)
         {
            String refusal;
            const bool done = change.clear
               ? EffectiveSetting::ClearOverride(change.name, scope, ownerId, refusal)
               : EffectiveSetting::SetOverride(change.name, scope, ownerId, change.value, refusal);

            if (!done)
            {
               Unicode::WideToMultiByte(refusal, error);
               return false;
            }
         }

         return true;
      }

      AnsiString
      TableJson()
      {
         return EffectiveSetting::TableJson();
      }

      const char *
      DescriptionSentence()
      {
         return "The settings that may be set below the server (GET /api/v1/settings/levels lists them; docs/SettingLevels.md) are answered for each as <name>_effective, the value in effect, and <name>_source, where it came from - server, domain or account - and, where the level is held in the overlay rather than in a field of its own, as <name>, the level's own value or null when it takes the level above. A body sets such a <name> with a value and makes it inherit with null; a field of its own keeps its contract, its 0 or -1 taking the level above. <name>_effective and <name>_source are answered and never taken. A limit or a ceiling among them (the setting's server_administrators_only) is the server administrator's: a key restricted to named domains is refused it with 403.";
      }

   }

   AnsiString
   RestApiServer::OpenApiSettingLevelsPaths_()
   {
      return
         ",\"/api/v1/settings/levels\":{\"get\":{\"summary\":\"The settings that may be set below the server\","
         "\"description\":\"One value, three levels (docs/SettingLevels.md): every setting a domain or an account may hold its own value of, as {settings:[{name, type, levels, server, server_key, domain_storage, account_storage, combine, server_administrators_only, server_value}]}. name is how the domain and account routes, COM's SettingOverride, EffectiveSetting and EffectiveSettingSource, and hmctl spell it; type is integer, boolean or string; levels is the deepest level it may be set at; server says where the server's value comes from - property (a row of hm_settings), ini (a key of the settings store), ini-section ([SendingLimits], which the store does not hold) or default (no server setting: the value a domain is created with) - and server_key its name there; domain_storage and account_storage say where each level is held - column (a field of the domain or the account that already held it, whose 0 or -1 takes the level above), overlay (hm_settingoverrides, where no row takes the level above), overlay-then-file (an overlay row, else the address's [SendingLimitsOverrides] line) or none; combine is override (the deepest level that holds a value wins) or ceiling (the same, but the server's value, when it is not 0, is a ceiling no level below exceeds); server_value is the server's value now. Server-wide; refused for domain-restricted keys.\","
         "\"responses\":{\"200\":{\"description\":\"{settings:[...]}\"}}}}";
   }
}
