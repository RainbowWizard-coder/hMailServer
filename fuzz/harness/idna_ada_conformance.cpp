// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The oracle checked before it is trusted: ada's domain parser against the
// server's (HM::Idna::DomainToAscii, which passes every line of Unicode's
// IdnaTestV2.txt in IdnaTester) over the source column of every line of that
// file. Where the two differ, the line must hold a code point whose UTS 46 data
// differ between the Unicode version of ada's tables and the server's, or the
// difference must be one of ada's four departures from UTS 46
// (idna_ada_common.h): an answer not in NFC, or, on a line the file itself
// fails, RFC 5893's rule left out (the line failing by a B status code), a
// joiner (by any code, since ada checks nothing more of a label once it has
// taken a joiner in it - not its bidi rule either), or Punycode that begins
// with its delimiter (by a P code). Those are what the differential fuzz sets
// aside, so it never sets aside more than this shows ada gets wrong. Any other
// difference fails.
//
// Before that, the server's NFC is held to Unicode's NormalizationTest.txt,
// whose path is the one argument: every line of it, and every code point it
// does not list normalising to itself. A difference where ada's answer is not
// in NFC is set aside as ada's (its third departure), and that is judged by
// the server's NFC, so it is held to the file first.
//
// Run by build/ci/idna-differential-fuzz.sh before it fuzzes. Exit 0 when the
// NFC holds and every difference is explained, 1 when not, 2 on a usage error.

