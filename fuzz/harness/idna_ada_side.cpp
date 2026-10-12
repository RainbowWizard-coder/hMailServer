// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The oracle's half of the differential fuzz of UTS 46 (idna_ada_fuzzer.cpp):
// what ada, the WHATWG URL parser of Node.js, makes of a domain and of a link's
// authority, and how it reads a code point's direction and marks. Compiled
// against ada alone - ada.h and ada.cpp, the amalgamation of ada 4.0.0 that
// build/ci/idna-differential-fuzz.sh takes from PyPI - and without the
// server's forced precompiled header, which is why it is a translation unit of
// its own.

#include "ada.h"

#include <string>

// The URL standard's domain parser with beStrict false: ada's to_ascii, which is
// the standard's ToASCII with an ASCII domain lowered whatever ToASCII would
// say, and then what the standard adds - the empty string and a forbidden
// domain code point are failures.
bool AdaDomainParser(const std::string &utf8, std::string &ascii)
{
   ascii.clear();
   if (utf8.empty())
      return false;

   if (!ada::idna::to_ascii(utf8, ascii))
      return false;

   if (ascii.empty() || ada::idna::contains_forbidden_domain_code_point(ascii))
      return false;

   return true;
}

// The host a browser goes to for http://<authority>/, as ada's URL parser reads
// it. False where the parser fails the URL.
bool AdaHost(const std::string &authority, std::string &host)
{
   host.clear();

   auto url = ada::parse<ada::url_aggregator>("http://" + authority + "/");
   if (!url)
      return false;

   host = std::string(url->get_hostname());
   return true;
}

const char *AdaVersion()
{
   return ADA_VERSION;
}

// How ada's own label check reads a code point's direction and whether it is a
// mark, asked of six labels, one bit each (idna_ada_common.h, ExpectedProbes):
//   1  'a' and it is refused: ada takes it for R, AL or AN
//   2  ALEF, it, ALEF is taken: it may stand inside a right-to-left label
//   4  ALEF and it is taken: it may end one (R, AL, EN, AN, or an NSM after)
//   8  it and ALEF is taken: it may begin one (R or AL, and not a mark)
//  16  ALEF, it, ARABIC-INDIC DIGIT ONE, ALEF is taken: inside, and not EN
//  32  it alone is taken: no mark and not NSM, or R or AL
bool AdaLabelValid(const std::u32string &label)
{
   return ada::idna::is_label_valid(label);
}

unsigned int AdaDirectionProbes(unsigned int codePoint)
{
   const char32_t c = static_cast<char32_t>(codePoint);
   const char32_t alef = 0x05D0;
   const char32_t arabicIndicOne = 0x0661;
   unsigned int bits = 0;
   if (!AdaLabelValid(std::u32string({ U'a', c })))
      bits |= 1;
   if (AdaLabelValid(std::u32string({ alef, c, alef })))
      bits |= 2;
   if (AdaLabelValid(std::u32string({ alef, c })))
      bits |= 4;
   if (AdaLabelValid(std::u32string({ c, alef })))
      bits |= 8;
   if (AdaLabelValid(std::u32string({ alef, c, arabicIndicOne, alef })))
      bits |= 16;
   if (AdaLabelValid(std::u32string(1, c)))
      bits |= 32;
   return bits;
}
