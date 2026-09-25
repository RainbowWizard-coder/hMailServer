// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// See PstCodePages.h.

#include "StdAfx.h"

#include "PstCodePages.h"
#include "PstFormat.h"

#ifdef HM_PLATFORM_POSIX
#include <errno.h>
#include <iconv.h>
#endif

#include <vector>

namespace HM
{
   namespace
   {
      struct CodePageRow
      {
         int codePage;
         const char *charset;
         const char *iconvName;
      };

      // Every code page Outlook writes a PT_STRING8 or an HTML body in for the
      // languages it ships, with the label RFC 2978's registry gives it and
      // what iconv calls it (for 932, 936 and 949, the Microsoft variants of the
      // standards, which carry the characters Windows added to them).
      const CodePageRow Rows[] =
      {
         { 437,   "ibm437",          "CP437" },
         { 850,   "ibm850",          "CP850" },
         { 852,   "ibm852",          "CP852" },
         { 866,   "cp866",           "CP866" },
         { 874,   "windows-874",     "CP874" },
         { 932,   "shift_jis",       "CP932" },
         { 936,   "gb2312",          "CP936" },
         { 949,   "ks_c_5601-1987",  "CP949" },
         { 950,   "big5",            "CP950" },
         { 1250,  "windows-1250",    "CP1250" },
         { 1251,  "windows-1251",    "CP1251" },
         { 1252,  "windows-1252",    "CP1252" },
         { 1253,  "windows-1253",    "CP1253" },
         { 1254,  "windows-1254",    "CP1254" },
         { 1255,  "windows-1255",    "CP1255" },
         { 1256,  "windows-1256",    "CP1256" },
         { 1257,  "windows-1257",    "CP1257" },
         { 1258,  "windows-1258",    "CP1258" },
         { 20127, "us-ascii",        "ASCII" },
         { 20866, "koi8-r",          "KOI8-R" },
         { 21866, "koi8-u",          "KOI8-U" },
         { 28591, "iso-8859-1",      "ISO-8859-1" },
         { 28592, "iso-8859-2",      "ISO-8859-2" },
         { 28593, "iso-8859-3",      "ISO-8859-3" },
         { 28594, "iso-8859-4",      "ISO-8859-4" },
         { 28595, "iso-8859-5",      "ISO-8859-5" },
         { 28596, "iso-8859-6",      "ISO-8859-6" },
         { 28597, "iso-8859-7",      "ISO-8859-7" },
         { 28598, "iso-8859-8",      "ISO-8859-8" },
         { 28599, "iso-8859-9",      "ISO-8859-9" },
         { 28603, "iso-8859-13",     "ISO-8859-13" },
         { 28605, "iso-8859-15",     "ISO-8859-15" },
         { 50220, "iso-2022-jp",     "ISO-2022-JP" },
         { 50221, "iso-2022-jp",     "ISO-2022-JP" },
         { 50222, "iso-2022-jp",     "ISO-2022-JP" },
         { 51932, "euc-jp",          "EUC-JP" },
         { 51949, "euc-kr",          "EUC-KR" },
         { 54936, "gb18030",         "GB18030" },
         { 65000, "utf-7",           "UTF-7" },
         { 65001, "utf-8",           "UTF-8" }
      };

      const CodePageRow *Find(int codePage)
      {
         for (size_t i = 0; i < sizeof(Rows) / sizeof(Rows[0]); i++)
         {
            if (Rows[i].codePage == codePage)
               return &Rows[i];
         }
         return nullptr;
      }
   }

   std::string
   PstCodePages::CharsetName(int codePage)
   {
      const CodePageRow *row = Find(codePage);
      return row ? std::string(row->charset) : std::string();
   }

   bool
   PstCodePages::Known(int codePage)
   {
      return Find(codePage) != nullptr;
   }

   size_t
   PstCodePages::Count()
   {
      return sizeof(Rows) / sizeof(Rows[0]);
   }

   bool
   PstCodePages::Row(size_t index, int &codePage, std::string &charset, std::string &iconvName)
   {
      if (index >= Count())
         return false;

      codePage = Rows[index].codePage;
      charset = Rows[index].charset;
      iconvName = Rows[index].iconvName;
      return true;
   }

   bool
   PstCodePages::Decode(const std::string &bytes, int codePage, std::string &utf8)
   {
      const CodePageRow *row = Find(codePage);
      if (!row)
         return false;

      if (bytes.empty())
      {
         utf8.clear();
         return true;
      }

      if (codePage == 65001)
      {
         // Already UTF-8; a byte that is not becomes U+FFFD, as the other
         // conversions treat what they cannot read.
         if (PstFormat::IsUtf8(bytes))
         {
            utf8 = bytes;
            return true;
         }
      }

#ifdef HM_PLATFORM_POSIX
      iconv_t converter = iconv_open("UTF-8", row->iconvName);
      if (converter == (iconv_t) -1)
         return false;

      std::string out;
      out.reserve(bytes.size() * 2);

      char *input = const_cast<char *>(bytes.data());
      size_t inputLeft = bytes.size();
      std::vector<char> buffer(4096);

      while (inputLeft > 0)
      {
         char *output = &buffer[0];
         size_t outputLeft = buffer.size();
         const size_t result = iconv(converter, &input, &inputLeft, &output, &outputLeft);
         out.append(&buffer[0], buffer.size() - outputLeft);

         if (result == (size_t) -1)
         {
            if (errno == E2BIG)
               continue;

            // A byte the code page does not have, or a sequence cut short: one
            // replacement character, and on after it.
            PstFormat::AppendUtf8(out, 0xFFFD);
            input++;
            inputLeft--;
         }
      }

      // The shift back to the initial state, for the stateful encodings.
      {
         char *output = &buffer[0];
         size_t outputLeft = buffer.size();
         iconv(converter, nullptr, nullptr, &output, &outputLeft);
         out.append(&buffer[0], buffer.size() - outputLeft);
      }

      iconv_close(converter);
      utf8 = out;
      return true;
#else
      if (bytes.size() > 0x7FFFFFFF)
         return false;

      const int needed = MultiByteToWideChar((UINT) codePage, 0, bytes.data(), (int) bytes.size(), nullptr, 0);
      if (needed <= 0)
         return false;

      std::wstring wide((size_t) needed, L'\0');
      const int written = MultiByteToWideChar((UINT) codePage, 0, bytes.data(), (int) bytes.size(), &wide[0], needed);
      if (written <= 0)
         return false;

      wide.resize((size_t) written);
      utf8 = PstFormat::WideToUtf8(wide);
      return true;
#endif
   }
}
