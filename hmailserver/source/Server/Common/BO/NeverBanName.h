// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "../Util/VariantDateTime.h"

namespace HM
{
   // One row of the never-ban list by reverse name (GitLab work item #56,
   // hm_neverbannames): a sending provider named by the reverse names of its
   // outbound hosts, for the operator whose provider's addresses move too often
   // for AutoBanNeverBan, which lists addresses. Util/NeverBanNameList.h decides
   // what a match does; this is the row.
   //
   // The pattern follows the domain white and black list's rules
   // (BO/DomainPolicyEntry.h): a whole name, mail.example.net, matches that
   // name alone, and a suffix written with a leading dot, .example.net, matches
   // the name and every name beneath it, at a label boundary. One more rule of
   // its own: a pattern that is itself a public suffix (com, co.uk, github.io)
   // is refused, since it would spare every name registered beneath it.
   class NeverBanName : public BusinessObject<NeverBanName>
   {
   public:
      NeverBanName();
      ~NeverBanName();

      // What the collection's GetItemByName compares.
      String GetName() const { return pattern_; }

      String GetPattern() const { return pattern_; }

      // Trimmed, lower-cased and stripped of a trailing dot on the way in, so
      // that every reader compares the text the ban writer compares.
      void SetPattern(const String &newVal);

      String GetNote() const { return note_; }
      void SetNote(const String &newVal) { note_ = newVal; }

      DateTime GetCreated() const { return created_; }
      void SetCreated(DateTime newVal) { created_ = newVal; }

      // True when the pattern is a suffix (written with a leading dot).
      bool IsSuffix() const;

      // Whether this row's pattern covers the name: the name itself for a
      // whole name, the name or anything beneath it for a suffix. The name is
      // expected already normalised (NeverBanNameList::NormaliseName).
      bool Covers(const String &normalisedName) const;

      // The same, for a pattern given as text, normalised as SetPattern does.
      static bool PatternCovers(const String &pattern, const String &normalisedName);

      // What a pattern must be: a host or domain name, or a dot and one, that
      // is not an address, not a wildcard written with a star, and not a public
      // suffix. The refusal says which rule it broke.
      static bool ValidatePattern(const String &pattern, String &refusal);

      // The pattern as SetPattern stores it.
      static String NormalisePattern(const String &pattern);

      // Whether a lower-cased name with no trailing dot is a public suffix or
      // shorter by the compiled-in Public Suffix List, both sections, which is
      // what the quarantine's review asks (QuarantineReview::IsPublicSuffix):
      // com, co.uk, github.io, and a name of one label such as localhost, which
      // the list's implicit rule makes one. Empty is not a suffix.
      static bool IsPublicSuffix(const String &normalisedName);

      bool XMLStore(XNode *pNode, int iOptions);
      bool XMLLoad(XNode *pNode, int iOptions);
      bool XMLLoadSubItems(XNode *pNode, int iOptions) { return true; }

   private:

      String pattern_;
      String note_;
      DateTime created_;
   };
}
