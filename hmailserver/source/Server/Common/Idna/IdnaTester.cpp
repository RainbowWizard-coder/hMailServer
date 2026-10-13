// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"
#include "Idna.h"
#include "IdnaTables.h"

#include <stdexcept>
#include <string>
#include <vector>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      typedef Idna::CodePoints CodePoints;

      // A number in upper-case hexadecimal, of at least so many digits.
      std::string HexDigits_(unsigned int value, size_t digits)
      {
         const char *const hexDigits = "0123456789ABCDEF";
         std::string text;
         do
         {
            text.insert(text.begin(), hexDigits[value & 0xF]);
            value >>= 4;
         } while (value != 0);
         while (text.size() < digits)
            text.insert(text.begin(), '0');
         return text;
      }

      std::string Hex_(const CodePoints &text)
      {
         if (text.empty())
            return "(empty)";

         std::string hex;
         for (size_t i = 0; i < text.size(); i++)
            hex += (i == 0 ? "U+" : " U+") + HexDigits_(text[i], 4);
         return hex;
      }

      std::string Printable_(const std::string &text)
      {
         return Hex_(Idna::FromUtf8(text));
      }

      [[noreturn]] void Fail_(const std::string &where, const std::string &what)
      {
         throw std::logic_error("IdnaTester: " + where + ": " + what);
      }

      std::string Where_(size_t line, const std::string &source)
      {
         return "IdnaTestV2.txt test line " + std::to_string(line + 1) + " (" + source + ")";
      }

      std::string Trimmed_(const std::string &text)
      {
         size_t first = 0;
         size_t last = text.size();
         while (first < last && (text[first] == ' ' || text[first] == '\t'))
            first++;
         while (last > first && (text[last - 1] == ' ' || text[last - 1] == '\t'))
            last--;
         return text.substr(first, last - first);
      }

      int HexDigit_(char c)
      {
         if (c >= '0' && c <= '9')
            return c - '0';
         if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
         if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
         return -1;
      }

      // A column's text as code points: UTF-8, with the file's escapes
      // \uXXXX and \x{XXXX} read as the code point they name, and "" as the
      // empty string (FORMAT, in the file's header).
      CodePoints Unescaped_(const std::string &column, const std::string &where)
      {
         if (column == "\"\"")
            return CodePoints();

         CodePoints text;
         std::string run;
         size_t i = 0;
         while (i < column.size())
         {
            if (column[i] != '\\')
            {
               run += column[i++];
               continue;
            }

            if (!run.empty())
            {
               const CodePoints decoded = Idna::FromUtf8(run);
               text.insert(text.end(), decoded.begin(), decoded.end());
               run.clear();
            }

            unsigned int value = 0;
            size_t digits = 0;
            if (i + 5 < column.size() && column[i + 1] == 'u')
            {
               for (size_t j = i + 2; j < i + 6; j++)
               {
                  const int digit = HexDigit_(column[j]);
                  if (digit < 0)
                     Fail_(where, "a \\u escape without four hexadecimal digits");
                  value = value * 16 + static_cast<unsigned int>(digit);
               }
               text.push_back(value);
               i += 6;
               continue;
            }

            if (i + 3 < column.size() && column[i + 1] == 'x' && column[i + 2] == '{')
            {
               size_t j = i + 3;
               while (j < column.size() && column[j] != '}')
               {
                  const int digit = HexDigit_(column[j]);
                  if (digit < 0 || ++digits > 6)
                     Fail_(where, "a \\x{} escape that is not a code point");
                  value = value * 16 + static_cast<unsigned int>(digit);
                  j++;
               }
               if (j >= column.size() || digits == 0)
                  Fail_(where, "a \\x{} escape that is not closed");
               text.push_back(value);
               i = j + 1;
               continue;
            }

            Fail_(where, "a backslash that starts no escape");
         }

         if (!run.empty())
         {
            const CodePoints decoded = Idna::FromUtf8(run);
            text.insert(text.end(), decoded.begin(), decoded.end());
         }

         return text;
      }

      // A status column: its codes, none for "[]".
      std::vector<std::string> Statuses_(const std::string &column, const std::string &where)
      {
         std::vector<std::string> codes;
         if (column.size() < 2 || column.front() != '[' || column.back() != ']')
            Fail_(where, "a status that is not in brackets: " + column);

         const std::string inside = column.substr(1, column.size() - 2);
         size_t start = 0;
         while (start <= inside.size())
         {
            size_t comma = inside.find(',', start);
            if (comma == std::string::npos)
               comma = inside.size();
            const std::string code = Trimmed_(inside.substr(start, comma - start));
            if (!code.empty())
               codes.push_back(code);
            start = comma + 1;
         }

         return codes;
      }

      bool HasErrorBeyond_(const std::vector<std::string> &codes, const std::vector<std::string> &setAside)
      {
         for (const std::string &code : codes)
         {
            bool asideThis = false;
            for (const std::string &aside : setAside)
               if (code == aside)
                  asideThis = true;
            if (!asideThis)
               return true;
         }
         return false;
      }

      std::string Joined_(const std::vector<std::string> &codes)
      {
         std::string joined = "[";
         for (size_t i = 0; i < codes.size(); i++)
            joined += (i > 0 ? ", " : "") + codes[i];
         return joined + "]";
      }

      std::string ErrorsOf_(unsigned int errors)
      {
         if (errors == 0)
            return "none";
         return "0x" + HexDigits_(errors, 1);
      }

      void ExpectPunycode_(const CodePoints &unicode, const std::string &punycode, const char *what)
      {
         std::string encoded;
         if (!Idna::PunycodeEncode(unicode, encoded) || encoded != punycode)
            Fail_(what, "encoded as \"" + encoded + "\", expected \"" + punycode + "\"");

         CodePoints decoded;
         if (!Idna::PunycodeDecode(punycode, decoded) || decoded != unicode)
            Fail_(what, "decoded as " + Hex_(decoded) + ", expected " + Hex_(unicode));
      }

      void ExpectNfc_(const CodePoints &input, const CodePoints &expected, const char *what)
      {
         CodePoints normalised = input;
         Idna::ToNfc(normalised);
         if (normalised != expected)
            Fail_(what, "NFC gave " + Hex_(normalised) + ", expected " + Hex_(expected));
         if (Idna::IsNfc(input) != (input == expected))
            Fail_(what, "IsNfc is wrong");
      }

      void ExpectDomain_(const CodePoints &domain, bool succeeds, const std::string &expected, const char *what)
      {
         std::string ascii;
         const bool result = Idna::DomainToAscii(domain, ascii);
         if (result != succeeds)
            Fail_(what, std::string("the domain parser ") + (result ? "succeeded with \"" + ascii + "\"" : "failed") +
                           " on " + Hex_(domain));
         if (succeeds && ascii != expected)
            Fail_(what, "the domain parser gave \"" + ascii + "\", expected \"" + expected + "\"");
      }
   }

   void
   IdnaTester::Test()
   {
      linesRun_ = 0;

      if (std::string(IdnaTables::UnicodeVersion) != "18.0.0")
         Fail_("the tables", "are of Unicode " + std::string(IdnaTables::UnicodeVersion) + ", and this tester of 18.0.0");

      // IdnaTestV2.txt of Unicode 18.0.0 holds 6,396 test lines. Fewer is a
      // table cut short, which would pass on what it left out.
      if (IdnaTables::TestLineCount != 6396)
         Fail_("IdnaTestV2.txt", "holds " + std::to_string(IdnaTables::TestLineCount) + " test lines, and 6396 were generated");

      const Idna::Options conformance = Idna::ConformanceOptions();
      Idna::Options transitional = conformance;
      transitional.transitionalProcessing = true;
      const Idna::Options url = Idna::UrlStandardOptions();

      // The status codes of the checks the URL standard's flags leave off, which
      // the file says to set aside when they are off (CheckHyphens: V2 and V3;
      // UseSTD3ASCIIRules: U1; VerifyDnsLength: A4_1 and A4_2), and X4_2, which
      // is ToUnicode's mark for an empty label and A4_2 to ToASCII.
      const std::vector<std::string> unicodeAside = { "X4_2" };
      const std::vector<std::string> urlAside = { "V2", "V3", "U1", "A4_1", "A4_2", "X4_2" };
      const std::vector<std::string> none;

      for (size_t line = 0; line < IdnaTables::TestLineCount; line++)
      {
         const std::string text = IdnaTables::TestLines[line];

         std::vector<std::string> columns;
         size_t start = 0;
         for (;;)
         {
            const size_t semicolon = text.find(';', start);
            columns.push_back(Trimmed_(text.substr(start, semicolon == std::string::npos ? std::string::npos : semicolon - start)));
            if (semicolon == std::string::npos)
               break;
            start = semicolon + 1;
         }

         const std::string where = Where_(line, columns[0]);
         if (columns.size() < 7)
            Fail_(where, "fewer than seven columns");

         const CodePoints source = Unescaped_(columns[0], where);
         const CodePoints toUnicode = columns[1].empty() ? source : Unescaped_(columns[1], where);
         const std::vector<std::string> toUnicodeStatus = columns[2].empty() ? none : Statuses_(columns[2], where);
         const CodePoints toAsciiN = columns[3].empty() ? toUnicode : Unescaped_(columns[3], where);
         const std::vector<std::string> toAsciiNStatus = columns[4].empty() ? toUnicodeStatus : Statuses_(columns[4], where);
         const CodePoints toAsciiT = columns[5].empty() ? toAsciiN : Unescaped_(columns[5], where);
         const std::vector<std::string> toAsciiTStatus = columns[6].empty() ? toAsciiNStatus : Statuses_(columns[6], where);

         // ToUnicode.
         CodePoints unicode;
         const unsigned int unicodeErrors = Idna::ToUnicode(source, conformance, unicode);
         if (unicode != toUnicode)
            Fail_(where, "ToUnicode gave " + Hex_(unicode) + ", expected " + Hex_(toUnicode));
         if ((unicodeErrors != 0) != HasErrorBeyond_(toUnicodeStatus, unicodeAside))
            Fail_(where, "ToUnicode's errors " + ErrorsOf_(unicodeErrors) + ", expected " + Joined_(toUnicodeStatus));

         // ToASCII, Nontransitional and Transitional.
         std::string ascii;
         const unsigned int asciiErrors = Idna::ToAscii(source, conformance, ascii);
         if (ascii != Idna::ToUtf8(toAsciiN))
            Fail_(where, "ToASCII gave " + Printable_(ascii) + ", expected " + Hex_(toAsciiN));
         if ((asciiErrors != 0) != !toAsciiNStatus.empty())
            Fail_(where, "ToASCII's errors " + ErrorsOf_(asciiErrors) + ", expected " + Joined_(toAsciiNStatus));

         std::string asciiTransitional;
         const unsigned int transitionalErrors = Idna::ToAscii(source, transitional, asciiTransitional);
         if (asciiTransitional != Idna::ToUtf8(toAsciiT))
            Fail_(where, "Transitional ToASCII gave " + Printable_(asciiTransitional) + ", expected " + Hex_(toAsciiT));
         if ((transitionalErrors != 0) != !toAsciiTStatus.empty())
            Fail_(where, "Transitional ToASCII's errors " + ErrorsOf_(transitionalErrors) + ", expected " + Joined_(toAsciiTStatus));

         // ToASCII with the URL standard's flags: the same name, and an error
         // only for what is left once the codes of the checks it leaves off are
         // set aside.
         std::string asciiUrl;
         const unsigned int urlErrors = Idna::ToAscii(source, url, asciiUrl);
         if (asciiUrl != Idna::ToUtf8(toAsciiN))
            Fail_(where, "ToASCII with the URL standard's flags gave " + Printable_(asciiUrl) + ", expected " + Hex_(toAsciiN));
         if ((urlErrors != 0) != HasErrorBeyond_(toAsciiNStatus, urlAside))
            Fail_(where, "ToASCII with the URL standard's flags: errors " + ErrorsOf_(urlErrors) + ", expected " + Joined_(toAsciiNStatus) +
                            " less V2, V3, U1, A4_1, A4_2 and X4_2");

         linesRun_++;
      }

      // RFC 3492 section 7.1's samples: (A) Arabic (Egyptian), (B) Chinese
      // (simplified), (L) a Japanese name with ASCII in it, both ways.
      ExpectPunycode_({ 0x0644, 0x064A, 0x0647, 0x0645, 0x0627, 0x0628, 0x062A, 0x0643, 0x0644, 0x0645, 0x0648, 0x0634, 0x0639,
                        0x0631, 0x0628, 0x064A, 0x061F }, "egbpdaj6bu4bxfgehfvwxn", "RFC 3492 sample (A)");
      ExpectPunycode_({ 0x4ED6, 0x4EEC, 0x4E3A, 0x4EC0, 0x4E48, 0x4E0D, 0x8BF4, 0x4E2D, 0x6587 }, "ihqwcrb4cv8a8dqg056pqjye",
                      "RFC 3492 sample (B)");
      ExpectPunycode_({ 0x0033, 0x5E74, 0x0042, 0x7D44, 0x91D1, 0x516B, 0x5148, 0x751F }, "3B-ww4c5e180e575a65lsy2b",
                      "RFC 3492 sample (L)");
      ExpectPunycode_({ 'b', 0x00FC, 'c', 'h', 'e', 'r' }, "bcher-kva", "b<u-umlaut>cher");

      CodePoints decoded;
      if (Idna::PunycodeDecode("-", decoded))
         Fail_("Punycode", "\"-\" alone was decoded; the delimiter is consumed only after basic code points");
      if (!Idna::PunycodeDecode("abc-", decoded) || decoded != CodePoints({ 'a', 'b', 'c' }))
         Fail_("Punycode", "\"abc-\" did not decode to abc");
      if (Idna::PunycodeDecode("kva!", decoded))
         Fail_("Punycode", "a character that is no digit was decoded");
      if (Idna::PunycodeDecode("99999999999", decoded))
         Fail_("Punycode", "a value past a signed 32-bit integer was decoded");
      if (Idna::PunycodeDecode("9", decoded))
         Fail_("Punycode", "a number cut short was decoded");
      if (!Idna::PunycodeDecode("a", decoded) || decoded != CodePoints({ 0x80 }))
         Fail_("Punycode", "\"a\", a whole number of value 0, did not decode to U+0080");

      // NFC: composition, Hangul by arithmetic, canonical order, a singleton,
      // and a composite excluded from composition.
      ExpectNfc_({ 'a', 0x0301 }, { 0x00E1 }, "a and COMBINING ACUTE ACCENT");
      ExpectNfc_({ 0x1100, 0x1161, 0x11A8 }, { 0xAC01 }, "Hangul L V T");
      ExpectNfc_({ 0xAC00, 0x11A8 }, { 0xAC01 }, "Hangul LV T");
      ExpectNfc_({ 'a', 0x0301, 0x0323 }, { 0x1EA1, 0x0301 }, "two marks out of canonical order");
      ExpectNfc_({ 0x212B }, { 0x00C5 }, "ANGSTROM SIGN, a singleton");
      ExpectNfc_({ 0x0958 }, { 0x0915, 0x093C }, "DEVANAGARI LETTER QA, excluded from composition");
      ExpectNfc_({ 0x00E1 }, { 0x00E1 }, "a composite already in NFC");

      // The URL standard's domain parser.
      ExpectDomain_({ 0xFF45, 0xFF58, 0xFF41, 0xFF4D, 0xFF50, 0xFF4C, 0xFF45, 0xFF0E, 0xFF43, 0xFF4F, 0xFF4D }, true, "example.com",
                    "fullwidth letters and a fullwidth full stop");
      ExpectDomain_({ 'e', 'x', 'a', 'm', 'p', 'l', 'e', 0x3002, 'c', 'o', 'm' }, true, "example.com", "an ideographic full stop");
      ExpectDomain_({ 'e', 'x', 'a', 'm', 'p', 'l', 'e', 0xFF61, 'c', 'o', 'm' }, true, "example.com", "a halfwidth ideographic full stop");
      ExpectDomain_({ 'B', 0x00FC, 'c', 'h', 'e', 'r', '.', 'E', 'x', 'a', 'm', 'p', 'l', 'e' }, true, "xn--bcher-kva.example",
                    "a capital and a u-umlaut");
      ExpectDomain_({ 'f', 'a', 0x00DF, '.', 'd', 'e' }, true, "xn--fa-hia.de", "sharp s, a deviation kept");
      ExpectDomain_({ 'e', 'x', 0x00AD, 'a', 'm', 'p', 'l', 'e', '.', 'c', 'o', 'm' }, true, "example.com", "a soft hyphen, ignored");
      ExpectDomain_({ 0x2615, '.', 'e', 'x', 'a', 'm', 'p', 'l', 'e' }, true, "xn--53h.example", "the URL standard's own example");
      ExpectDomain_({ 'X', 'N', '-', '-', 'Z', 'Z', '.', 'e', 'x', 'a', 'm', 'p', 'l', 'e' }, true, "xn--zz.example",
                    "an ASCII name with an xn-- label that is not Punycode, lowered and kept");
      ExpectDomain_({ 'x', 'n', '-', '-', 'z', 'z', '.', 'e', 'x', 0x00E4, 'm', 'p', 'l', 'e' }, false, "",
                    "the same label beside a non-ASCII one");
      ExpectDomain_({ 'e', 'x', 'a', '%', 'm', 'p', 'l', 'e' }, false, "", "a percent sign, a forbidden domain code point");
      ExpectDomain_({ 'a', 0xFF0F, 'b', '.', 'e', 'x', 'a', 'm', 'p', 'l', 'e' }, false, "",
                    "a fullwidth solidus, mapped to a forbidden '/'");
      ExpectDomain_({}, false, "", "the empty string");
      ExpectDomain_({ 0x00AD }, false, "", "a soft hyphen alone, which maps to nothing");
      ExpectDomain_({ 'a', 0x05D0, '.', 'e', 'x', 'a', 'm', 'p', 'l', 'e' }, false, "", "a label of Latin and Hebrew (RFC 5893)");
      ExpectDomain_({ 0xFFFD, '.', 'e', 'x', 'a', 'm', 'p', 'l', 'e' }, false, "", "U+FFFD, where UTF-8 was not");

      // A label longer than Options::longestLabel is left alone, at once,
      // however long: a hundred thousand distinct code points would take
      // Punycode some five billion steps.
      CodePoints longLabel;
      for (unsigned int i = 0; i < 100000; i++)
         longLabel.push_back(0x4E00 + i % 20000 + (i / 20000) * 0x10000);
      longLabel.push_back('.');
      longLabel.push_back('c');
      longLabel.push_back('n');

      Idna::Options limited = url;
      limited.longestLabel = 63;
      std::vector<Idna::Label> labels;
      Idna::ProcessLabels(longLabel, limited, labels);
      if (labels.size() != 2 || !(labels[0].errors & Idna::ErrorLabelTooLong) || !labels[0].ascii.empty() ||
          labels[1].errors != 0 || labels[1].ascii != "cn")
         Fail_("a long label", "was not left alone beside the label after it");

      // An ASCII label is processed whatever its length - it needs no Punycode
      // - so that an address a browser reads with a long run of zeros in a
      // part, beside a fullwidth full stop, stays one.
      CodePoints longAscii = { '0', 'x', '1', 0xFF0E };
      longAscii.insert(longAscii.end(), 100, '0');
      longAscii.push_back(0xFF0E);
      longAscii.push_back('1');
      Idna::ProcessLabels(longAscii, limited, labels);
      if (labels.size() != 3 || labels[1].errors != 0 || labels[1].ascii != std::string(100, '0') || labels[2].ascii != "1")
         Fail_("a long ASCII label", "was not processed as any other ASCII label is");

      // So is one written in fullwidth digits, which map to ASCII ones: however
      // long, it is mapped whole, and is an ASCII label like any other.
      CodePoints fullwidthZeros = { '0', 'x', '1', 0xFF0E };
      fullwidthZeros.insert(fullwidthZeros.end(), 300, 0xFF10);
      fullwidthZeros.push_back(0xFF0E);
      fullwidthZeros.push_back('1');
      Idna::ProcessLabels(fullwidthZeros, limited, labels);
      if (labels.size() != 3 || labels[1].errors != 0 || labels[1].ascii != std::string(300, '0') || labels[2].ascii != "1")
         Fail_("a long label of fullwidth digits", "was not mapped whole into the ASCII label it is");

      // A label that can never be short enough is not mapped whole: U+FDFA maps
      // to eighteen code points, and a megabyte of it in one label was mapped
      // into some seven million, and put in NFC, before its length was looked
      // at. Now no more of it is mapped than LongestDecomposition times
      // longestLabel and one mapping, and the labels after it are read as ever.
      CodePoints expanding(350000, 0xFDFA);
      const CodePoints evilExample = { '.', 'e', 'v', 'i', 'l', '.', 'e', 'x', 'a', 'm', 'p', 'l', 'e' };
      expanding.insert(expanding.end(), evilExample.begin(), evilExample.end());
      Idna::ProcessLabels(expanding, limited, labels);
      const size_t mappedAtMost = IdnaTables::LongestDecomposition * limited.longestLabel + 18;
      if (labels.size() != 3 || !(labels[0].errors & Idna::ErrorLabelTooLong) || !labels[0].ascii.empty() ||
          labels[0].unicode.size() > mappedAtMost || labels[0].mapped.size() > mappedAtMost ||
          labels[1].ascii != "evil" || labels[2].ascii != "example" || labels[1].errors != 0 || labels[2].errors != 0)
         Fail_("a label of 350,000 U+FDFA", "was mapped further than its length could ever allow (" +
                  std::to_string(labels.empty() ? 0 : labels[0].unicode.size()) + " code points kept), or the labels after it were not read");

      // What is left unmapped still counts towards a bidi domain name: a Hebrew
      // letter at the end of a label cut short makes "1a" - a label that begins
      // with a digit - break RFC 5893's rule, as it does when the label is
      // mapped whole.
      CodePoints rightToLeftAtTheEnd = { '1', 'a', '.' };
      rightToLeftAtTheEnd.insert(rightToLeftAtTheEnd.end(), 300, 0x00E9);
      rightToLeftAtTheEnd.push_back(0x05D0);
      Idna::ProcessLabels(rightToLeftAtTheEnd, limited, labels);
      if (labels.size() != 2 || !(labels[0].errors & Idna::ErrorBidi) || !(labels[1].errors & Idna::ErrorLabelTooLong))
         Fail_("a bidi domain name whose R is in the part of a label left unmapped",
               "did not hold the label before it to RFC 5893's rule");
      Idna::ProcessLabels(rightToLeftAtTheEnd, url, labels);
      if (labels.size() != 2 || !(labels[0].errors & Idna::ErrorBidi))
         Fail_("the same name with no limit on a label", "did not hold \"1a\" to RFC 5893's rule");

      // Each label as written beside the label as Processing leaves it: an
      // "xn--" label that decodes to ASCII alone is refused, decoded; the
      // label it was is kept, as the mapping left it (in small letters).
      Idna::ProcessLabels({ 'X', 'N', '-', '-', 'A', 'B', 'C', '-', '.', 'b', 0x00FC, 'c', 'h', 'e', 'r' }, url, labels);
      if (labels.size() != 2 || !(labels[0].errors & Idna::ErrorPunycode) || !labels[0].punycode ||
          labels[0].unicode != CodePoints({ 'a', 'b', 'c' }) || labels[0].mapped != CodePoints({ 'x', 'n', '-', '-', 'a', 'b', 'c', '-' }) ||
          labels[1].errors != 0 || labels[1].punycode || labels[1].ascii != "xn--bcher-kva" ||
          labels[1].mapped != labels[1].unicode)
         Fail_("an xn-- label that decodes to ASCII", "was not kept as written beside the label it decodes to");

      // What the mapping writes a code point as, told without writing it
      // (KindOfMapping).
      struct KindCase
      {
         unsigned int codePoint;
         Idna::MappingKind kind;
         const char *what;
      };

      const KindCase kindCases[] =
      {
         { 0x00AD, Idna::MapsToNothing, "a soft hyphen, ignored (Cf)" },
         { 0xFE0F, Idna::MapsToNothing, "a variation selector, ignored (Mn)" },
         { 0x002E, Idna::MapsToFullStop, "a full stop" },
         { 0x3002, Idna::MapsToFullStop, "an ideographic full stop" },
         { 0xFF0E, Idna::MapsToFullStop, "a fullwidth full stop" },
         { 0xFF61, Idna::MapsToFullStop, "a halfwidth ideographic full stop" },
         { 'a', Idna::MapsToNameCharacters, "a small letter" },
         { 'A', Idna::MapsToNameCharacters, "a capital, mapped to a small letter" },
         { '-', Idna::MapsToNameCharacters, "a hyphen-minus" },
         { 0xFF45, Idna::MapsToNameCharacters, "a fullwidth e" },
         { 0xFF0D, Idna::MapsToNameCharacters, "a fullwidth hyphen-minus" },
         { 0xFF3F, Idna::MapsToNameCharacters, "a fullwidth low line" },
         { 0x24D4, Idna::MapsToNameCharacters, "a circled e (So)" },
         { 0x2122, Idna::MapsToNameCharacters, "the trade mark sign, tm (So)" },
         { 0x2167, Idna::MapsToNameCharacters, "the Roman numeral eight, viii (Nl)" },
         { '!', Idna::MapsToOtherAscii, "an exclamation mark" },
         { 0xFF0F, Idna::MapsToOtherAscii, "a fullwidth solidus, a '/'" },
         { 0x00A0, Idna::MapsToOtherAscii, "a no-break space, a space (Zs)" },
         { 0x2100, Idna::MapsToOtherAscii, "the account-of sign, a/c" },
         { 0x00FC, Idna::MapsPastAscii, "a u with diaeresis" },
         { 0xFDFA, Idna::MapsPastAscii, "an Arabic ligature mapped to eighteen code points (Lo)" },
         { 0x201D, Idna::MapsPastAscii, "a right double quotation mark, valid (Pf)" },
         { 0x3001, Idna::MapsPastAscii, "an ideographic comma (Po)" },
         { 0x300D, Idna::MapsPastAscii, "a right corner bracket (Pe)" },
         { 0x2011, Idna::MapsPastAscii, "a non-breaking hyphen, mapped to U+2010 (Pd)" },
         { 0x200D, Idna::MapsPastAscii, "a zero-width joiner, a deviation kept (Cf)" },
         { 0x2665, Idna::MapsPastAscii, "a black heart suit, valid (So)" },
         { 0x0378, Idna::MapsPastAscii, "an unassigned code point, disallowed" },
         { 0xE000, Idna::MapsPastAscii, "a private-use code point (Co)" },
         { 0xD800, Idna::MapsPastAscii, "a surrogate (Cs)" },
         { 0x0301, Idna::MapsPastAscii, "a combining acute accent (Mn)" },
         { 0x0661, Idna::MapsPastAscii, "an Arabic-Indic digit one (Nd)" },
         { 0x110000, Idna::MapsPastAscii, "past Unicode's last code point" },
      };

      for (const KindCase &test : kindCases)
      {
         if (Idna::KindOfMapping(test.codePoint) != test.kind)
            Fail_(test.what, "KindOfMapping gave " + std::to_string(static_cast<int>(Idna::KindOfMapping(test.codePoint))) +
                     ", expected " + std::to_string(static_cast<int>(test.kind)));
      }

      // KindOfMapping says what ProcessLabels' mapping writes, at both ends of
      // every range of IdnaMappingTable.txt the tables hold: nothing exactly
      // when it deletes the code point, a full stop exactly when it ends a
      // label there, and past ASCII exactly when it writes a code point past
      // ASCII, between two letters, once in NFC.
      for (size_t range = 0; range < IdnaTables::MappingRangeCount; range++)
      {
         const unsigned int ends[2] = { IdnaTables::MappingRanges[range].first, IdnaTables::MappingRanges[range].last };
         for (unsigned int codePoint : ends)
         {
            const Idna::MappingKind kind = Idna::KindOfMapping(codePoint);
            Idna::ProcessLabels({ 'a', codePoint, 'b' }, url, labels);

            bool pastAscii = false;
            for (const Idna::Label &label : labels)
               for (unsigned int written : label.mapped)
                  pastAscii = pastAscii || written >= 0x80;

            const bool deleted = labels.size() == 1 && labels[0].mapped == CodePoints({ 'a', 'b' });
            if ((kind == Idna::MapsToNothing) != deleted || (kind == Idna::MapsToFullStop) != (labels.size() == 2) ||
                (kind == Idna::MapsPastAscii) != pastAscii)
               Fail_(Hex_({ codePoint }), "KindOfMapping gave " + std::to_string(static_cast<int>(kind)) +
                        ", and ProcessLabels wrote it otherwise");
         }
      }
   }
}
