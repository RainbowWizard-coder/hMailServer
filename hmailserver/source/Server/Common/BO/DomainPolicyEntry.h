// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "../Util/VariantDateTime.h"

namespace HM
{
   // One row of the domain white and black list (discussion #285): a sending
   // organisation named by its domain rather than by the address of the week.
   // DomainPolicyList decides what a match does; this is the row.
   //
   // The pattern is either a whole domain, example.com, which matches that name
   // alone, or a suffix written with a leading dot, .example.com, which matches
   // the name and every name beneath it. It is matched against the envelope
   // sender's domain, the HELO/EHLO name, or both, and it is white or black.
   class DomainPolicyEntry : public BusinessObject<DomainPolicyEntry>
   {
   public:
      enum MatchOn
      {
         MatchEnvelope = 0,
         MatchHelo = 1,
         MatchBoth = 2
      };

      enum Action
      {
         ActionWhite = 0,
         ActionBlack = 1
      };

      DomainPolicyEntry();
      ~DomainPolicyEntry();

      // What the collection's GetItemByName compares.
      String GetName() const { return pattern_; }

      String GetPattern() const { return pattern_; }

      // Trimmed, lower-cased and stripped of a trailing dot on the way in, so
      // that every reader compares the text the SMTP session compares.
      void SetPattern(const String &newVal);

      MatchOn GetMatchOn() const { return match_on_; }
      void SetMatchOn(MatchOn newVal) { match_on_ = newVal; }

      Action GetAction() const { return action_; }
      void SetAction(Action newVal) { action_ = newVal; }

      String GetNote() const { return note_; }
      void SetNote(const String &newVal) { note_ = newVal; }

      DateTime GetCreated() const { return created_; }
      void SetCreated(DateTime newVal) { created_ = newVal; }

      // True when the pattern is a suffix (written with a leading dot).
      bool IsSuffix() const;

      // Whether this entry's pattern covers the name: the name itself for a
      // whole domain, the name or anything beneath it for a suffix. The name is
      // expected already normalised as SetPattern normalises.
      bool Covers(const String &normalisedName) const;

      // The words the REST document, COM and the Deck use for the two enums, and
      // the way back. Reading answers false for a word that is not one of them.
      static String MatchOnWord(MatchOn value);
      static bool ReadMatchOn(const String &word, MatchOn &value);
      static String ActionWord(Action value);
      static bool ReadAction(const String &word, Action &value);

      // What a pattern must be: a domain name, or a dot and a domain name, with
      // no @ and no whitespace. The refusal says which rule it broke.
      static bool ValidatePattern(const String &pattern, String &refusal);

      bool XMLStore(XNode *pNode, int iOptions);
      bool XMLLoad(XNode *pNode, int iOptions);
      bool XMLLoadSubItems(XNode *pNode, int iOptions) { return true; }

   private:

      String pattern_;
      MatchOn match_on_;
      Action action_;
      String note_;
      DateTime created_;
   };
}
