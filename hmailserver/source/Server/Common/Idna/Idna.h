// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <string>
#include <vector>

namespace HM
{
   // Unicode Technical Standard #46, Unicode IDNA Compatibility Processing
   // (revision 36, Unicode 18.0.0): what a browser does to the host of a link
   // before it looks the name up. The WHATWG URL standard's "domain to ASCII"
   // is its ToASCII, so a link to "evil.example" written in fullwidth letters
   // with an ideographic full stop (U+FF45 ... U+3002 ...) goes to evil.example,
   // and one to "b<U+00FC>cher.example" to xn--bcher-kva.example. Done here from tables
   // generated out of Unicode's own data files (IdnaTables.h,
   // build/generate-idna-tables.py) and held to Unicode's conformance tests,
   // IdnaTestV2.txt, on Windows and Linux alike (IdnaTester): the same answer on
   // both, whatever Unicode version the operating system has.
   //
   // A domain name is a sequence of code points - UTF-16 is read into one before
   // it gets here, a surrogate on its own being a code point like any other.
   namespace Idna
   {
      typedef std::vector<unsigned int> CodePoints;

      // UTS 46's flags (sections 4 and 4.2). IgnoreInvalidPunycode is not one:
      // both readers here, the URL standard and IdnaTestV2.txt, set it false.
      struct Options
      {
         Options();

         bool useStd3AsciiRules;
         bool checkHyphens;
         bool checkBidi;
         bool checkJoiners;
         bool transitionalProcessing;
         bool verifyDnsLength;

         // Not UTS 46's: a label of more code points than this, after mapping and
         // normalisation, that holds one past ASCII or is written "xn--" is not
         // checked nor converted to or from Punycode, and gets
         // ErrorLabelTooLong; an ASCII label not written "xn--" is processed
         // whatever its length. 0, the default, is no limit. No label of more
         // than 63 code points has an ASCII form DNS can carry - every code point
         // is a character of it at least - and Punycode takes time that grows
         // with the square of a label's length, so a reader of untrusted text
         // that only wants names it can look up (SURBL) says 63.
         //
         // Nor is such a label mapped whole: once its mapping holds a code point
         // past ASCII and more than four times this many code points, it can
         // never be short enough - NFC makes a label four times shorter at most,
         // no code point's canonical decomposition being longer than four - and
         // the rest of it is only looked up for the full stop that ends it and
         // for the direction of its code points (ProcessLabels). Its unicode and
         // mapped then hold the label as far as it was mapped, in NFC. So one
         // label of a megabyte costs time and memory in proportion to its
         // length, where a mapping of up to 18 code points for one (U+FDFA) and
         // NFC over the whole of it made both many times that. A name of many
         // labels is not held so: each label short enough is mapped whole, and
         // a megabyte of them costs what their mappings do. A reader that wants
         // a name's cost bounded weighs the name before it processes it
         // (KindOfMapping), as SURBL's reading of a link's host does.
         size_t longestLabel;
      };

      // The WHATWG URL standard's flags, those of its "domain to ASCII" with
      // beStrict false: CheckBidi and CheckJoiners, nothing else. What Options()
      // is.
      Options UrlStandardOptions();

      // IdnaTestV2.txt's: every check on, Nontransitional Processing.
      Options ConformanceOptions();

      // What was wrong, as bits; IdnaTestV2.txt's status codes beside each.
      enum Error
      {
         ErrorPunycode = 1 << 0,       // P4, A3: an "xn--" label holds non-ASCII,
                                       // is not Punycode, or decodes to nothing or
                                       // to ASCII alone; or a label's Punycode
                                       // would overflow
         ErrorNotNfc = 1 << 1,         // V1
         ErrorHyphens34 = 1 << 2,      // V2: "--" in the third and fourth places
         ErrorHyphenAtEnd = 1 << 3,    // V3: a hyphen first or last
         ErrorXnPrefix = 1 << 4,       // V4: begins "xn--" (CheckHyphens off)
         ErrorFullStop = 1 << 5,       // V5
         ErrorLeadingMark = 1 << 6,    // V6: begins with a combining mark
         ErrorStatus = 1 << 7,         // V7: a code point neither valid nor (Nontransitional) a deviation
         ErrorStd3 = 1 << 8,           // U1: an ASCII code point not a-z, 0-9 or '-'
         ErrorJoiner = 1 << 9,         // C1, C2: ZERO WIDTH NON-JOINER or JOINER out of context
         ErrorBidi = 1 << 10,          // B1-B6: RFC 5893's rule, in a bidi domain name
         ErrorDomainLength = 1 << 11,  // A4_1: VerifyDnsLength, the name not 1 to 253
         ErrorLabelLength = 1 << 12,   // A4_2: VerifyDnsLength, a label not 1 to 63
         ErrorLabelTooLong = 1 << 13   // Options::longestLabel
      };

      // One label of a domain name after Processing (section 4).
      struct Label
      {
         Label();

         // The label as Processing leaves it: mapped, in NFC, and an "xn--"
         // label decoded from Punycode when it could be.
         CodePoints unicode;

         // The label as the mapping and NFC left it, before an "xn--" label
         // was decoded: what the text wrote, read as UTS 46 reads it. The same
         // as unicode for a label not written "xn--".
         CodePoints mapped;

