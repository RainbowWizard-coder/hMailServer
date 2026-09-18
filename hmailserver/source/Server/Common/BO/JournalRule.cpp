// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"
#include "JournalRule.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   JournalRule::JournalRule() :
      scope_(ScopeDomain),
      domain_id_(0),
      account_id_(0),
      direction_(DirectionBoth),
      mode_(ModeReport),
      enabled_(true)
   {

   }

   JournalRule::~JournalRule()
   {

   }

   bool
   JournalRule::XMLStore(XNode *parentNode, int options)
   {
      XNode *node = parentNode->AppendChild(_T("JournalRule"));

      // The target by name and never by id: a restore reissues every domain and
      // account id, and the rule is carried under its domain (Domain::XMLStore),
      // whose new id JournalRules::PreSaveObject supplies. An account rule is
      // resolved from the address again once the domain's accounts are back.
      node->AppendAttr(_T("Scope"), StringParser::IntToString((int) scope_));
      node->AppendAttr(_T("Target"), target_);
      node->AppendAttr(_T("JournalAddress"), journal_address_);
      node->AppendAttr(_T("Direction"), StringParser::IntToString((int) direction_));
      node->AppendAttr(_T("Mode"), StringParser::IntToString((int) mode_));
      node->AppendAttr(_T("Enabled"), enabled_ ? _T("1") : _T("0"));
      node->AppendAttr(_T("Note"), note_);

      return true;
   }

   bool
   JournalRule::XMLLoad(XNode *node, int options)
   {
      scope_ = _ttoi(node->GetAttrValue(_T("Scope"))) == ScopeAccount ? ScopeAccount : ScopeDomain;
      target_ = node->GetAttrValue(_T("Target"));
      journal_address_ = node->GetAttrValue(_T("JournalAddress"));

      // A number the file should not carry lands on the value that copies the
      // most rather than the least: a rule restored with a direction nobody
      // recognises must not quietly stop journaling half of what it covered.
      const int direction = _ttoi(node->GetAttrValue(_T("Direction")));
      direction_ = (direction >= DirectionInbound && direction <= DirectionBoth) ? (Direction) direction : DirectionBoth;

      mode_ = _ttoi(node->GetAttrValue(_T("Mode"))) == ModeCopy ? ModeCopy : ModeReport;

      // An absent Enabled restores as enabled, which is what a rule in the file
      // was; an empty string is not "0".
      const String enabledValue = node->GetAttrValue(_T("Enabled"));
      enabled_ = enabledValue.IsEmpty() ? true : enabledValue == _T("1");

      note_ = node->GetAttrValue(_T("Note"));

      return true;
   }
}
