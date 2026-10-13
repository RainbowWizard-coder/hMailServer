// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// What the differential fuzz of UTS 46 against ada (idna_ada_fuzzer.cpp) and
// its check of ada against Unicode's conformance tests (idna_ada_conformance.cpp)
// share: the oracle's three answers, UTF-8, the code points whose data differ
// between the two Unicode versions, and the four places where ada 4.0.0 is not
// UTS 46 - a difference there is ada's. The conformance check shows the first two
// line by line on IdnaTestV2.txt, the file the server's own UTS 46 passes whole,
// and holds the server's NFC, which the third is judged by, to
// NormalizationTest.txt; the fourth is RFC 3492's own text and sample code.
//
//   * CONTEXTJ. RFC 5892's rule for ZERO WIDTH NON-JOINER asks for a character
//     of Joining_Type L or D before it and R or D after it with only Joining_Type
//     T between; ada looks for any such character anywhere before and after,
//     from lists of its own, and once it has taken a joiner checks nothing more
//     of the label, its bidi rule included. A domain that holds U+200C or
//     U+200D, as written or in a label UTS 46 decodes from Punycode, is set
//     aside.
//   * RFC 5893's rule, which UTS 46 applies to every label of a bidi domain name
//     (one that holds an R, AL or AN anywhere) and ada only to a label that holds
//     one itself. A difference where the server refused and every label it
//     refused has no R, AL or AN in it, and was refused for that rule alone, is
//     set aside.
//   * NFC. ada skips normalising a name its quick check passes, and the check
//     passes a precomposed character followed by a combining mark that its own
//     decomposition must be reordered with, so ada's answer is then not in NFC.
//     A difference where ada's answer has a label that is not is set aside
//     (AdaAnswerNotNfc).
//   * Punycode. A label written "xn--" whose Punycode begins with its only
//     delimiter ("xn---ssm4ef") is no Punycode by RFC 3492, which consumes the
//     delimiter only after basic code points; ada skips it. A difference where
//     the server refused such a label for that alone is set aside.

#pragma once

#include "Common/Idna/Idna.h"
#include "Common/Idna/IdnaTables.h"

// build/generate-idna-tables.py --differences: the code points whose UTS 46 data
// differ between the Unicode version of ada's tables and the server's.
#include "idna_version_differences.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// idna_ada_side.cpp, compiled against ada alone.
bool AdaDomainParser(const std::string &utf8, std::string &ascii);
bool AdaHost(const std::string &authority, std::string &host);
unsigned int AdaDirectionProbes(unsigned int codePoint);
const char *AdaVersion();

namespace idna_ada
{
   // Well-formed UTF-8 only: no overlong form, no surrogate, nothing past
   // U+10FFFF, no sequence cut short.
   inline bool DecodeUtf8(const uint8_t *data, size_t size, std::vector<unsigned int> &codePoints)
   {
      codePoints.clear();
      size_t i = 0;
      while (i < size)
      {
         const unsigned int lead = data[i];
         unsigned int length = 0;
         unsigned int codePoint = 0;
         unsigned int least = 0;

         if (lead < 0x80)
         {
            codePoints.push_back(lead);
            i++;
            continue;
         }

         if (lead >= 0xC2 && lead <= 0xDF)
         {
            length = 2;
            codePoint = lead & 0x1F;
            least = 0x80;
         }
         else if (lead >= 0xE0 && lead <= 0xEF)
         {
            length = 3;
            codePoint = lead & 0x0F;
            least = 0x800;
         }
         else if (lead >= 0xF0 && lead <= 0xF4)
         {
            length = 4;
            codePoint = lead & 0x07;
            least = 0x10000;
         }
         else
         {
            return false;
         }

         if (i + length > size)
            return false;

         for (unsigned int j = 1; j < length; j++)
         {
            if ((data[i + j] & 0xC0) != 0x80)
               return false;
            codePoint = (codePoint << 6) | (data[i + j] & 0x3F);
         }

         if (codePoint < least || codePoint > 0x10FFFF || (codePoint >= 0xD800 && codePoint <= 0xDFFF))
            return false;

         codePoints.push_back(codePoint);
         i += length;
      }

      return true;
   }

   // The bytes a host part holds once its %XX are decoded, as the URL
   // standard's host parser decodes them; a '%' that starts no %XX is itself.
   inline std::string PercentDecoded(const std::string &text)
   {
      auto hex = [](char c) -> int
      {
         if (c >= '0' && c <= '9')
            return c - '0';
         if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
         if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
         return -1;
      };

      std::string decoded;
      for (size_t i = 0; i < text.size(); i++)
      {
         if (text[i] == '%' && i + 2 < text.size() && hex(text[i + 1]) >= 0 && hex(text[i + 2]) >= 0)
         {
            decoded += static_cast<char>(hex(text[i + 1]) * 16 + hex(text[i + 2]));
            i += 2;
            continue;
         }
         decoded += text[i];
      }
      return decoded;
   }

