// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"
#include "SessionPolicies.h"

#include "Unicode.h"
#include "../Application/Configuration.h"
#include "../Application/EffectiveSetting.h"
#include "../BO/Account.h"
#include "../BO/Domain.h"
#include "../Cache/CacheContainer.h"
#include "../Util/Parsing/StringParser.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      AnsiString JsonEscape(const AnsiString &value)
      {
         AnsiString result;
         result.reserve(value.GetLength() + 8);

         for (int i = 0; i < value.GetLength(); i++)
         {
            char character = value[i];

            switch (character)
            {
            case '\"':
               result += "\\\"";
               break;
            case '\\':
               result += "\\\\";
               break;
            case '\r':
               result += "\\r";
               break;
            case '\n':
               result += "\\n";
               break;
            case '\t':
               result += "\\t";
               break;
            default:
               if (static_cast<unsigned char>(character) >= 0x20)
                  result += character;
               break;
            }
         }

         return result;
      }

      AnsiString Utf8Of(const String &value)
      {
         AnsiString utf8;
         Unicode::WideToMultiByte(value, utf8);
         return utf8;
      }
   }

   void
   SessionPolicies::ReadServer_(SessionPolicy &policy)
   {
      Configuration *configuration = Configuration::Instance();

      policy.server_idle_seconds = configuration->GetSessionIdleSeconds();
      policy.server_lifetime_seconds = configuration->GetSessionLifetimeSeconds();
      policy.server_one_per_device = configuration->GetSessionOnePerDevice();
      policy.server_bind_to_address = configuration->GetSessionBindToAddress();
   }

   void
   SessionPolicies::ApplyDomain_(SessionPolicy &policy, std::shared_ptr<const Domain> domain)
   {
      if (!domain)
         return;

      policy.has_domain = true;
      policy.domain_name = domain->GetName();
      policy.domain_idle_seconds = domain->GetSessionIdleSeconds();
      policy.domain_lifetime_seconds = domain->GetSessionLifetimeSeconds();
      policy.domain_one_per_device = domain->GetSessionOnePerDevice();
      policy.domain_bind_to_address = domain->GetSessionBindToAddress();
   }

   void
   SessionPolicies::ApplyAccount_(SessionPolicy &policy, std::shared_ptr<const Account> account)
   {
      if (!account)
         return;

      policy.has_account = true;
      policy.account_address = account->GetAddress();

      const EffectiveSetting::Definition *idle = EffectiveSetting::Find(_T("session_idle_seconds"));
      const EffectiveSetting::Definition *lifetime = EffectiveSetting::Find(_T("session_lifetime_seconds"));
      const EffectiveSetting::Definition *onePerDevice = EffectiveSetting::Find(_T("session_one_per_device"));
      const EffectiveSetting::Definition *bindToAddress = EffectiveSetting::Find(_T("session_bind_to_address"));

      if (idle)
         EffectiveSetting::AccountValue(*idle, account, policy.account_idle_seconds);
      if (lifetime)
         EffectiveSetting::AccountValue(*lifetime, account, policy.account_lifetime_seconds);
      if (onePerDevice)
         EffectiveSetting::AccountValue(*onePerDevice, account, policy.account_one_per_device);
      if (bindToAddress)
         EffectiveSetting::AccountValue(*bindToAddress, account, policy.account_bind_to_address);
   }

   void
   SessionPolicies::Resolve_(SessionPolicy &policy, std::shared_ptr<const Domain> domain, std::shared_ptr<const Account> account)
   {
      // The account's own value where it has one, then its domain's, then the
      // server's, then the ceiling that has always applied - one resolution,
      // Application/EffectiveSetting's, and the rule this function held for the
      // domain and the server before an account had a level of its own. A
      // stored value outside the acceptable range is treated as unset rather
      // than honoured: the interfaces refuse such a number, so one in the
      // database was put there by hand, and honouring it would let a typed
      // zero-too-many lock a domain out.
      //
      // A lifetime shorter than the idle period is not refused - both are real
      // ceilings and the shorter one simply wins - but it is worth knowing that
      // the idle period then never fires.
      policy.idle_seconds = EffectiveSetting::ResolveInt(_T("session_idle_seconds"), domain, account);
      policy.lifetime_seconds = EffectiveSetting::ResolveInt(_T("session_lifetime_seconds"), domain, account);
      policy.one_per_device = EffectiveSetting::ResolveBool(_T("session_one_per_device"), domain, account);
      policy.bind_to_address = EffectiveSetting::ResolveBool(_T("session_bind_to_address"), domain, account);

      if (policy.idle_seconds <= 0)
         policy.idle_seconds = DefaultIdleSeconds;
      if (policy.lifetime_seconds <= 0)
         policy.lifetime_seconds = DefaultLifetimeSeconds;
   }

   SessionPolicy
   SessionPolicies::ServerWide()
   {
      SessionPolicy policy;

      ReadServer_(policy);
      Resolve_(policy, std::shared_ptr<const Domain>(), std::shared_ptr<const Account>());

      return policy;
   }

   SessionPolicy
   SessionPolicies::ForDomain(const String &domainName)
   {
      SessionPolicy policy;

      ReadServer_(policy);

      std::shared_ptr<const Domain> domain;
      if (!domainName.IsEmpty())
      {
         domain = CacheContainer::Instance()->GetDomain(domainName);
         ApplyDomain_(policy, domain);
      }

      Resolve_(policy, domain, std::shared_ptr<const Account>());

      return policy;
   }

   SessionPolicy
   SessionPolicies::ForAddress(const String &address)
   {
      std::shared_ptr<const Account> account = CacheContainer::Instance()->GetAccount(address);
      if (!account)
         return ForDomain(StringParser::ExtractDomain(address));

      return ForAccount(account);
   }

   SessionPolicy
   SessionPolicies::ForAccount(std::shared_ptr<const Account> account)
   {
      if (!account)
         return ServerWide();

      SessionPolicy policy;

      ReadServer_(policy);

      std::shared_ptr<const Domain> domain = CacheContainer::Instance()->GetDomain(account->GetDomainID());
      ApplyDomain_(policy, domain);
      ApplyAccount_(policy, account);

      Resolve_(policy, domain, account);

      return policy;
   }

   String
   SessionPolicies::CheckSeconds(int seconds)
   {
      if (seconds < 0)
         return _T("A session policy is a number of seconds, or 0 for the server's own ceiling; it cannot be negative.");

      if (seconds > MaximumSeconds)
         return _T("The longest a session policy may be is 2592000 seconds - thirty days. A longer one is the absence of a policy rather than a policy.");

      return String();
   }

   String
   SessionPolicies::CheckSwitch(int value)
   {
      if (value < SwitchInherit || value > SwitchOff)
         return _T("A domain's session switch is 0 to take the server's answer, 1 for on, or 2 for off.");

      return String();
   }

   AnsiString
   SessionPolicies::ToJson(const SessionPolicy &policy)
   {
      AnsiString json;

      json.Format("{\"domain\":\"%hs\",\"in_force\":{\"idle_seconds\":%d,\"lifetime_seconds\":%d,\"one_per_device\":%hs,\"bind_to_address\":%hs},"
                  "\"server\":{\"idle_seconds\":%d,\"lifetime_seconds\":%d,\"one_per_device\":%hs,\"bind_to_address\":%hs},"
                  "\"defaults\":{\"idle_seconds\":%d,\"lifetime_seconds\":%d}",
         JsonEscape(Utf8Of(policy.domain_name)).c_str(),
         policy.idle_seconds, policy.lifetime_seconds,
         policy.one_per_device ? "true" : "false",
         policy.bind_to_address ? "true" : "false",
         policy.server_idle_seconds, policy.server_lifetime_seconds,
         policy.server_one_per_device ? "true" : "false",
         policy.server_bind_to_address ? "true" : "false",
         (int) DefaultIdleSeconds, (int) DefaultLifetimeSeconds);

      if (policy.has_domain)
      {
         AnsiString domainPart;
         domainPart.Format(",\"domain_policy\":{\"idle_seconds\":%d,\"lifetime_seconds\":%d,\"one_per_device\":%d,\"bind_to_address\":%d}",
            policy.domain_idle_seconds, policy.domain_lifetime_seconds,
            policy.domain_one_per_device, policy.domain_bind_to_address);

         json += domainPart;
      }

      // The account's own values, where the policy was asked for an account:
      // null is "its domain's" (docs/SettingLevels.md).
      if (policy.has_account)
      {
         auto own = [](const String &value, bool flag) -> AnsiString
         {
            if (value.IsEmpty())
               return "null";
            if (flag)
               return value == _T("1") ? "true" : "false";
            return AnsiString(value);
         };

         json += ",\"account\":\"" + JsonEscape(Utf8Of(policy.account_address)) + "\"";
         json += ",\"account_policy\":{\"idle_seconds\":" + own(policy.account_idle_seconds, false) +
                 ",\"lifetime_seconds\":" + own(policy.account_lifetime_seconds, false) +
                 ",\"one_per_device\":" + own(policy.account_one_per_device, true) +
                 ",\"bind_to_address\":" + own(policy.account_bind_to_address, true) + "}";
      }

      json += "}";

      return json;
   }
}
