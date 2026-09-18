// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

namespace HM
{
   // One row of the per-domain greylisting bypass list (hm_greylisting_bypass,
   // schema 6072; discussion #284): a domain or a host suffix, what it is matched
   // against, and the authentication results a message must carry before
   // greylisting is skipped for it. A privilege the administrator grants to a
   // named domain, as against BypassGreylistingOnSPFSuccess, which every SPF pass
   // in the world enjoys. Evaluated by Common/AntiSpam/GreyListingBypass.h.
   class GreyListingBypassEntry : public BusinessObject<GreyListingBypassEntry>
   {
   public:
      // What the pattern is compared with.
      enum MatchOn
      {
         MatchEnvelope = 1,   // the domain of MAIL FROM
         MatchHelo = 2,       // the HELO/EHLO host
         MatchBoth = 3        // either
      };

      // The results a message must carry, as flags. At least one is required: a
      // row that requires nothing is the domain white list's job, not this one's.
      enum Require
      {
         RequireSpf = 1,
         RequireDkim = 2,
         RequireDmarc = 4
      };

      GreyListingBypassEntry(void);
      ~GreyListingBypassEntry(void);

      String GetName() const;

      // The pattern as stored: lower-cased, with a leading dot meaning "this name
      // and every name under it" and without one meaning "this name exactly".
      String GetPattern() const { return pattern_; }
      void SetPattern(const String &value);

      int GetMatchOn() const { return match_on_; }
      void SetMatchOn(int value) { match_on_ = value; }

      int GetRequire() const { return require_; }
      void SetRequire(int value) { require_ = value; }

      String GetNote() const { return note_; }
      void SetNote(const String &value) { note_ = value; }

      // Whether this row's pattern covers the name: exactly, or as a suffix at a
      // label boundary when the pattern starts with a dot. Case does not matter.
      bool Covers(const String &name) const;

      // The words the REST resource, hmctl and the Deck use for the two enumerations.
      static String MatchOnWord(int matchOn);
      static bool MatchOnFromWord(const String &word, int &matchOn);

      bool XMLStore(XNode *pNode, int iOptions);
      bool XMLLoad(XNode *pNode, int iOptions);
      bool XMLLoadSubItems (XNode *pNode, int iOptions) {return true; }

   private:

      String pattern_;
      int match_on_;
      int require_;
      String note_;
   };
}
