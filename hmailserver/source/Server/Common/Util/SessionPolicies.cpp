// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"
#include "SessionPolicies.h"

#include "Unicode.h"
#include "../Application/Configuration.h"
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
   SessionPolicies::Resolve_(SessionPolicy &policy)
   {
      // The domain first where it has an answer, then the server, then the
      // ceiling that has always applied. A stored value outside the acceptable
      // range is treated as unset rather than honoured: the interfaces refuse
      // such a number, so one in the database was put there by hand, and
      // honouring it would let a typed zero-too-many lock a domain out.
      int idle = 0;
      if (policy.domain_idle_seconds > 0 && policy.domain_idle_seconds <= MaximumSeconds)
         idle = policy.domain_idle_seconds;
      else if (policy.server_idle_seconds > 0 && policy.server_idle_seconds <= MaximumSeconds)
         idle = policy.server_idle_seconds;

      policy.idle_seconds = idle > 0 ? idle : DefaultIdleSeconds;

      int lifetime = 0;
      if (policy.domain_lifetime_seconds > 0 && policy.domain_lifetime_seconds <= MaximumSeconds)
         lifetime = policy.domain_lifetime_seconds;
      else if (policy.server_lifetime_seconds > 0 && policy.server_lifetime_seconds <= MaximumSeconds)
         lifetime = policy.server_lifetime_seconds;

      policy.lifetime_seconds = lifetime > 0 ? lifetime : DefaultLifetimeSeconds;

      // A lifetime shorter than the idle period is not refused - both are real
      // ceilings and the shorter one simply wins - but it is worth knowing that
      // the idle period then never fires.
      policy.one_per_device = policy.domain_one_per_device == SwitchOn ? true :
                              policy.domain_one_per_device == SwitchOff ? false :
                              policy.server_one_per_device;

      policy.bind_to_address = policy.domain_bind_to_address == SwitchOn ? true :
                               policy.domain_bind_to_address == SwitchOff ? false :
                               policy.server_bind_to_address;
   }

   SessionPolicy
   SessionPolicies::ServerWide()
   {
      SessionPolicy policy;

      ReadServer_(policy);
      Resolve_(policy);

      return policy;
   }

   SessionPolicy
   SessionPolicies::ForDomain(const String &domainName)
   {
      SessionPolicy policy;

      ReadServer_(policy);

      if (!domainName.IsEmpty())
         ApplyDomain_(policy, CacheContainer::Instance()->GetDomain(domainName));

      Resolve_(policy);

      return policy;
   }

   SessionPolicy
   SessionPolicies::ForAddress(const String &address)
   {
      return ForDomain(StringParser::ExtractDomain(address));
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

      json += "}";

      return json;
   }
}
