// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"
#include "PlusAddressing.h"
#include "../Common/BO/Domain.h"
#include "../Common/BO/Account.h"
#include "../Common/Cache/CacheContainer.h"
#include "../Common/Application/EffectiveSetting.h"
#include "../Common/Persistence/PersistentSettingOverride.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   PlusAddressing::PlusAddressing(void)
   {
   }

   PlusAddressing::~PlusAddressing(void)
   {
   }

   String
   PlusAddressing::ExtractAccountAddress(const String &sAddress, std::shared_ptr<const Domain> pDomain)
   {
      if (!pDomain)
         return sAddress;

      // The character is the domain's alone: it decides which account an
      // address names, so it is read before any account is known.
      String sPlusAddrChar = pDomain->GetPlusAddressingChar();

      // Extract the account address. Easier this way.
      String sAddrPart = StringParser::ExtractAddress(sAddress);

      // Locate the plus address char (if it exists)
      int iPlusCharPos = sAddrPart.Find(sPlusAddrChar);

      if (iPlusCharPos == -1)
      {
         // Plus character wasn't used in the address.
         return sAddress;
      }

      // Copy the address from the start up until (but excluding)
      // the plus addressing character.
      String sAddressPart = sAddrPart.Mid(0, iPlusCharPos);

      String sDomainName = StringParser::ExtractDomain(sAddress);
      const String sAccountAddress = sAddressPart + "@" + sDomainName;

      // Whether plus addressing is on is a setting with levels
      // (Application/EffectiveSetting.h): the account the address would name
      // may hold its own switch, over its domain's. With no account of that
      // name, or none of its own, the domain's decides - as it always did. An
      // empty part before the character names no account and is not looked up,
      // and nor is any account while no account anywhere holds a switch of its
      // own - the common case, read from the cache of the name's rows - so an
      // address with the character in it costs what it always did.
      bool anyAccountHoldsOne = false;
      if (!sAddressPart.IsEmpty())
      {
         std::shared_ptr<const std::map<std::pair<int, __int64>, String> > rows =
            CacheContainer::Instance()->GetSettingOverridesNamed(_T("plus_addressing_enabled"));

         // Unknown - the database did not answer - is looked up, as before.
         anyAccountHoldsOne = !rows;
         if (rows)
         {
            for (const auto &row : *rows)
            {
               if (row.first.first == PersistentSettingOverride::ScopeAccount)
               {
                  anyAccountHoldsOne = true;
                  break;
               }
            }
         }
      }

      std::shared_ptr<const Account> account = anyAccountHoldsOne
         ? CacheContainer::Instance()->GetAccount(sAccountAddress)
         : std::shared_ptr<const Account>();

      if (!EffectiveSetting::ResolveBool(_T("plus_addressing_enabled"), pDomain, account))
      {
         // Plus addressing is not enabled for this recipient.
         return sAddress;
      }

      return sAccountAddress;
   }

}