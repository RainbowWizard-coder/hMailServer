// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

namespace HM
{
   // One rule for what "this entry covers that sender" means, in one place.
   //
   // It was written for the server-wide blocked-sender list and lived inside
   // BlockedSender::Matches. The reader's own safe-sender and blocked-sender
   // lists ask the same question of the same kind of text, and a second
   // implementation of it would be a second answer: an administrator who has
   // learned that "example.com" covers "mail.example.com" but not
   // "notexample.com" must not have to learn it twice, differently. So the rule
   // moved here and BlockedSender::Matches now calls it - the extraction is the
   // whole point, not a tidy-up.
   //
   // The rule, deliberately narrower than the white list's wildcard matching:
   //
   //   * An entry containing '@' is one exact address, matched
   //     case-insensitively. No wildcards: a list a person keeps must be
   //     predictable, and "notspammer@example.com" must never be caught by an
   //     entry for "spammer@example.com".
   //   * An entry without '@' is a domain, and covers that domain and any
   //     subdomain of it, anchored at a label boundary, so "example.com" covers
   //     "mail.example.com" and neither "notexample.com" nor
   //     "example.com.attacker.net".
   //   * A leading '@' on a domain entry is tolerated: "@example.com" means
   //     "example.com".
   //   * The null sender (an empty address) matches nothing. <> carries
   //     bounces, and a list that could swallow those breaks RFC 5321.
   class SenderPattern
   {
   public:
      static bool Matches(const String &pattern, const String &address);

      // How specific a pattern is, for deciding which of two entries in the
      // same list wins when both match: an address (2) beats a domain (1), and
      // a longer domain beats a shorter one, which is what makes
      // "mail.example.com" an exception to "example.com" rather than a
      // duplicate of it. 0 is "matches nothing".
      //
      // The number returned for a domain is 1 plus the number of labels, so
      // "co.uk" (3) loses to "example.co.uk" (4). Label count rather than
      // string length, because a long name is not a specific one.
      static int Specificity(const String &pattern);

      // What a person may type into one of these lists. Empty is refused, a
      // pattern with a space in it is refused, and an address entry has to have
      // something on both sides of its '@'. out_error is the sentence to show
      // them, in the API's own voice.
      static bool Validate(const String &pattern, String &out_error);

      // The pattern as it is stored: trimmed, lower-cased, and with a leading
      // '@' removed, so that "@Example.COM " and "example.com" are one row and
      // not two. Called by the setter, so nothing downstream has to remember.
      static String Canonical(const String &pattern);
   };
}
