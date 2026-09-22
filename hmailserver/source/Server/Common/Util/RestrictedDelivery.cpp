// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "RestrictedDelivery.h"

#include "Parsing/StringParser.h"
#include "../Application/Logger.h"
#include "../Application/ObjectCache.h"
#include "../AntiSpam/DomainPolicyList.h"
#include "../BO/Account.h"
#include "../BO/Domain.h"
#include "../BO/DomainAliases.h"
#include "../BO/DomainPolicyEntry.h"
#include "../Cache/CacheContainer.h"

#include <set>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // The characters that separate names in the list as typed. A comma, a
      // semicolon and any whitespace; the stored form uses ", ".
      bool IsSeparator_(wchar_t c)
      {
         return c == ',' || c == ';' || c == ' ' || c == '\t' || c == '\r' || c == '\n';
      }
   }

   bool
   RestrictedDelivery::IsRestricted(const Account &account, const Domain &domain)
   {
      const int mode = account.GetRestrictedDelivery();

      if (mode == Exempt)
         return false;

      if (mode == Restricted)
         return true;

      // Follows the domain.
      String list = domain.GetRestrictedDomains();
      list.Trim();
      return !list.IsEmpty();
   }

   std::vector<String>
   RestrictedDelivery::EffectiveList(const Account &account, const Domain &domain)
   {
      std::vector<String> patterns;

      if (!IsRestricted(account, domain))
         return patterns;

      // The stored form is canonical, but a row a restore or a hand edit put
      // there is read with the same tolerance the canonicaliser has.
      const String list = domain.GetRestrictedDomains();
      String current;

      for (int i = 0; i <= list.GetLength(); i++)
      {
         const wchar_t c = i < list.GetLength() ? list.GetAt(i) : L',';

         if (IsSeparator_(c))
         {
            if (!current.IsEmpty())
            {
               current.ToLower();
               patterns.push_back(current);
               current = _T("");
            }
         }
         else
         {
            current += c;
         }
      }

      return patterns;
   }

   String
   RestrictedDelivery::DomainOf_(const String &address)
   {
      // The domain as written, compared as the policy list compares a name -
      // lower-cased, no trailing dot, and empty for an address literal.
      return DomainPolicyList::NormaliseName(StringParser::ExtractDomain(address));
   }

   String
   RestrictedDelivery::ResolvedDomainOf_(const String &address)
   {
      // The same, after the domain aliases: an address at one of a hosted
      // domain's alias names is an address in that domain. Both spellings are
      // judged, because a list may name the alias name - the name the
      // administrator knows the other side by - or the primary.
      std::shared_ptr<DomainAliases> aliases = ObjectCache::Instance()->GetDomainAliases();
      if (!aliases)
         return DomainOf_(address);

      return DomainPolicyList::NormaliseName(StringParser::ExtractDomain(aliases->ApplyAliasesOnAddress(address)));
   }

   RestrictedDelivery::Verdict
   RestrictedDelivery::Check(const Account &account, const Domain &domain, const String &otherAddress, Direction direction)
   {
      if (!IsRestricted(account, domain))
         return Allowed;

      String other = otherAddress;
      other.Trim();
      other.ToLower();

      // The null sender: a bounce or a DSN, which must reach a restricted
      // account or it never learns that a message failed. Outbound, a
      // recipient is never empty; an empty one is nothing to refuse.
      if (other.IsEmpty())
         return Allowed;

      // The other side's domain in both spellings: as written, and after the
      // domain aliases (an address at an alias name of a hosted domain is an
      // address in that domain). Either may be the account's own domain, and
      // either may be the name the list holds.
      const String writtenDomain = DomainOf_(other);
      const String resolvedDomain = ResolvedDomainOf_(other);

      String ownName = domain.GetName();
      ownName.Trim();
      ownName.ToLower();

      if ((!writtenDomain.IsEmpty() && writtenDomain == ownName) || (!resolvedDomain.IsEmpty() && resolvedDomain == ownName))
         return Allowed;

      // The domain's postmaster, wherever the administrator put that address.
      String postmaster = domain.GetPostmaster();
      postmaster.Trim();
      postmaster.ToLower();

      if (!postmaster.IsEmpty() && other == postmaster)
         return Allowed;

      // The named domains, matched as the domain policy list matches a name:
      // a whole domain is that name; .example is example and everything
      // beneath it, anchored at a label boundary. The written spelling first,
      // then the resolved one when it differs, and the log says which.
      if (!writtenDomain.IsEmpty())
      {
         const std::vector<String> patterns = EffectiveList(account, domain);

         for (const String &pattern : patterns)
         {
            DomainPolicyEntry entry;
            entry.SetPattern(pattern);

            if (entry.Covers(writtenDomain))
            {
               LOG_DEBUG(_T("RestrictedDelivery: allowed ") + String(direction == Inbound ? _T("inbound") : _T("outbound")) +
                         _T(" for ") + account.GetAddress() + _T(": ") + other + _T(" is covered by ") + pattern + _T(" as written."));
               return Allowed;
            }

            if (resolvedDomain != writtenDomain && !resolvedDomain.IsEmpty() && entry.Covers(resolvedDomain))
            {
               LOG_DEBUG(_T("RestrictedDelivery: allowed ") + String(direction == Inbound ? _T("inbound") : _T("outbound")) +
                         _T(" for ") + account.GetAddress() + _T(": ") + other + _T(" is covered by ") + pattern +
                         _T(" through the domain alias, as ") + resolvedDomain + _T("."));
               return Allowed;
            }
         }
      }

      LOG_DEBUG(_T("RestrictedDelivery: refused ") + String(direction == Inbound ? _T("inbound") : _T("outbound")) +
                _T(" for ") + account.GetAddress() + _T(": ") + other +
                (direction == Inbound ? _T(" is not a named domain's sender") : _T(" is not in a named domain")) +
                (resolvedDomain != writtenDomain && !resolvedDomain.IsEmpty() ? _T(" (nor is ") + resolvedDomain + _T(", which the domain alias resolves it to)") : String()) +
                _T(". The list is \"") + domain.GetRestrictedDomains() + _T("\" and the account's setting is ") + ModeWord(account.GetRestrictedDelivery()) + _T("."));

      return Refused;
   }

   RestrictedDelivery::Verdict
   RestrictedDelivery::Check(std::shared_ptr<const Account> account, const String &otherAddress, Direction direction)
   {
      if (!account)
         return Allowed;

      std::shared_ptr<const Domain> domain = CacheContainer::Instance()->GetDomain(account->GetDomainID());
      if (!domain)
         return Allowed;

      return Check(*account, *domain, otherAddress, direction);
   }

   bool
   RestrictedDelivery::CanonicaliseList(const String &text, String &canonical, String &refusal)
   {
      String value = text;
      value.Trim();

      if (value.GetLength() > MaxListLength)
      {
         refusal.Format(_T("restricted_domains is at most %d characters: domain names separated by commas or spaces."), MaxListLength);
         return false;
      }

      for (int i = 0; i < value.GetLength(); i++)
      {
         if ((unsigned) value.GetAt(i) > 127)
         {
            refusal = _T("restricted_domains is ASCII, like the domain policy list: write an internationalised domain name in its IDNA form (xn--...).");
            return false;
         }
      }

      std::vector<String> names;
      std::set<String> seen;
      String current;

      for (int i = 0; i <= value.GetLength(); i++)
      {
         const wchar_t c = i < value.GetLength() ? value.GetAt(i) : L',';

         if (!IsSeparator_(c))
         {
            current += c;
            continue;
         }

         if (current.IsEmpty())
            continue;

         current.ToLower();

         // The list names domains, never mailboxes: a list keyed by addresses
         // would be the sender list, which is another feature.
         if (current.Find(_T("@")) >= 0)
         {
            refusal = _T("restricted_domains names domains, not addresses: ") + current +
                      _T(" is an address. Write the domain (partner.example), or .example for the domain and everything beneath it.");
            return false;
         }

         // The rest of what a name must be is what the domain policy list
         // requires of a pattern, in its own sentence.
         String patternRefusal;
         if (!DomainPolicyEntry::ValidatePattern(current, patternRefusal))
         {
            refusal = _T("restricted_domains: ") + current + _T(": ") + patternRefusal;
            return false;
         }

         while (current.GetLength() > 1 && current.EndsWith(_T(".")))
            current = current.Left(current.GetLength() - 1);

         if (seen.insert(current).second)
            names.push_back(current);

         current = _T("");
      }

      canonical = _T("");

      for (size_t i = 0; i < names.size(); i++)
      {
         if (i > 0)
            canonical += _T(", ");

         canonical += names[i];
      }

      if (canonical.GetLength() > MaxListLength)
      {
         refusal.Format(_T("restricted_domains is at most %d characters: domain names separated by commas or spaces."), MaxListLength);
         return false;
      }

      return true;
   }

   bool
   RestrictedDelivery::ReadMode(const String &word, int &mode)
   {
      String lower = word;
      lower.Trim();
      lower.ToLower();

      if (lower == _T("0") || lower == _T("domain"))
         mode = FollowsDomain;
      else if (lower == _T("1") || lower == _T("restricted"))
         mode = Restricted;
      else if (lower == _T("2") || lower == _T("exempt"))
         mode = Exempt;
      else
         return false;

      return true;
   }

   String
   RestrictedDelivery::ModeWord(int mode)
   {
      switch (mode)
      {
      case Restricted:
         return _T("restricted");
      case Exempt:
         return _T("exempt");
      default:
         return _T("domain");
      }
   }

   String
   RestrictedDelivery::InboundRefusal(const String &recipientAddress)
   {
      return recipientAddress + _T(": this mailbox accepts mail from named domains only.");
   }

   String
   RestrictedDelivery::OutboundRefusal(const String &recipientAddress)
   {
      return recipientAddress + _T(": this account may send to named domains only.");
   }
}
