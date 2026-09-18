// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "DomainPolicyEntry.h"
#include "../Util/Time.h"
#include "../Util/Parsing/StringParser.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   DomainPolicyEntry::DomainPolicyEntry() :
      match_on_(MatchBoth),
      action_(ActionWhite)
   {
      created_ = DateTime::GetCurrentTime();
   }

   DomainPolicyEntry::~DomainPolicyEntry()
   {
   }

   void
   DomainPolicyEntry::SetPattern(const String &newVal)
   {
      String value = newVal;
      value.Trim();
      value.ToLower();

      // A trailing dot is the DNS root, which a HELO name may carry and a person
      // never types on purpose; matching is by label, so it has to go.
      while (value.GetLength() > 1 && value.EndsWith(_T(".")))
         value = value.Left(value.GetLength() - 1);

      pattern_ = value;
   }

   bool
   DomainPolicyEntry::IsSuffix() const
   {
      return pattern_.GetLength() > 1 && pattern_.StartsWith(_T("."));
   }

   bool
   DomainPolicyEntry::Covers(const String &normalisedName) const
   {
      if (pattern_.IsEmpty() || normalisedName.IsEmpty())
         return false;

      if (!IsSuffix())
         return normalisedName == pattern_;

      // .example.com covers example.com and everything beneath it, anchored at
      // a label boundary: notexample.com is not covered, and neither is
      // example.com.evil.test.
      const String apex = pattern_.Mid(1);

      if (normalisedName == apex)
         return true;

      return normalisedName.GetLength() > pattern_.GetLength() &&
             normalisedName.EndsWith(pattern_);
   }

   String
   DomainPolicyEntry::MatchOnWord(MatchOn value)
   {
      switch (value)
      {
      case MatchEnvelope:
         return _T("envelope");
      case MatchHelo:
         return _T("helo");
      default:
         return _T("both");
      }
   }

   bool
   DomainPolicyEntry::ReadMatchOn(const String &word, MatchOn &value)
   {
      String lower = word;
      lower.Trim();
      lower.ToLower();

      if (lower == _T("envelope"))
         value = MatchEnvelope;
      else if (lower == _T("helo"))
         value = MatchHelo;
      else if (lower == _T("both"))
         value = MatchBoth;
      else
         return false;

      return true;
   }

   String
   DomainPolicyEntry::ActionWord(Action value)
   {
      return value == ActionBlack ? _T("black") : _T("white");
   }

   bool
   DomainPolicyEntry::ReadAction(const String &word, Action &value)
   {
      String lower = word;
      lower.Trim();
      lower.ToLower();

      if (lower == _T("white"))
         value = ActionWhite;
      else if (lower == _T("black"))
         value = ActionBlack;
      else
         return false;

      return true;
   }

   bool
   DomainPolicyEntry::ValidatePattern(const String &pattern, String &refusal)
   {
      String value = pattern;
      value.Trim();
      value.ToLower();

      while (value.GetLength() > 1 && value.EndsWith(_T(".")))
         value = value.Left(value.GetLength() - 1);

      if (value.IsEmpty() || value == _T("."))
      {
         refusal = _T("pattern is required: a domain such as example.com, or .example.com for the domain and everything beneath it");
         return false;
      }

      if (value.Find(_T("@")) >= 0)
      {
         refusal = _T("pattern is a domain, not an address: this list is keyed by the envelope sender's domain or the HELO name, never by a mailbox");
         return false;
      }

      if (value.Find(_T(" ")) >= 0 || value.Find(_T("\t")) >= 0 || value.Find(_T("/")) >= 0)
      {
         refusal = _T("pattern must be a single domain name, with no spaces and no path");
         return false;
      }

      // An address literal is not a name, and this list is the one keyed by
      // names: an address belongs in an IP range or the anti-spam white list.
      if (value.Find(_T("[")) >= 0 || value.Find(_T("]")) >= 0)
      {
         refusal = _T("pattern is a domain name, not an address literal: an IP address belongs in an IP range or the anti-spam white list");
         return false;
      }

      const String domain = value.StartsWith(_T(".")) ? value.Mid(1) : value;

      if (domain.IsEmpty() || domain.StartsWith(_T(".")) || !StringParser::IsValidDomainName(domain))
      {
         refusal = _T("pattern must be a domain name such as example.com, or .example.com for the domain and everything beneath it");
         return false;
      }

      if (value.GetLength() > 255)
      {
         refusal = _T("pattern is at most 255 characters");
         return false;
      }

      return true;
   }

   bool
   DomainPolicyEntry::XMLStore(XNode *pParentNode, int iOptions)
   {
      XNode *pNode = pParentNode->AppendChild(_T("DomainPolicyEntry"));

      pNode->AppendAttr(_T("Pattern"), pattern_);
      pNode->AppendAttr(_T("MatchOn"), MatchOnWord(match_on_));
      pNode->AppendAttr(_T("Action"), ActionWord(action_));
      pNode->AppendAttr(_T("Note"), note_);
      pNode->AppendAttr(_T("Created"), Time::GetTimeStampFromDateTime(created_));

      return true;
   }

   bool
   DomainPolicyEntry::XMLLoad(XNode *pNode, int iOptions)
   {
      SetPattern(pNode->GetAttrValue(_T("Pattern")));

      MatchOn matchOn = MatchBoth;
      if (ReadMatchOn(pNode->GetAttrValue(_T("MatchOn")), matchOn))
         match_on_ = matchOn;

      // An archive that says nothing readable about the action restores the
      // entry as white, never as black: a row that refuses mail is the one that
      // must not be conjured out of a blank.
      Action action = ActionWhite;
      if (ReadAction(pNode->GetAttrValue(_T("Action")), action))
         action_ = action;

      note_ = pNode->GetAttrValue(_T("Note"));

      created_ = Time::GetDateFromSystemDate(pNode->GetAttrValue(_T("Created")));
      if (created_.GetStatus() != DateTime::valid)
         created_ = DateTime::GetCurrentTime();

      return true;
   }
}