         // ToASCII's form of it (section 4.2, step 3): itself when it is ASCII,
         // otherwise "xn--" and its Punycode. Empty when the Punycode would
         // overflow, or the label is longer than Options::longestLabel.
         std::string ascii;

         // Whether the label was written as "xn--" (after mapping).
         bool punycode;

         // The errors recorded against this label (bits of Error).
         unsigned int errors;
      };

      // Processing (section 4) of domain, label by label, with each label's
      // ASCII form. Never fails: the errors are in the labels.
      void ProcessLabels(const CodePoints &domain, const Options &options, std::vector<Label> &labels);

      // Whether UTS 46 ignores the code point: deletes it from a domain name
      // before anything else (IdnaMappingTable.txt's "ignored" - the soft
      // hyphen, the zero-width space, the variation selectors and the like).
      bool IsIgnored(unsigned int codePoint);

      // What UTS 46's mapping (section 4, step 1, Nontransitional Processing)
      // writes a code point as, told without writing it, for a reader that
      // weighs a name before it processes it: nothing, for a code point it
      // ignores; a full stop alone (U+002E, U+3002, U+FF0E, U+FF61 - no other
      // mapping holds a full stop, build/generate-idna-tables.py holds the data
      // to that); a name's characters alone, a-z, 0-9, '-' and '_' (a capital,
      // a fullwidth letter, a circled one, the trade mark sign's "tm"); other
      // ASCII alone (a space, a fullwidth solidus's '/', U+2100's "a/c"); or
      // code points one of which is past ASCII - as every code point past ASCII
      // it does not map is written, itself. No mapping but an ignored code
      // point's is empty.
      enum MappingKind
      {
         MapsToNothing = 0,
         MapsToFullStop = 1,
         MapsToNameCharacters = 2,
         MapsToOtherAscii = 3,
         MapsPastAscii = 4
      };

      MappingKind KindOfMapping(unsigned int codePoint);

      // Whether UTS 46's mapping (Nontransitional) writes a code point past
      // ASCII as text holding one the URL standard forbids in a domain - a
      // space (U+FDFA, a no-break space, the spacing accents), a '/' (U+2100,
      // a fullwidth solidus), a ':', a '|' and the like - so that no browser
      // reaches a host holding it: the URL standard's host parser fails a
      // domain whose ASCII form holds one (DomainToAscii). False for ASCII.
      bool MapsToAForbiddenDomainCodePoint(unsigned int codePoint);

      // ToASCII (section 4.2): the errors, none when it succeeded, and the name
      // as far as the conversion went.
      unsigned int ToAscii(const CodePoints &domain, const Options &options, std::string &ascii);

      // ToUnicode (section 4.3): the errors, and the converted name.
      unsigned int ToUnicode(const CodePoints &domain, const Options &options, CodePoints &unicode);

      // The URL standard's "domain parser" with beStrict false
      // (url.spec.whatwg.org, section 3.3, as of 7 October 2026), on a host
      // already percent-decoded as UTF-8: an ASCII domain lowered, whatever
      // ToASCII would say of it; any other through ToASCII with
      // UrlStandardOptions; and a failure when the result is empty or holds a
      // forbidden domain code point. False when a browser fails the host.
      bool DomainToAscii(const CodePoints &domain, std::string &ascii);

      // RFC 3492, on one label without its "xn--". Encoding fails when the
      // input would overflow Punycode's integers; decoding when the input is
      // not Punycode, or decodes past U+10FFFF or to a code point below U+0080
      // that the basic part did not hold.
      bool PunycodeEncode(const CodePoints &input, std::string &output);
      bool PunycodeDecode(const std::string &input, CodePoints &output);

      // Unicode Normalization Form C (UAX #15), and whether text is in it.
      void ToNfc(CodePoints &text);
      bool IsNfc(const CodePoints &text);

      // Text as UTF-8 and back, for the testers: a code point that is not a
      // scalar value (a surrogate) is written and read as its three bytes,
      // as IdnaTestV2.txt's lone surrogates need. FromUtf8 reads a byte that
      // starts no sequence, or a sequence cut short, as U+FFFD.
      std::string ToUtf8(const CodePoints &text);
      CodePoints FromUtf8(const std::string &text);
   }

   // UTS 46 held to IdnaTestV2.txt - every line, ToUnicode, ToASCII
   // Nontransitional and Transitional with every check on, and ToASCII again
   // with the URL standard's flags, the status codes of the checks it leaves off
   // set aside as the file says - and to cases of its own: Punycode against
   // RFC 3492's samples, NFC, the URL standard's domain to ASCII (fullwidth
   // letters, ideographic full stops, forbidden code points), the time a long
   // label takes, a label as written beside the label it decodes to, and what
   // KindOfMapping says of a code point, held to the mapping at both ends of
   // every range of the mapping table. It
   // runs inside the service with the other class tests (ClassTester), and on
   // Linux in idna-tests. No file, DNS, database or setting: the test data is
   // compiled in. It throws std::logic_error naming the first thing that is
   // wrong.
   class IdnaTester
   {
   public:
      void Test();

      // How many of IdnaTestV2.txt's lines the last Test() ran: all of them,
      // when it returned.
      size_t GetLinesRun() const { return linesRun_; }

   private:
      size_t linesRun_ = 0;
   };
}
