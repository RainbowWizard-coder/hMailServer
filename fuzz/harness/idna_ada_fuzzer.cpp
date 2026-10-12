// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// A differential fuzz target: the server's reading of a host (Common/Idna,
// UTS 46, and SURBL's reading of a link) against ada, the WHATWG URL parser of
// Node.js, which a browser's reading is held to. Built and run by
// build/ci/idna-differential-fuzz.sh, on Linux with clang's libFuzzer and
// AddressSanitizer (hmailserver/docs/Fuzzing.md, "UTS 46 against ada").
//
// The input is a string in UTF-8; one that is not well-formed UTF-8 is set
// aside, as is one that holds a code point whose UTS 46 data differ between
// the Unicode version of ada's tables and the server's
// (IdnaVersionDifferences, written by build/generate-idna-tables.py
// --differences): those are differences of the two Unicode versions, not of the
// two programs. So is a difference that is one of ada's four departures from
// UTS 46 (idna_ada_common.h), which idna_ada_conformance.cpp shows on Unicode's
// own conformance tests, or holds the server's NFC to, before a run starts.
// Three things are asked of every other input, and the first that does not
// hold ends the run with both answers:
//
//   1. The domain parser. HM::Idna::DomainToAscii and ada's - its to_ascii and
//      the URL standard's two checks after it - both fail, or give one name.
//   2. The host of a link's authority. Where the input holds nothing that ends
//      an authority or that the URL parser strips (C0 controls, space, DELETE,
//      and @ : / \ ? # [ ]), SURBL::HostOfAuthority of it is what
//      HostOfAuthority makes of the host ada's parser gives for
//      http://<input>/, and empty when ada fails it.
//   3. The link reader misses no host a browser reaches. When that host is a
//      name or an address a list can be asked about, SURBL::FindLinkHosts
//      hands it over for the text "see http://<input>/ now" - unless the input
//      holds white space or an angle bracket, where a mail program ends a link
//      in running text before any browser is given it - and for the HTML
//      <a href="http://<input>/">, unless it holds the '"' that ends the value
//      or the '&' that begins a character reference.

#include "idna_ada_common.h"
#include "Common/AntiSpam/SURBL.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{
   HM::String Wide_(const std::vector<unsigned int> &codePoints)
   {
      std::wstring wide;
      for (unsigned int codePoint : codePoints)
         wide += static_cast<wchar_t>(codePoint);
      return HM::String(wide.c_str());
   }

   HM::String Wide_(const std::string &ascii)
   {
      std::wstring wide;
      for (char character : ascii)
         wide += static_cast<wchar_t>(static_cast<unsigned char>(character));
      return HM::String(wide.c_str());
   }

   std::string Printable_(const std::wstring &text)
   {
      std::string printable;
      char buffer[16];
      for (wchar_t character : text)
      {
         const unsigned int codePoint = static_cast<unsigned int>(character);
         if (codePoint >= 0x20 && codePoint < 0x7F)
         {
            printable += static_cast<char>(codePoint);
            continue;
         }
         snprintf(buffer, sizeof(buffer), "<U+%04X>", codePoint);
         printable += buffer;
      }
      return printable;
   }

   std::string Printable_(const std::vector<unsigned int> &codePoints)
   {
      std::wstring wide;
      for (unsigned int codePoint : codePoints)
         wide += static_cast<wchar_t>(codePoint);
      return Printable_(wide);
   }

   [[noreturn]] void Differs_(const char *what, const std::vector<unsigned int> &input, const std::string &ours, const std::string &theirs)
   {
      fprintf(stderr, "\nDIFFERENCE (%s)\n  input: %s\n  hMailServer: %s\n  ada %s: %s\n", what, Printable_(input).c_str(),
              ours.c_str(), AdaVersion(), theirs.c_str());
      abort();
   }
}

