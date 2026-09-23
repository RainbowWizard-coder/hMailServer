// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"
#include "PlusAddressing.h"
#include "../Common/BO/Domain.h"
#include "../Common/BO/Account.h"
#include "../Common/Cache/CacheContainer.h"
#include "../Common/Application/EffectiveSetting.h"

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
      // empty part before the character names no account and is not looked up.
      std::shared_ptr<const Account> account = sAddressPart.IsEmpty()
         ? std::shared_ptr<const Account>()
         : CacheContainer::Instance()->GetAccount(sAccountAddress);

      if (!EffectiveSetting::ResolveBool(_T("plus_addressing_enabled"), pDomain, account))
      {
         // Plus addressing is not enabled for this recipient.
         return sAddress;
      }

      return sAccountAddress;
   }

}