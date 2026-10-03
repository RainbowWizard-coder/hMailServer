// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

namespace HM
{
   // The quadratic-time sweep of 6.3.6: nine readers of what a remote party
   // sends that took time growing with the square of its size, or faster, each
   // made a single pass. Every test holds the new code to what 6.3.5's code -
   // kept here - made of the same generated cases, and to a time bound over an
   // input built to be the old code's worst, which the new code passes in
   // milliseconds and the old took seconds to minutes over. Nothing touches the
   // network, the configuration or a database.
   //
   // On Windows ClassTester runs them; on Linux, input-cost-tests. Each throws
   // std::logic_error on a failure.
   class InputCostTester
   {
   public:
      void Test();

      // MimeEncodedWord::Decode: a word of an encoding other than B or Q.
      static void TestEncodedWords();

      // MimeHeader::DeleteFields (MessageData::DeleteField) and
      // MimeHeader::RemoveFieldsAt (Sieve's deleteheader).
      static void TestFieldDeletion();

      // The DKIM and ARC header canonicalisations' choice of fields.
      static void TestDkimHeaderSelection();

      // Sieve's :matches.
      static void TestSieveMatches();

      // SPF's cut of a name too long to look up (RFC 7208 7.1).
      static void TestSpfTruncation();

      // MimeField::RemoveParameter.
      static void TestParameterRemoval();

      // LIST's lines, each once.
      static void TestListLines();

      // JSON objects' members, the first of a repeated name kept.
      static void TestJsonMembers();

      // An organiser's CANCEL of a stored series from an occurrence on, over
      // an RDATE of many values and a rule of many parts, and over dates,
      // overrides and a DTSTART in a time zone of many transitions (unreleased
      // code of 6.3.6; ImipTester holds what it makes).
      static void TestCalendarCancel();
   };
}
