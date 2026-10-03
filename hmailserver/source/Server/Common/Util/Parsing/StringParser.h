// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

namespace HM
{
   class StringParser  
   {
   public:
      StringParser();
      virtual ~StringParser();

      static String ExtractDomain(const String &sEMailAddress);
      static String ExtractAddress(const String &sEMailAddress);

      static bool IsValidEmailAddress(const String &sEmailAddress);
      // RFC 6531/6532 (SMTPUTF8/EAI): when allowInternationalized is true the
      // local-part and domain may contain non-ASCII (UTF-8) characters.
      static bool IsValidEmailAddress(const String &sEmailAddress, bool allowInternationalized);
      static bool IsValidDomainName(const String &sEmailAddress);
      // Returns true if the value contains any character outside the 7-bit US-ASCII range.
      static bool ContainsNonAscii(const String &sValue);
      static bool WildcardMatch(const String &pattern, const String &value);
      static bool WildcardMatchNoCase(const String &sWildcard, const String &sString);
      // The regular expression WildcardMatch tests with: * for any run of
      // characters, ? for one, and every other character of the pattern taken
      // literally. Public so that a caller matching ONE pattern against many
      // values can compile it once instead of once per value, and still be
      // asking exactly what WildcardMatch asks.
      static String WildcardToRegularExpression(const String &pattern);

      static char* Search(const char *haystack, size_t haystackSize, const char *needle);

      // Where an e-mail address occurs in text as a whole address, from from on,
      // or std::string::npos. Both are compared as they are given - lower-case
      // both first for a match without case. A match is whole when the byte
      // before it could not be part of a local part (RFC 5322 atext, '.', or a
      // UTF-8 byte) and the byte after it could not continue the domain (a
      // letter, a digit, '-', a UTF-8 byte, or a '.' followed by one of those),
      // so ann@example.com is not found inside joann@example.com nor
      // ann@example.com.au, and is found in <ann@example.com> and at the end of a
      // sentence. Three atext bytes the logs write in front of an address count
      // as a boundary before it: '=', '/', and a quotation mark that opens a
      // quotation ('ann@example.com', not o'ann@example.com).
      static size_t FindWholeAddress(const std::string &text, const std::string &address, size_t from = 0);

      static String IntToString(int lTheInt);
      static String IntToString(unsigned int lTheInt);
      static String IntToString(__int64 lTheInt);

      static bool TryParseInt(const std::string &str, int &value);

      
      static std::vector<String> SplitString(const String &sInput, const String &sSeperators);
      static std::vector<AnsiString> SplitString(const AnsiString &sInput, const AnsiString &sSeperators);
      // Splits a string into a vector
   
      static String JoinVector(const std::vector<String> &sVector, const String &sSeperators);
      // Joins a vector into a long string

      static void Base64Encode(const String &sInput, String &sOutput);
      static void Base64Decode(const String &sInput, String &sOutput);
      
      static bool IsValidIPAddress(const String &sAddress);
      static String ToUSASCII(const String &sInput);
      static std::vector<String> GetAllButFirst(std::vector<String> sInput);

      static bool IsNumeric(const String &sInput);

      static bool ValidateString(const String &sString, const String &sAllowedChars);

      static void RemoveDuplicateItems(std::vector<String> &items);

      // RFC 4013 SASLprep (pragmatic subset): normalises a SASL credential string by
      // mapping non-ASCII space characters to U+0020 and removing the RFC 3454 table B.1
      // "mapped to nothing" characters (soft hyphen, zero-width spaces/joiners, BOM, ...).
      // Returns true and the prepared string on success; returns false if the input
      // contains a prohibited control character (RFC 3454 tables C.2.1/C.2.2). Full NFKC
      // normalisation is not performed (it would require a Unicode library); ASCII input
      // is therefore returned unchanged.
      static bool SaslPrep(const String &sInput, String &sOutput);

      // Decodes a SASL PLAIN base64 token (RFC 4616: authzid NUL authcid NUL passwd)
      // into its three UTF-8 fields. Returns true when exactly three fields are present.
      static bool DecodeSaslPlain(const String &sBase64, String &sAuthzid, String &sAuthcid, String &sPassword);

   private:

      static bool AnyOfCharsExists_(const String &sChars, const String &sLookIn);
   
   };

   class StringParserTester
   {
   public :
      StringParserTester () {};
      ~StringParserTester () {};      
      
      void Test();
   };

   // String's FindNoCase, ContainsNoCase and ReplaceNoCase (StdString.h) held
   // to what they answered up to 6.3.5 - the code of then, kept here - over
   // generated cases, wide and narrow, and to the time they take: one pass,
   // where 20,000 occurrences took ReplaceNoCase 43 seconds. Run by ClassTester
   // on Windows and by imap-parse-tests on Linux.
   class StringNoCaseTester
   {
   public:
      void Test(int cases = 20000);
   };

}