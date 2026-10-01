// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "File.h"
#include <vector>

namespace HM
{
   // A .zip file written entry by entry: each entry deflated in memory (the
   // vendored zlib, raw deflate), its local header and bytes appended, and the
   // central directory written on Close. The 32-bit format for sizes and
   // offsets - an archive or an entry past 4 GB is refused rather than written
   // wrong - with entry names marked as UTF-8, which every unpacker of the last
   // fifteen years reads. The count of entries is 16 bits in the classic end
   // record, so from 65,535 entries on Close writes the ZIP64 end record and
   // its locator as well (APPNOTE 4.3.14, 4.3.15), which carry the count in 64
   // bits: an eDiscovery export may hold 100,000 messages. What a person's
   // export is handed as: one file, not a directory.
   class ZipWriter
   {
   public:
      ZipWriter();
      ~ZipWriter();

      bool Open(const String &path);

      // Deflates and appends one entry. An entry that does not shrink is stored
      // as it is. Names use '/' between directories.
      bool Add(const AnsiString &name, const AnsiString &bytes);

      // Writes the central directory and closes the file. False when anything
      // went wrong, with the reason in Error().
      bool Close();

      String Error() const { return error_; }
      int Entries() const { return (int) entries_.size(); }

   private:

      struct Entry
      {
         AnsiString name;
         unsigned int crc = 0;
         unsigned int compressedSize = 0;
         unsigned int size = 0;
         unsigned int offset = 0;
         unsigned short method = 0;
         unsigned short time = 0;
         unsigned short date = 0;
      };

      static void Put16_(AnsiString &out, unsigned int value);
      static void Put32_(AnsiString &out, unsigned int value);
      static void Put64_(AnsiString &out, unsigned long long value);
      static bool Deflate_(const AnsiString &bytes, AnsiString &deflated);
      bool Write_(const AnsiString &bytes);
      void Fail_(const String &reason);

      File file_;
      std::vector<Entry> entries_;
      unsigned int position_;
      bool failed_;
      String error_;
      unsigned short time_;
      unsigned short date_;
   };

   class ZipWriterTester
   {
   public:
      void Test();
   };
}
