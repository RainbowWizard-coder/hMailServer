// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"
#include "GreyListingBypassEntry.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   GreyListingBypassEntry::GreyListingBypassEntry(void) :
      match_on_(MatchEnvelope),
      require_(RequireSpf)
   {
   }

   GreyListingBypassEntry::~GreyListingBypassEntry(void)
   {
   }

   String
   GreyListingBypassEntry::GetName() const
   {
      return pattern_;
   }

   void
   GreyListingBypassEntry::SetPattern(const String &value)
   {
      String pattern = value;
      pattern.Trim();
      pattern.ToLower();
      pattern_ = pattern;
   }

   bool
   GreyListingBypassEntry::Covers(const String &name) const
   {
      if (pattern_.IsEmpty() || name.IsEmpty())
         return false;

      String candidate = name;
      candidate.Trim();
      candidate.ToLower();

      if (pattern_[0] != _T('.'))
         return candidate == pattern_;

      // ".example.com" covers example.com itself and anything under it, at a
      // label boundary: notexample.com is not covered.
      const String bare = pattern_.Mid(1);
      if (candidate == bare)
         return true;

      return candidate.GetLength() > pattern_.GetLength() &&
             candidate.Right(pattern_.GetLength()) == pattern_;
   }

   String
   GreyListingBypassEntry::MatchOnWord(int matchOn)
   {
      switch (matchOn)
      {
      case MatchHelo:
         return _T("helo");
      case MatchBoth:
         return _T("both");
      default:
         return _T("envelope");
      }
   }

   bool
   GreyListingBypassEntry::MatchOnFromWord(const String &word, int &matchOn)
   {
      String lowered = word;
      lowered.Trim();
      lowered.ToLower();

      if (lowered == _T("envelope"))
         matchOn = MatchEnvelope;
      else if (lowered == _T("helo"))
         matchOn = MatchHelo;
      else if (lowered == _T("both"))
         matchOn = MatchBoth;
      else
         return false;

      return true;
   }

   bool
   GreyListingBypassEntry::XMLStore(XNode *pParentNode, int iOptions)
   {
      XNode *pNode = pParentNode->AppendChild(_T("BypassEntry"));

      pNode->AppendAttr(_T("Pattern"), pattern_);
      pNode->AppendAttr(_T("MatchOn"), StringParser::IntToString(match_on_));
      pNode->AppendAttr(_T("Require"), StringParser::IntToString(require_));
      pNode->AppendAttr(_T("Note"), note_);

      return true;
   }

   bool
   GreyListingBypassEntry::XMLLoad(XNode *pNode, int iOptions)
   {
      SetPattern(pNode->GetAttrValue(_T("Pattern")));
      match_on_ = _ttoi(pNode->GetAttrValue(_T("MatchOn")));
      require_ = _ttoi(pNode->GetAttrValue(_T("Require")));
      note_ = pNode->GetAttrValue(_T("Note"));

      return true;
   }
}