   // Whether a code point's UTS 46 data differ between the two Unicode
   // versions: an input holding one shows a difference of the versions, not of
   // the two programs.
   inline bool DiffersBetweenVersions(unsigned int codePoint)
   {
      const size_t count = sizeof(IdnaVersionDifferences) / sizeof(IdnaVersionDifferences[0]);
      size_t low = 0;
      size_t high = count;
      while (low < high)
      {
         const size_t middle = low + (high - low) / 2;
         if (IdnaVersionDifferences[middle][1] < codePoint)
            low = middle + 1;
         else if (IdnaVersionDifferences[middle][0] > codePoint)
            high = middle;
         else
            return true;
      }
      return false;
   }

   inline unsigned int BidiClassOf(unsigned int codePoint);

   inline bool IsMarkOf(unsigned int codePoint)
   {
      size_t low = 0;
      size_t high = HM::IdnaTables::MarkCount;
      while (low < high)
      {
         const size_t middle = low + (high - low) / 2;
         if (HM::IdnaTables::Marks[middle].last < codePoint)
            low = middle + 1;
         else if (HM::IdnaTables::Marks[middle].first > codePoint)
            high = middle;
         else
            return true;
      }
      return false;
   }

   // What ada's six probes of a code point (idna_ada_side.cpp,
   // AdaDirectionProbes) answer when its direction and whether it is a mark
   // are the server's: RFC 5893's rule and the leading-mark rule as ada
   // applies them to those labels.
   inline unsigned int ExpectedProbes(unsigned int codePoint)
   {
      using namespace HM::IdnaTables;
      const unsigned int b = BidiClassOf(codePoint);
      const bool rightToLeft = b == BidiR || b == BidiAL || b == BidiAN;
      const bool inside = rightToLeft || b == BidiEN || b == BidiES || b == BidiCS || b == BidiET || b == BidiON ||
                          b == BidiBN || b == BidiNSM;
      const bool end = b == BidiR || b == BidiAL || b == BidiEN || b == BidiAN || b == BidiNSM;
      const bool mark = IsMarkOf(codePoint);

      unsigned int bits = 0;
      if (rightToLeft)
         bits |= 1;
      if (inside)
         bits |= 2;
      if (end)
         bits |= 4;
      if ((b == BidiR || b == BidiAL) && !mark)
         bits |= 8;
      if (inside && b != BidiEN)
         bits |= 16;
      if (!mark && b != BidiNSM && (!rightToLeft || b == BidiR || b == BidiAL))
         bits |= 32;
      return bits;
   }

   // Whether ada reads a code point's direction, or whether it is a mark,
   // otherwise than the server's data. ada's direction and mark tables are
   // older than the IDNA mapping table it has from Unicode 17.0.0 - it reads
   // GARAY DIGIT EIGHT (U+10D48, Unicode 16.0, AN) as left-to-right - so the
   // code points are found by asking ada itself, once, for every one.
   inline bool AdaReadsDirectionOtherwise(unsigned int codePoint)
   {
      static const std::vector<bool> otherwise = []
      {
         std::vector<bool> table(0x110000, false);
         for (unsigned int c = 0; c < 0x110000; c++)
         {
            if ((c >= 0xD800 && c <= 0xDFFF) || c == 0x200C || c == 0x200D)
               continue;
            table[c] = ExpectedProbes(c) != AdaDirectionProbes(c);
         }
         return table;
      }();
      return codePoint < otherwise.size() && otherwise[codePoint];
   }

   inline size_t CountAdaReadsDirectionOtherwise()
   {
      size_t count = 0;
      for (unsigned int c = 0; c < 0x110000; c++)
         if (AdaReadsDirectionOtherwise(c))
            count++;
      return count;
   }

   // Whether a domain depends on data in which ada's tables and the server's
   // differ: a code point of it, or of a label as UTS 46 processes it - an
   // "xn--" label's decoded among them - whose UTS 46 data differ between the
   // two Unicode versions, or whose direction or mark ada reads otherwise. A
   // difference over such a domain is one of the tables, not of the programs.
   inline bool DependsOnTablesThatDiffer(const std::vector<unsigned int> &domain)
   {
      for (unsigned int codePoint : domain)
         if (DiffersBetweenVersions(codePoint) || AdaReadsDirectionOtherwise(codePoint))
            return true;

      std::vector<HM::Idna::Label> labels;
      HM::Idna::ProcessLabels(domain, HM::Idna::UrlStandardOptions(), labels);
      for (const HM::Idna::Label &label : labels)
         for (unsigned int codePoint : label.unicode)
            if (DiffersBetweenVersions(codePoint) || AdaReadsDirectionOtherwise(codePoint))
               return true;

      return false;
   }