#include "idna_ada_common.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace
{
   // A source column's text as UTF-8: the file's \uXXXX and \x{XXXX} escapes
   // read, "" as the empty string. False for a column that holds a lone
   // surrogate, which is no UTF-8 and nothing a URL can carry.
   bool Unescaped_(const std::string &column, std::string &utf8)
   {
      utf8.clear();
      if (column == "\"\"")
         return true;

      std::vector<unsigned int> codePoints;
      size_t i = 0;
      while (i < column.size())
      {
         unsigned int value = 0;
         if (column[i] == '\\' && i + 5 < column.size() && column[i + 1] == 'u')
         {
            value = static_cast<unsigned int>(std::stoul(column.substr(i + 2, 4), nullptr, 16));
            i += 6;
         }
         else if (column[i] == '\\' && i + 2 < column.size() && column[i + 1] == 'x' && column[i + 2] == '{')
         {
            const size_t close = column.find('}', i);
            if (close == std::string::npos)
               return false;
            value = static_cast<unsigned int>(std::stoul(column.substr(i + 3, close - i - 3), nullptr, 16));
            i = close + 1;
         }
         else
         {
            const unsigned char lead = static_cast<unsigned char>(column[i]);
            const size_t length = lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
            std::vector<unsigned int> one;
            if (!idna_ada::DecodeUtf8(reinterpret_cast<const uint8_t *>(column.data() + i), std::min(length, column.size() - i), one) ||
                one.size() != 1)
               return false;
            value = one[0];
            i += length;
         }

         if (value >= 0xD800 && value <= 0xDFFF)
            return false;
         codePoints.push_back(value);
      }

      utf8 = HM::Idna::ToUtf8(codePoints);
      return true;
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

   // The status column of a test line that ToASCII (Nontransitional) gives: the
   // fifth, or the third when the fifth is empty (the file's FORMAT section).
   std::string AsciiStatus_(const std::string &text)
   {
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
      if (columns.size() < 5)
         return std::string();
      return columns[4].empty() ? columns[2] : columns[4];
   }

   // Whether a status column holds a code that begins with this letter, or,
   // for 0, any code.
   bool HasCode_(const std::string &status, char letter)
   {
      for (size_t i = 0; i + 1 < status.size(); i++)
         if ((status[i] == '[' || status[i] == ' ') && status[i + 1] != ']' && status[i + 1] != ' ' &&
             (letter == 0 || status[i + 1] == letter))
            return true;
      return false;
   }

   // NormalizationTest.txt's columns c1 to c5, each a sequence of hexadecimal
   // code points; NFC(c1) = NFC(c2) = NFC(c3) = c2 and NFC(c4) = NFC(c5) = c4
   // on every line, and every code point Part 1 does not list is its own NFC.
   // The number of failures, each printed; -1 when the file cannot be read.
   long NormalizationFailures_(const char *path, size_t &linesRun)
   {
      std::ifstream file(path);
      if (!file)
         return -1;

      long failures = 0;
      linesRun = 0;
      bool partOne = false;
      std::set<unsigned int> listed;
      std::string line;
      while (std::getline(file, line))
      {
         if (!line.empty() && line.back() == '\r')
            line.pop_back();
         if (line.compare(0, 5, "@Part") == 0)
         {
            partOne = line.compare(0, 6, "@Part1") == 0;
            continue;
         }
         const std::string data = line.substr(0, line.find('#'));
         if (data.find(';') == std::string::npos)
            continue;

         std::vector<std::vector<unsigned int> > columns;
         std::stringstream fields(data);
         std::string field;
         while (columns.size() < 5 && std::getline(fields, field, ';'))
         {
            std::vector<unsigned int> codePoints;
            std::stringstream hex(field);
            std::string word;
            while (hex >> word)
               codePoints.push_back(static_cast<unsigned int>(std::stoul(word, nullptr, 16)));
            columns.push_back(codePoints);
         }
         if (columns.size() != 5)
            continue;

         linesRun++;
         if (partOne && columns[0].size() == 1)
            listed.insert(columns[0][0]);

         const int wanted[5] = { 1, 1, 1, 3, 3 };
         for (int column = 0; column < 5; column++)
         {
            std::vector<unsigned int> normalised = columns[column];
            HM::Idna::ToNfc(normalised);
            if (normalised != columns[wanted[column]])
            {
               failures++;
               printf("NFC FAILED  NormalizationTest.txt: %s (column c%d)\n", data.c_str(), column + 1);
            }
         }
         if (!HM::Idna::IsNfc(columns[1]) || !HM::Idna::IsNfc(columns[3]))
         {
            failures++;
            printf("NFC FAILED  NormalizationTest.txt: %s (IsNfc of c2 or c4)\n", data.c_str());
         }
      }

      for (unsigned int codePoint = 0; codePoint <= 0x10FFFF; codePoint++)
      {
         if ((codePoint >= 0xD800 && codePoint <= 0xDFFF) || listed.count(codePoint) != 0)
            continue;
         std::vector<unsigned int> one(1, codePoint);
         HM::Idna::ToNfc(one);
         if (one.size() != 1 || one[0] != codePoint)
         {
            failures++;
            printf("NFC FAILED  U+%04X, which Part 1 does not list, is not its own NFC\n", codePoint);
         }
      }

      return failures;
   }

   std::string Answer_(bool succeeded, const std::string &ascii)
   {
      return succeeded ? "\"" + ascii + "\"" : std::string("failure");
   }
}

int
main(int argc, char *argv[])
{
   if (argc != 2)
   {
      printf("usage: %s NormalizationTest.txt\n", argv[0]);
      return 2;
   }

   size_t normalisationLines = 0;
   const long normalisationFailures = NormalizationFailures_(argv[1], normalisationLines);
   if (normalisationFailures < 0)
   {
      printf("Cannot read %s.\n", argv[1]);
      return 2;
   }
   if (normalisationFailures > 0 || normalisationLines < 20000)
   {
      printf("FAILED: the server's NFC against NormalizationTest.txt: %ld failure(s) in %zu lines\n", normalisationFailures,
             normalisationLines);
      return 1;
   }
   printf("The server's NFC holds to NormalizationTest.txt: all %zu lines, and every code point it does not list.\n",
          normalisationLines);

   printf("ada %s reads the direction, or whether it is a mark, of %zu code points otherwise than the server's data (U+10D48: %s).\n",
          AdaVersion(), idna_ada::CountAdaReadsDirectionOtherwise(),
          idna_ada::AdaReadsDirectionOtherwise(0x10D48) ? "otherwise" : "alike");
   {
      // Of them, the ones a label may hold, by Unicode block, so that a run's
      // log shows what the fuzz sets aside: new scripts, not old ones.
      std::map<unsigned int, size_t> byBlock;
      size_t valid = 0;
      for (unsigned int c = 0; c < 0x110000; c++)
      {
         if (!idna_ada::AdaReadsDirectionOtherwise(c))
            continue;
         std::vector<HM::Idna::Label> one;
         HM::Idna::ProcessLabels(std::vector<unsigned int>(1, c), HM::Idna::UrlStandardOptions(), one);
         if (one.size() == 1 && one[0].unicode == std::vector<unsigned int>(1, c) && !(one[0].errors & HM::Idna::ErrorStatus))
         {
            valid++;
            byBlock[c & ~0x7Fu]++;
         }
      }
      printf("  of them %zu that a label may hold, in %zu runs of 128:", valid, byBlock.size());
      for (const auto &entry : byBlock)
         printf(" U+%04X(%zu)", entry.first, entry.second);
      printf("\n");
   }

   std::map<std::string, size_t> setAside;
   size_t lines = 0;
   size_t agreed = 0;
   size_t skipped = 0;
   size_t versions = 0;
   size_t unexplained = 0;

   for (size_t line = 0; line < HM::IdnaTables::TestLineCount; line++)
   {
      const std::string text = HM::IdnaTables::TestLines[line];
      const std::string source = Trimmed_(text.substr(0, text.find(';')));

      std::string utf8;
      if (!Unescaped_(source, utf8))
      {
         skipped++;
         continue;
      }

      lines++;
      std::vector<unsigned int> codePoints;
      idna_ada::DecodeUtf8(reinterpret_cast<const uint8_t *>(utf8.data()), utf8.size(), codePoints);

      std::string ours;
      std::string theirs;
      const bool oursSucceeded = HM::Idna::DomainToAscii(codePoints, ours);
      const bool theirsSucceeded = AdaDomainParser(utf8, theirs);
      if (oursSucceeded == theirsSucceeded && (!oursSucceeded || ours == theirs))
      {
         agreed++;
         continue;
      }

      // A code point, of the line or of a label as UTS 46 processes it, that
      // Unicode 18.0.0 gave other data than the version of ada's tables did,
      // or whose direction or mark ada reads otherwise: there the tables
      // differ, not the programs.
      if (idna_ada::DependsOnTablesThatDiffer(codePoints))
      {
         versions++;
         continue;
      }

      // One of ada's departures, where ada took what the server refused,
      // and the file itself fails the line: for RFC 5893's rule, by that rule.
      if (theirsSucceeded && idna_ada::AdaAnswerNotNfc(theirs))
      {
         setAside["ada's answer is not in NFC"]++;
         continue;
      }

      const char *why = idna_ada::AdaDeviation(codePoints);
      const std::string status = AsciiStatus_(text);
      if (why != nullptr && !oursSucceeded && theirsSucceeded &&
          HasCode_(status, why == idna_ada::joinerDeparture ? 0 : why == idna_ada::bidiDeparture ? 'B' : 'P'))
      {
         setAside[why]++;
         continue;
      }

      unexplained++;
      printf("UNEXPLAINED  IdnaTestV2.txt test line %zu: %s (status %s)\n  hMailServer: %s\n  ada %s: %s\n", line + 1,
             source.c_str(), status.c_str(), Answer_(oursSucceeded, ours).c_str(), AdaVersion(),
             Answer_(theirsSucceeded, theirs).c_str());
   }

   printf("ada %s against IdnaTestV2.txt's %zu source columns that are UTF-8 (%zu with a lone surrogate left out): "
          "%zu agree with the server's domain parser\n", AdaVersion(), lines, skipped, agreed);
   printf("  %zu differ, set aside: a code point whose UTS 46 data differ between ada's Unicode and the server's, or whose direction or mark ada reads otherwise\n", versions);
   for (const auto &entry : setAside)
      printf("  %zu differ, set aside, each a line the file fails: %s\n", entry.second, entry.first.c_str());

   if (unexplained > 0)
   {
      printf("FAILED: %zu difference(s) that are neither one of ada's four departures from UTS 46 nor of the two programs' tables\n",
             unexplained);
      return 1;
   }

   printf("Every difference is of the two programs' tables or one of ada's four departures from UTS 46.\n");
   return 0;
}