extern "C" int LLVMFuzzerInitialize(int * /*argc*/, char *** /*argv*/)
{
   // The code points whose direction ada reads otherwise, found once, before
   // the first input, and said, so that a run's log tells what it set aside.
   fprintf(stderr, "ada %s reads the direction, or whether it is a mark, of %zu code points otherwise than the server's data (U+10D48: %s)\n",
           AdaVersion(), idna_ada::CountAdaReadsDirectionOtherwise(),
           idna_ada::AdaReadsDirectionOtherwise(0x10D48) ? "otherwise" : "alike");
   return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
   // ada's to_ascii refuses a domain past 16 KB; a host longer than a DNS
   // name many times over says nothing more.
   if (size == 0 || size > 1024)
      return 0;

   std::vector<unsigned int> codePoints;
   if (!idna_ada::DecodeUtf8(data, size, codePoints))
      return 0;

   const std::string utf8(reinterpret_cast<const char *>(data), size);

   // 1. The domain parser.
   std::string ours;
   std::string theirs;
   const bool oursSucceeded = HM::Idna::DomainToAscii(codePoints, ours);
   const bool theirsSucceeded = AdaDomainParser(utf8, theirs);
   if (oursSucceeded != theirsSucceeded || (oursSucceeded && ours != theirs))
   {
      if (idna_ada::DependsOnTablesThatDiffer(codePoints) || idna_ada::AdaDeviation(codePoints) != nullptr ||
          (theirsSucceeded && idna_ada::AdaAnswerNotNfc(theirs)))
         return 0;
      Differs_("the domain parser", codePoints, oursSucceeded ? "\"" + ours + "\"" : "failure",
               theirsSucceeded ? "\"" + theirs + "\"" : "failure");
   }

   // 2. The host of a link's authority, which both percent-decode first: what
   // is set aside above is set aside for the host it decodes to as well.
   for (unsigned char character : utf8)
      if (character <= 0x20 || character == 0x7F || strchr("@:/\\?#[]", character) != nullptr)
         return 0;

   std::string adaHost;
   const bool adaReached = AdaHost(utf8, adaHost);

   // Whether a difference over the host is one set aside: of the tables, or
   // one of ada's departures.
   auto setAside = [&]() -> bool
   {
      const std::string decoded = idna_ada::PercentDecoded(utf8);
      std::vector<unsigned int> decodedCodePoints;
      if (idna_ada::DecodeUtf8(reinterpret_cast<const uint8_t *>(decoded.data()), decoded.size(), decodedCodePoints) &&
          (idna_ada::DependsOnTablesThatDiffer(decodedCodePoints) || idna_ada::AdaDeviation(decodedCodePoints) != nullptr))
         return true;
      return adaReached && idna_ada::AdaAnswerNotNfc(adaHost);
   };

   const HM::String expected = adaReached ? HM::SURBL::HostOfAuthority(Wide_(adaHost)) : HM::String();
   const HM::String got = HM::SURBL::HostOfAuthority(Wide_(codePoints));
   if (got != expected)
   {
      if (setAside())
         return 0;
      Differs_("the host of a link's authority", codePoints, "\"" + Printable_(got) + "\"",
               adaReached ? "\"" + adaHost + "\", read as \"" + Printable_(expected) + "\"" : "failure");
   }

   // 3 and 4. The link reader hands over the host a browser reaches: in running
   // text, where white space and the angle brackets end a link, as a mail
   // program ends it before a browser is given it (an input holding one is
   // left out), and in an HTML attribute's value, where they do not (an input
   // holding a '"', which ends the value, or a '&', which starts a character
   // reference, is left out).
   if (expected.IsEmpty())
      return 0;

   bool endsTextLink = false;
   bool endsAttribute = false;
   for (unsigned int codePoint : codePoints)
   {
      if (codePoint == '<' || codePoint == '>' || codePoint == 0xA0 || codePoint == 0x1680 || (codePoint >= 0x2000 && codePoint <= 0x200A) ||
          codePoint == 0x2028 || codePoint == 0x2029 || codePoint == 0x202F || codePoint == 0x205F || codePoint == 0x3000)
         endsTextLink = true;
      if (codePoint == '"' || codePoint == '&')
         endsAttribute = true;
   }

   auto expectHandedOver = [&](const char *what, const char *before, const char *after, bool html)
   {
      bool handedOver = false;
      std::string all;
      HM::String text = Wide_(std::string(before));
      text += Wide_(codePoints);
      text += Wide_(std::string(after));
      HM::SURBL::FindLinkHosts(text, html, [&](const HM::String &host) -> bool
      {
         all += (all.empty() ? "" : "|") + Printable_(host);
         if (host == expected)
            handedOver = true;
         return true;
      });

      if (!handedOver && !setAside())
         Differs_(what, codePoints, all.empty() ? "nothing" : all, "\"" + Printable_(expected) + "\"");
   };

   if (!endsTextLink)
      expectHandedOver("the link reader missed the host a browser reaches, in running text", "see http://", "/ now", false);
   if (!endsAttribute)
      expectHandedOver("the link reader missed the host a browser reaches, in an HTML link", "<a href=\"http://", "/\">a link</a>", true);

   return 0;
}
