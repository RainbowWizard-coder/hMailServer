// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "NeverBanName.h"
#include "../AntiSpam/DMARC/PublicSuffixList.h"
#include "../Util/Time.h"
#include "../Util/Parsing/StringParser.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   NeverBanName::NeverBanName()
   {
      created_ = DateTime::GetCurrentTime();
   }

   NeverBanName::~NeverBanName()
   {
   }

   String
   NeverBanName::NormalisePattern(const String &pattern)
   {
      String value = pattern;
      value.Trim();
      value.ToLower();

      // A trailing dot is the DNS root, which a reverse name carries and a person
      // never types on purpose; matching is by label, so it has to go.
      while (value.GetLength() > 1 && value.EndsWith(_T(".")))
         value = value.Left(value.GetLength() - 1);

      return value;
   }

   void
   NeverBanName::SetPattern(const String &newVal)
   {
      pattern_ = NormalisePattern(newVal);
   }

   bool
   NeverBanName::IsSuffix() const
   {
      return pattern_.GetLength() > 1 && pattern_.StartsWith(_T("."));
   }

   bool
   NeverBanName::PatternCovers(const String &pattern, const String &normalisedName)
   {
      const String value = NormalisePattern(pattern);

      if (value.IsEmpty() || value == _T(".") || normalisedName.IsEmpty())
         return false;

      if (!value.StartsWith(_T(".")))
         return normalisedName == value;

      // .example.net covers example.net and everything beneath it, anchored at
      // a label boundary: notexample.net is not covered, and neither is
      // example.net.attacker.test.
      const String apex = value.Mid(1);

      if (normalisedName == apex)
         return true;

      return normalisedName.GetLength() > value.GetLength() &&
             normalisedName.EndsWith(value);
   }

   bool
   NeverBanName::Covers(const String &normalisedName) const
   {
      return PatternCovers(pattern_, normalisedName);
   }

   bool
   NeverBanName::IsPublicSuffix(const String &normalisedName)
   {
      String name = normalisedName;
      if (name.StartsWith(_T(".")))
         name = name.Mid(1);

      if (name.IsEmpty())
         return false;

      const std::vector<String> labels = StringParser::SplitString(name, _T("."));
      if (labels.empty())
         return false;

      // One label is a suffix by the list's implicit "*" rule: localhost, a
      // top-level domain.
      if (labels.size() == 1)
         return true;

      return PublicSuffixList::GetPublicSuffixLabelCount(labels) >= labels.size();
   }

   bool
   NeverBanName::ValidatePattern(const String &pattern, String &refusal)
   {
      const String value = NormalisePattern(pattern);

      if (value.IsEmpty() || value == _T("."))
      {
         refusal = _T("pattern is required: a reverse name such as mail.example.net, or .example.net for the domain and every name beneath it");
         return false;
      }

      if (value.GetLength() > 255)
      {
         refusal = _T("pattern is at most 255 characters");
         return false;
      }

      if (value.Find(_T("@")) >= 0)
      {
         refusal = _T("pattern is a host or domain name, not an address: this list is matched against the reverse name of the connecting address");
         return false;
      }

      if (value.Find(_T("*")) >= 0)
      {
         // The refusal says how to write what was meant: *.example.org is
         // .example.org. Only a pattern this check would take is echoed - one star,
         // as the first label, before a name that is no public suffix - so nothing
         // of the text goes back unvalidated; any other star is answered with the
         // example.
         String meant = _T(".example.net");
         String unused;
         if (value.StartsWith(_T("*.")) && value.Find(_T("*"), 1) < 0 && ValidatePattern(value.Mid(1), unused))
            meant = value.Mid(1);

         refusal = String(_T("pattern has no * wildcard: write ")) + meant + _T(" for ") + meant.Mid(1) + _T(" and every name beneath it");
         return false;
      }

      if (value.Find(_T(" ")) >= 0 || value.Find(_T("\t")) >= 0 || value.Find(_T("/")) >= 0 ||
          value.Find(_T("[")) >= 0 || value.Find(_T("]")) >= 0 || value.Find(_T(":")) >= 0)
      {
         refusal = _T("pattern must be a single host or domain name: an address or a network belongs in AutoBanNeverBan, which lists addresses and CIDR blocks");
         return false;
      }

      const String name = value.StartsWith(_T(".")) ? value.Mid(1) : value;

      if (StringParser::IsValidIPAddress(name))
      {
         refusal = _T("pattern is a name, not an address: an address belongs in AutoBanNeverBan, which lists addresses and CIDR blocks");
         return false;
      }

      if (name.IsEmpty() || name.StartsWith(_T(".")) || !StringParser::IsValidDomainName(name))
      {
         refusal = _T("pattern must be a host or domain name such as mail.example.net, or .example.net for the domain and every name beneath it");
         return false;
      }

      // The one rule the domain list does not have. A suffix such as .com or
      // .co.uk, or a hosting suffix of the list's private section such as
      // .github.io, would spare every name anybody can register beneath it -
      // and an address's reverse name is set by whoever holds the address.
      if (IsPublicSuffix(name))
      {
         refusal = _T("pattern is a public suffix, under which anybody can register a name: list the provider's own domain, such as .example.net, instead");
         return false;
      }

      return true;
   }

   bool
   NeverBanName::XMLStore(XNode *pParentNode, int iOptions)
   {
      XNode *pNode = pParentNode->AppendChild(_T("NeverBanName"));

      pNode->AppendAttr(_T("Pattern"), pattern_);
      pNode->AppendAttr(_T("Note"), note_);
      pNode->AppendAttr(_T("Created"), Time::GetTimeStampFromDateTime(created_));

      return true;
   }

   bool
   NeverBanName::XMLLoad(XNode *pNode, int iOptions)
   {
      SetPattern(pNode->GetAttrValue(_T("Pattern")));
      note_ = pNode->GetAttrValue(_T("Note"));

      created_ = Time::GetDateFromSystemDate(pNode->GetAttrValue(_T("Created")));
      if (created_.GetStatus() != DateTime::valid)
         created_ = DateTime::GetCurrentTime();

      return true;
   }
}
