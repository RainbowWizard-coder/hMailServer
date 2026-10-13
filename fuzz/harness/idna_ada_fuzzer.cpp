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
// Five things are asked of every other input, and the first that does not
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
//   4. And for the same HTML with the input written in character references,
//      which a browser reads before its URL parser: '&' and '"' as &amp; and
//      &quot;, and each code point past ASCII that HTML has a named reference
//      for as one of its names, picked by its place in the input - with its
//      ';', or, for one HTML reads without it, without where what follows is
//      no letter, digit, '=' or ';' - so that no input is left out for an '&'
//      or a '"', and HTML's whole list of names is read.
//   5. The link reader hands over every host 6.3.6 handed over for the input
//      as a link's host part, whatever a browser makes of it - the name before
//      a character a mail program ends a link at, and the labels after one -
//      by a copy of 6.3.6's reading (surbl_636_reading.h), in the text and in
//      both HTML links of 3 and 4, wherever each is asked.

#include "idna_ada_common.h"
#include "surbl_636_reading.h"
#include "Common/AntiSpam/SURBL.h"

// build/ci/idna-differential-fuzz.sh writes it beside idna_version_differences.h:
// HTML's named references from Python's own copy of the standard's list, held to
// the list the server's table is generated from, so that check 4 writes a link in
// references from a list that is not the server's table.
#include "html_named_references.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
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

   bool IsAsciiAlphanumeric_(unsigned int codePoint)
   {
      return (codePoint >= 'a' && codePoint <= 'z') || (codePoint >= 'A' && codePoint <= 'Z') || (codePoint >= '0' && codePoint <= '9');
   }

   // The input as an HTML attribute's value written in character references
   // (check 4): '&' and '"' as &amp; and &quot;, and each code point past ASCII
   // that HTML has a named reference of one code point for as one of its
   // names, picked by the code point's place in the input; a name HTML reads
   // without its ';' is written so only where what follows in the value is no
   // letter, digit, '=' or ';', and with it elsewhere. The names are the
   // harness's own (html_named_references.h), not the server's table.
   std::wstring WithReferences_(const std::vector<unsigned int> &codePoints)
   {
      static const std::map<unsigned int, std::vector<std::string> > names = []
      {
         std::map<unsigned int, std::vector<std::string> > table;
         for (const HtmlNamedReferenceOracle &reference : HtmlNamedReferencesOracle)
            table[reference.codePoint].push_back(reference.name);
         return table;
      }();

      auto written = [&](unsigned int codePoint) -> bool
      {
         return codePoint == '&' || codePoint == '"' || names.find(codePoint) != names.end();
      };

      std::wstring text;
      for (size_t i = 0; i < codePoints.size(); i++)
      {
         const unsigned int codePoint = codePoints[i];
         if (codePoint == '&')
         {
            text += L"&amp;";
            continue;
         }
         if (codePoint == '"')
         {
            text += L"&quot;";
            continue;
         }

         const std::map<unsigned int, std::vector<std::string> >::const_iterator found = names.find(codePoint);
         if (found == names.end())
         {
            text += static_cast<wchar_t>(codePoint);
            continue;
         }

         std::string name = found->second[i % found->second.size()];
         if (name.back() != ';')
         {
            // What the value holds next: a reference's '&', the code point, or
            // the slash the link goes on with.
            const unsigned int next = i + 1 < codePoints.size() ? codePoints[i + 1] : '/';
            const unsigned int nextWritten = written(next) ? '&' : next;
            if (IsAsciiAlphanumeric_(nextWritten) || nextWritten == '=' || nextWritten == ';')
               name += ';';
         }

         text += L'&';
         for (char character : name)
            text += static_cast<wchar_t>(static_cast<unsigned char>(character));
      }
      return text;
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

   // 3, 4 and 5. The link reader hands over the host a browser reaches, and
   // every host 6.3.6 handed over: in running text, where white space and the
   // angle brackets end a link, as a mail program ends it before a browser is
   // given it (an input holding one is left out); in an HTML attribute's
   // value, where they do not (an input holding a '"', which ends the value, or
   // a '&', which starts a character reference, is left out); and in the same
   // value written in character references, which leaves no input out.
   const std::vector<std::wstring> released = surbl636::HostsOfHostPart(codePoints);
   if (expected.IsEmpty() && released.empty())
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

   auto expectHandedOver = [&](const char *what, const char *whatReleased, const std::string &before, const std::wstring &link,
                               const char *after, bool html)
   {
      std::vector<HM::String> hosts;
      std::string all;
      HM::String text = Wide_(before);
      text += HM::String(link.c_str());
      text += Wide_(std::string(after));
      HM::SURBL::FindLinkHosts(text, html, [&](const HM::String &host) -> bool
      {
         all += (all.empty() ? "" : "|") + Printable_(host);
         hosts.push_back(host);
         return true;
      });

      auto handedOver = [&](const HM::String &host) -> bool
      {
         for (const HM::String &found : hosts)
            if (found == host)
               return true;
         return false;
      };

      if (!expected.IsEmpty() && !handedOver(expected) && !setAside())
         Differs_(what, codePoints, all.empty() ? "nothing" : all, "\"" + Printable_(expected) + "\"");

      for (const std::wstring &host : released)
      {
         if (!handedOver(HM::String(host.c_str())))
         {
            fprintf(stderr, "\nDIFFERENCE (%s)\n  input: %s\n  hMailServer: %s\n  6.3.6 (surbl_636_reading.h) handed over: \"%s\"\n",
                    whatReleased, Printable_(codePoints).c_str(), all.empty() ? "nothing" : all.c_str(), Printable_(host).c_str());
            abort();
         }
      }
   };

   std::wstring raw;
   for (unsigned int codePoint : codePoints)
      raw += static_cast<wchar_t>(codePoint);

   if (!endsTextLink)
      expectHandedOver("the link reader missed the host a browser reaches, in running text",
                       "the link reader missed a host 6.3.6 handed over, in running text", "see http://", raw, "/ now", false);
   if (!endsAttribute)
      expectHandedOver("the link reader missed the host a browser reaches, in an HTML link",
                       "the link reader missed a host 6.3.6 handed over, in an HTML link", "<a href=\"http://", raw, "/\">a link</a>", true);
   expectHandedOver("the link reader missed the host a browser reaches, in an HTML link written in character references",
                    "the link reader missed a host 6.3.6 handed over, in an HTML link written in character references",
                    "<a href=\"http://", WithReferences_(codePoints), "/\">a link</a>", true);

   return 0;
}