   inline unsigned int BidiClassOf(unsigned int codePoint)
   {
      size_t low = 0;
      size_t high = HM::IdnaTables::BidiClassCount;
      while (low < high)
      {
         const size_t middle = low + (high - low) / 2;
         if (HM::IdnaTables::BidiClasses[middle].last < codePoint)
            low = middle + 1;
         else if (HM::IdnaTables::BidiClasses[middle].first > codePoint)
            high = middle;
         else
            return HM::IdnaTables::BidiClasses[middle].value;
      }
      return HM::IdnaTables::BidiL;
   }

   // Whether ada's answer, an ASCII name, has a label whose Punycode decodes to
   // text that is not in NFC - which UTS 46's Processing puts every name in
   // (step 2), and ada leaves a name in when its quick check passes it: a
   // precomposed character followed by a combining mark its own decomposition
   // must be reordered with (U+00E5 U+0323, whose NFC is U+1EA1 U+030A). The
   // server's NFC, which this asks, is held to NormalizationTest.txt by
   // idna_ada_conformance.cpp first.
   inline bool AdaAnswerNotNfc(const std::string &ascii)
   {
      size_t start = 0;
      while (start <= ascii.size())
      {
         size_t dot = ascii.find('.', start);
         if (dot == std::string::npos)
            dot = ascii.size();
         const std::string label = ascii.substr(start, dot - start);
         if (label.size() > 4 && (label[0] == 'x' || label[0] == 'X') && (label[1] == 'n' || label[1] == 'N') &&
             label[2] == '-' && label[3] == '-')
         {
            std::vector<unsigned int> decoded;
            if (HM::Idna::PunycodeDecode(label.substr(4), decoded) && !HM::Idna::IsNfc(decoded))
               return true;
         }
         start = dot + 1;
      }
      return false;
   }

   // The reasons AdaDeviation gives, by which idna_ada_conformance.cpp knows
   // which status code of IdnaTestV2.txt a line it sets aside must fail by.
   const char *const joinerDeparture =
      "a joiner, where ada's CONTEXTJ is not RFC 5892's and it checks nothing more of the label";
   const char *const bidiDeparture =
      "RFC 5893's rule on a label with no R, AL or AN of a bidi domain name, which ada does not apply";
   const char *const punycodeDeparture =
      "an xn-- label whose Punycode begins with its only delimiter, which RFC 3492 reads as a digit and ada skips";

   // Whether the label was written "xn--" and its Punycode's last delimiter is
   // its first character: RFC 3492 (section 6.2, and its sample code) consumes
   // the delimiter only after basic code points, so the '-' is then read as a
   // digit, which it is not, and the label is no Punycode - as ICU and the Rust
   // idna crate read it; ada (and Python's codec) skip it and decode the rest.
   inline bool PunycodeBeginsWithItsDelimiter(const HM::Idna::Label &label)
   {
      const std::vector<unsigned int> &written = label.mapped;
      if (!label.punycode || written.size() < 6 || written[4] != '-')
         return false;
      for (size_t i = 5; i < written.size(); i++)
         if (written[i] == '-')
            return false;
      return true;
   }

   // Why a difference over this domain (percent-decoded, in code points) is
   // ada's and not the server's, or nullptr when it is not one of ada's
   // departures above: the server refused the domain, and every label it
   // refused, it refused for one of them alone.
   inline const char *AdaDeviation(const std::vector<unsigned int> &domain)
   {
      for (unsigned int codePoint : domain)
         if (codePoint == 0x200C || codePoint == 0x200D)
            return joinerDeparture;

      std::vector<HM::Idna::Label> labels;
      HM::Idna::ProcessLabels(domain, HM::Idna::UrlStandardOptions(), labels);

      // A joiner in a label written "xn--" is checked by ada's CONTEXTJ as one
      // written so is, once ada has decoded the label: xn--10cl1a0b660pa0b660p
      // is Sinhala with two ZERO WIDTH JOINERs, the second after the first and
      // not after a virama, which RFC 5892 refuses and ada, having taken the
      // first, takes.
      for (const HM::Idna::Label &label : labels)
         if (label.punycode)
            for (unsigned int codePoint : label.unicode)
               if (codePoint == 0x200C || codePoint == 0x200D)
                  return joinerDeparture;

      const char *why = nullptr;
      for (const HM::Idna::Label &label : labels)
      {
         if (label.errors == 0)
            continue;

         if (label.errors == HM::Idna::ErrorPunycode && PunycodeBeginsWithItsDelimiter(label))
         {
            why = punycodeDeparture;
            continue;
         }

         if (label.errors != HM::Idna::ErrorBidi)
            return nullptr;
         for (unsigned int codePoint : label.unicode)
         {
            const unsigned int bidiClass = BidiClassOf(codePoint);
            if (bidiClass == HM::IdnaTables::BidiR || bidiClass == HM::IdnaTables::BidiAL || bidiClass == HM::IdnaTables::BidiAN)
               return nullptr;
         }
         if (why == nullptr)
            why = bidiDeparture;
      }

      return why;
   }
}
