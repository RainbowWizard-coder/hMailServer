// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// Outbound content policy: what a rule can ask about a message before it
// leaves. Four questions, each answered as a text the rule engine's own match
// types read (contains, a wildcard, a regular expression, their negations), in
// the shape the attachment-name condition set: a list with ";" between the
// entries, so "contains x" reads as "any of them is x".
//
//   ReadableText      the text a person could read: the plain part, the HTML
//                     part with its tags removed, and every attachment whose
//                     bytes are text - whatever its name and declared type
//   Detectors         which detectors found something in that text:
//                     card_number (13 to 19 digits, with spaces or hyphens
//                     between groups, that pass the Luhn check - as a whole or
//                     up to one of the separators, so a card followed by its
//                     security code is found), iban (a country from the IBAN
//                     registry, its length, mod 97) and account_number (the
//                     DlpAccountNumberPattern setting's regular expression,
//                     when one is set, run through RuleGuard's breaker so a
//                     pattern that does not compile or that Boost abandons is
//                     reported as such and rested for a window)
//   AttachmentTypes   every attachment's type by its first bytes, not its
//                     name: executable, elf, pdf, zip, office_xml, jar,
//                     office, rar, 7z, gzip, png, jpeg, gif, rtf, script,
//                     text, empty or binary
//   ExternalRecipients every envelope recipient this server does not host
//
// A detector is a pattern, and a pattern has false positives: a 16-digit
// reference number passes Luhn one time in ten. The rule's other conditions -
// an external recipient, a domain - are what keep a policy from firing on the
// company's own invoices, and docs/OutboundContentPolicy.md says so.

namespace HM
{
   class MessageData;
   class Message;

   class ContentPolicy
   {
   public:
      static String ReadableText(std::shared_ptr<MessageData> data);

      static String Detectors(const String &text);
      static String Detectors(const String &text, const String &accountNumberPattern);

      static bool ContainsCardNumber(const String &text);
      static bool ContainsIban(const String &text);
      static bool MatchesAccountNumberPattern(const String &text, const String &pattern);

      // Whether a pattern is one the account_number detector could run: the
      // question the setting's REST check asks before saving it, and the
      // start-up tester asks without a report being written for the answer.
      static bool PatternCompiles(const String &pattern);

      static String AttachmentTypes(std::shared_ptr<MessageData> data);
      static AnsiString TypeOfBytes(const AnsiString &bytes);

      // A text attachment saved as UTF-16 - Excel's "Unicode Text", a Notepad
      // file - begins with a byte-order mark, and every other byte of Latin
      // text in it is NUL, which the byte test reads as binary. True, with the
      // text decoded, when the bytes begin with the mark; false otherwise, and
      // the caller reads them as UTF-8.
      static bool Utf16Text(const AnsiString &bytes, String &text);

      static String ExternalRecipients(std::shared_ptr<Message> message);

      // The two checks, on the bare characters: digits only for Luhn, the
      // country, check digits and account part with no spaces for the IBAN.
      static bool LuhnValid(const AnsiString &digits);
      static bool IbanValid(const AnsiString &iban);

      // The length the IBAN registry gives a country's IBAN, 0 for a code that
      // is not in it.
      static int IbanLength(char first, char second);

      static String StripHtml(const String &html);

   private:
      static bool LooksLikeText_(const AnsiString &bytes);
      static bool IsCardNumber_(const AnsiString &digits);
   };

   class ContentPolicyTester
   {
   public:
      void Test();

   private:
      void TestLuhn_();
      void TestIban_();
      void TestDetectors_();
      void TestTypes_();
   };
}
