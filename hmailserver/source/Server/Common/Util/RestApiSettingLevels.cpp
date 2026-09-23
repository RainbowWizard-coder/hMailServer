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
#include "../Cache/CacheContainer.h"

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
      // sentence for a member that is not of the setting's own JSON type: a
      // whole number for an integer, true or false for a switch, a string for
      // text - as the routes' own fields have always been read, so that true is
      // never a 1 KB limit and "30" never thirty days.
      bool MemberText(const EffectiveSetting::Definition &definition, const JsonValue &member, String &text, AnsiString &error)
      {
         const AnsiString name(definition.name);

         switch (definition.type)
         {
         case EffectiveSetting::TypeBoolean:
            if (member.IsBool())
            {
               text = member.AsBool() ? _T("1") : _T("0");
               return true;
            }
            error = name + " must be true or false, or null to take the level above";
            return false;

         case EffectiveSetting::TypeInteger:
            if (member.IsNumber())
            {
               const double number = member.AsNumber();
               if (std::floor(number) == number && number >= -2147483648.0 && number <= 2147483647.0)
               {
                  text = StringParser::IntToString((__int64) number);
                  return true;
               }
            }
            error = name + " must be a whole number, or null to take the level above";
            return false;

         default:
            if (member.IsString())
            {
               Unicode::MultiByteToWide(AnsiString(member.AsString().c_str()), text);
               return true;
            }
            error = name + " must be text, or null to take the level above";
            return false;
         }
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
      Read(const JsonValue &body, int scope, __int64 ownerId, bool serverAuthority, std::vector<Change> &changes, AnsiString &error, bool &forbidden)
      {
         forbidden = false;
         changes.clear();

         for (const std::pair<std::string, JsonValue> &member : body.Members())
         {
            if (!IsOverlayKey(member.first, scope))
               continue;

            const EffectiveSetting::Definition *definition = EffectiveSetting::Find(String(member.first.c_str()));

            // What the level holds now. A member that says what the level
            // already holds - an entry read and sent back as it was - changes
            // nothing, and asks nobody's authority or the ceiling: it is left
            // out, so that such a round trip is taken whoever sends it.
            String current;
            bool held = false;
            if (ownerId > 0)
            {
               std::shared_ptr<const std::map<String, String> > rows = CacheContainer::Instance()->GetSettingOverrides(scope, ownerId);
               if (rows)
               {
                  auto found = rows->find(String(definition->name));
                  if (found != rows->end())
                  {
                     String refused;
                     held = definition->canonicalise(found->second, current, refused);
                  }
               }
            }

            if (member.second.IsNull() && !held)
               continue;

            if (!member.second.IsNull() && held)
            {
               String text;
               String canonical;
               String refused;
               AnsiString ignored;
               if (MemberText(*definition, member.second, text, ignored) && definition->canonicalise(text, canonical, refused) && canonical == current)
                  continue;
            }

            if (definition->server_administrators_only() && !serverAuthority)
            {
               error = AnsiString(definition->name) + " is a capability the server grants, and the server administrator's to set for a " +
                       (scope == PersistentSettingOverride::ScopeDomain ? "domain" : "account") +
                       "; a key restricted to named domains cannot open or close it";
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
            if (!MemberText(*definition, member.second, text, error))
               return false;

            String canonical;
            String refusal;
            if (!definition->canonicalise(text, canonical, refusal) ||
                !EffectiveSetting::HoldsToCeiling(*definition, scope, ownerId, canonical, refusal))
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
         return "The settings that may be set below the server (GET /api/v1/settings/levels lists them; docs/SettingLevels.md) are answered for each as <name>_effective, the value in effect, and <name>_source, where it came from - server, domain or account - and, where the level is held in the overlay rather than in a field of its own, as <name>, the level's own value or null when it takes the level above. A body sets such a <name> with a value of its own JSON type (a whole number, true or false, text) and makes it inherit with null; a field of its own keeps its contract, its 0 or -1 taking the level above, and the authority it always had. <name>_effective and <name>_source are answered, and ignored when sent back. A limit (class limit) may be tightened below the level above and never loosened: a looser value is a 400 naming the level above. A gate (class gate) is the server administrator's: a key restricted to named domains is refused it with 403.";
      }

   }

   AnsiString
   RestApiServer::OpenApiSettingLevelsPaths_()
   {
      return
         ",\"/api/v1/settings/levels\":{\"get\":{\"summary\":\"The settings that may be set below the server\","
         "\"description\":\"One value, three levels (docs/SettingLevels.md): every setting a domain or an account may hold its own value of, as {settings:[{name, type, levels, server, server_key, domain_storage, account_storage, class, tighter, combine, server_administrators_only, server_per_address, domain_column_capped, server_value}]}. name is how the domain and account routes, COM's SettingOverride, EffectiveSetting and EffectiveSettingSource, and hmctl spell it; type is integer, boolean or string; levels is the deepest level it may be set at; server says where the server's value comes from - property (a row of hm_settings), ini (a key of the settings store), ini-section ([SendingLimits], which the store does not hold) or default (no server setting: the value a domain is created with) - and server_key its name there; server_per_address is true where the server holds a value per address (the [SendingLimitsOverrides] line of the sending limits), which is then that address's server level; domain_storage and account_storage say where each level is held - column (a field of the domain or the account that already held it, whose 0 or -1 takes the level above, with the authority that field always had) or overlay (hm_settingoverrides, where no row takes the level above); class is limit (it protects the server or the other tenants: a level below may tighten the level above and never loosen it - a looser value is refused when written, and capped when the level above has since tightened past it - and anybody who administers the level may tighten it), gate (a capability the server grants: the server administrator's at every level) or preference (the tenant's own: the deepest level that holds a value wins); tighter says which way a limit tightens - lower, lower-zero-is-none (0 is no limit at all) or on - and is none for the rest; combine is ceiling for a limit and override otherwise; server_administrators_only is true for a gate; domain_column_capped is true where a limit's domain field has always been capped by the server's value (the message size) - a limit's other domain fields go either way from the server's, as they always did, and the ceiling binds the account; server_value is the server's value now. Server-wide; refused for domain-restricted keys.\","
         "\"responses\":{\"200\":{\"description\":\"{settings:[...]}\"}}}}";
   }
}
