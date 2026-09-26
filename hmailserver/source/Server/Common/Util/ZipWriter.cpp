// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"
#include "ZipWriter.h"
#include "FileUtilities.h"
#include "ByteBuffer.h"

#include "../../zlib/zlib.h"
#include <time.h>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      const unsigned int Limit = 0xFFFFFFF0u;
   }

   ZipWriter::ZipWriter() :
      position_(0),
      failed_(false),
      time_(0),
      date_(0)
   {
      // The entries carry the moment the archive was made, in DOS terms: local
      // time, two-second resolution, years from 1980.
      time_t now = time(0);
      struct tm local;
#ifdef _WIN32
      localtime_s(&local, &now);
#else
      localtime_r(&now, &local);
#endif
      time_ = (unsigned short) ((local.tm_hour << 11) | (local.tm_min << 5) | (local.tm_sec / 2));
      int year = local.tm_year + 1900;
      if (year < 1980)
         year = 1980;
      date_ = (unsigned short) (((year - 1980) << 9) | ((local.tm_mon + 1) << 5) | local.tm_mday);
   }

   ZipWriter::~ZipWriter()
   {
      if (file_.IsOpen())
         file_.Close();
   }

   void
   ZipWriter::Fail_(const String &reason)
   {
      if (!failed_)
         error_ = reason;
      failed_ = true;
   }

   bool
   ZipWriter::Open(const String &path)
   {
      if (!file_.Open(path, File::OTCreate))
      {
         Fail_(_T("The archive file could not be created: ") + path);
         return false;
      }

      return true;
   }

   void
   ZipWriter::Put16_(AnsiString &out, unsigned int value)
   {
      char bytes[2] = { (char) (value & 0xFF), (char) ((value >> 8) & 0xFF) };
      out.append(bytes, 2);
   }

   void
   ZipWriter::Put32_(AnsiString &out, unsigned int value)
   {
      char bytes[4] = { (char) (value & 0xFF), (char) ((value >> 8) & 0xFF), (char) ((value >> 16) & 0xFF), (char) ((value >> 24) & 0xFF) };
      out.append(bytes, 4);
   }

   void
   ZipWriter::Put64_(AnsiString &out, unsigned long long value)
   {
      Put32_(out, (unsigned int) (value & 0xFFFFFFFFULL));
      Put32_(out, (unsigned int) (value >> 32));
   }

   bool
   ZipWriter::Deflate_(const AnsiString &bytes, AnsiString &deflated)
   {
      z_stream stream;
      memset(&stream, 0, sizeof(stream));

      // -15: a raw deflate stream, no zlib header, which is what a .zip entry
      // holds (the CRC lives in the entry's header instead).
      if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK)
         return false;

      // Deflated straight into the answer and cut to what was produced, rather
      // than into a buffer of its own and copied: an entry may be a 256 MB
      // part of an mbox, and a third copy of it is memory the service has
      // better uses for.
      const uLong bound = deflateBound(&stream, (uLong) bytes.size());
      deflated.clear();
      deflated.resize((size_t) bound + 16);

      stream.next_in = (Bytef *) bytes.data();
      stream.avail_in = (uInt) bytes.size();
      stream.next_out = (Bytef *) &*deflated.begin();
      stream.avail_out = (uInt) deflated.size();

      const int code = deflate(&stream, Z_FINISH);
      const size_t produced = deflated.size() - stream.avail_out;
      deflateEnd(&stream);

      if (code != Z_STREAM_END)
      {
         deflated.clear();
         return false;
      }

      deflated.resize(produced);
      return true;
   }

   bool
   ZipWriter::Write_(const AnsiString &bytes)
   {
      if (!file_.Write(bytes))
      {
         Fail_(_T("The archive could not be written."));
         return false;
      }

      position_ += (unsigned int) bytes.size();
      return true;
   }

   bool
   ZipWriter::Add(const AnsiString &name, const AnsiString &bytes)
   {
      if (failed_ || !file_.IsOpen())
         return false;

      if (bytes.size() >= Limit)
      {
         Fail_(_T("An entry is larger than the 4 GB the archive format holds."));
         return false;
      }

      AnsiString deflated;
      unsigned short method = 8;

      if (!Deflate_(bytes, deflated) || deflated.size() >= bytes.size())
      {
         deflated = bytes;
         method = 0;
      }

      if ((unsigned long long) position_ + deflated.size() + name.size() + 30 >= Limit)
      {
         Fail_(_T("The archive would be larger than the 4 GB the format holds."));
         return false;
      }

      Entry entry;
      entry.name = name;
      entry.crc = (unsigned int) crc32(0L, (const Bytef *) bytes.data(), (uInt) bytes.size());
      entry.compressedSize = (unsigned int) deflated.size();
      entry.size = (unsigned int) bytes.size();
      entry.offset = position_;
      entry.method = method;
      entry.time = time_;
      entry.date = date_;

      AnsiString header;
      Put32_(header, 0x04034b50);
      Put16_(header, 20);
      Put16_(header, 0x0800);       // bit 11: the name is UTF-8
      Put16_(header, method);
      Put16_(header, entry.time);
      Put16_(header, entry.date);
      Put32_(header, entry.crc);
      Put32_(header, entry.compressedSize);
      Put32_(header, entry.size);
      Put16_(header, (unsigned int) name.size());
      Put16_(header, 0);
      header += name;

      if (!Write_(header) || !Write_(deflated))
         return false;

      entries_.push_back(entry);
      return true;
   }

   bool
   ZipWriter::Close()
   {
      if (!file_.IsOpen())
         return !failed_;

      if (failed_)
      {
         file_.Close();
         return false;
      }

      const unsigned int directoryOffset = position_;
      AnsiString directory;

      for (const Entry &entry : entries_)
      {
         Put32_(directory, 0x02014b50);
         Put16_(directory, 20);
         Put16_(directory, 20);
         Put16_(directory, 0x0800);
         Put16_(directory, entry.method);
         Put16_(directory, entry.time);
         Put16_(directory, entry.date);
         Put32_(directory, entry.crc);
         Put32_(directory, entry.compressedSize);
         Put32_(directory, entry.size);
         Put16_(directory, (unsigned int) entry.name.size());
         Put16_(directory, 0);
         Put16_(directory, 0);
         Put16_(directory, 0);
         Put16_(directory, 0);
         Put32_(directory, 0);
         Put32_(directory, entry.offset);
         directory += entry.name;
      }

      AnsiString end;

      // The classic end record counts the entries in 16 bits; written as they
      // were, 70,000 entries read as 4,464, and a reader that trusts the count
      // refuses the archive or lists part of it. From 65,535 on (0xFFFF is the
      // mark that says "look in the ZIP64 record") the ZIP64 end record and its
      // locator go first, with the count in 64 bits. The sizes and offsets
      // stay 32-bit: Add refuses anything past 4 GB.
      const unsigned long long count = (unsigned long long) entries_.size();
      const bool zip64 = count >= 0xFFFF;

      if (zip64)
      {
         const unsigned long long recordOffset = (unsigned long long) directoryOffset + (unsigned long long) directory.size();

         Put32_(end, 0x06064b50);      // ZIP64 end of central directory record
         Put64_(end, 44);              // its size, less the 12 bytes before this
         Put16_(end, 45);              // version made by: 4.5, ZIP64
         Put16_(end, 45);              // version needed to extract
         Put32_(end, 0);               // this disk
         Put32_(end, 0);               // the disk the directory starts on
         Put64_(end, count);           // entries on this disk
         Put64_(end, count);           // entries in all
         Put64_(end, (unsigned long long) directory.size());
         Put64_(end, (unsigned long long) directoryOffset);

         Put32_(end, 0x07064b50);      // ZIP64 end of central directory locator
         Put32_(end, 0);               // the disk the record is on
         Put64_(end, recordOffset);
         Put32_(end, 1);               // disks in all
      }

      Put32_(end, 0x06054b50);
      Put16_(end, 0);
      Put16_(end, 0);
      Put16_(end, zip64 ? 0xFFFF : (unsigned int) count);
      Put16_(end, zip64 ? 0xFFFF : (unsigned int) count);
      Put32_(end, (unsigned int) directory.size());
      Put32_(end, directoryOffset);
      Put16_(end, 0);

      const bool written = Write_(directory) && Write_(end);
      file_.Close();

      return written && !failed_;
   }

   namespace
   {
      unsigned long long Little_(const AnsiString &bytes, size_t at, int width)
      {
         unsigned long long value = 0;
         for (int i = width - 1; i >= 0; i--)
            value = (value << 8) | (unsigned char) bytes.c_str()[at + (size_t) i];
         return value;
      }

      bool ReadWhole_(const String &path, AnsiString &bytes)
      {
         File file;
         if (!file.Open(path, File::OTReadOnly))
            return false;

         std::shared_ptr<ByteBuffer> buffer = file.ReadFile();
         file.Close();
         if (!buffer)
            return false;

         bytes.assign(buffer->GetCharBuffer(), buffer->GetSize());
         return true;
      }
   }

   void
   ZipWriterTester::Test()
   {
      // The end of an archive of a few entries: the classic record alone,
      // counting them.
      {
         const String path = FileUtilities::GetTempFileName();
         ZipWriter zip;
         if (!zip.Open(path)) throw std::logic_error("ZipWriter: a temporary archive could not be created");
         for (int i = 0; i < 3; i++)
            if (!zip.Add("small.txt", "x")) throw std::logic_error("ZipWriter: a small entry was refused");
         if (!zip.Close()) throw std::logic_error("ZipWriter: a small archive did not close");

         AnsiString bytes;
         const bool read = ReadWhole_(path, bytes);
         FileUtilities::DeleteFile(path);
         if (!read || bytes.size() < 22) throw std::logic_error("ZipWriter: the small archive could not be read back");

         const size_t end = bytes.size() - 22;
         if (Little_(bytes, end, 4) != 0x06054b50) throw std::logic_error("ZipWriter: the small archive does not end with the end record");
         if (Little_(bytes, end + 8, 2) != 3 || Little_(bytes, end + 10, 2) != 3) throw std::logic_error("ZipWriter: the small archive's count is not 3");
         if (bytes.find("PK\x06\x06") != std::string::npos) throw std::logic_error("ZipWriter: a small archive has a ZIP64 record");
      }

      // 70,000 entries: past what 16 bits count. The classic record says
      // 0xFFFF, and the ZIP64 record its locator points at says 70,000 - where
      // the classic record alone said 70,000 mod 65,536 = 4,464.
      {
         const unsigned int entries = 70000;
         const String path = FileUtilities::GetTempFileName();
         ZipWriter zip;
         if (!zip.Open(path)) throw std::logic_error("ZipWriter: a temporary archive could not be created");
         for (unsigned int i = 0; i < entries; i++)
         {
            AnsiString name;
            name.Format("e%u.txt", i);
            if (!zip.Add(name, "x")) throw std::logic_error("ZipWriter: an entry of the large archive was refused");
         }
         if (zip.Entries() != (int) entries) throw std::logic_error("ZipWriter: not every entry was counted");
         if (!zip.Close()) throw std::logic_error("ZipWriter: the large archive did not close");

         AnsiString bytes;
         const bool read = ReadWhole_(path, bytes);
         FileUtilities::DeleteFile(path);
         if (!read || bytes.size() < 22 + 20 + 56) throw std::logic_error("ZipWriter: the large archive could not be read back");

         const size_t end = bytes.size() - 22;
         if (Little_(bytes, end, 4) != 0x06054b50) throw std::logic_error("ZipWriter: the large archive does not end with the end record");
         if (Little_(bytes, end + 8, 2) != 0xFFFF || Little_(bytes, end + 10, 2) != 0xFFFF)
            throw std::logic_error("ZipWriter: the classic record of 70,000 entries does not say 0xFFFF");

         const size_t locator = end - 20;
         if (Little_(bytes, locator, 4) != 0x07064b50) throw std::logic_error("ZipWriter: no ZIP64 locator before the end record");
         const unsigned long long record = Little_(bytes, locator + 8, 8);
         if (record + 56 != (unsigned long long) locator) throw std::logic_error("ZipWriter: the locator does not point at the record before it");
         if (Little_(bytes, (size_t) record, 4) != 0x06064b50) throw std::logic_error("ZipWriter: no ZIP64 end record where the locator points");
         if (Little_(bytes, (size_t) record + 4, 8) != 44) throw std::logic_error("ZipWriter: the ZIP64 record's size is not 44");
         if (Little_(bytes, (size_t) record + 24, 8) != entries || Little_(bytes, (size_t) record + 32, 8) != entries)
            throw std::logic_error("ZipWriter: the ZIP64 record does not count 70,000 entries");

         const unsigned long long directorySize = Little_(bytes, (size_t) record + 40, 8);
         const unsigned long long directoryOffset = Little_(bytes, (size_t) record + 48, 8);
         if (directoryOffset + directorySize != record) throw std::logic_error("ZipWriter: the directory does not end where the ZIP64 record starts");
         if (Little_(bytes, (size_t) directoryOffset, 4) != 0x02014b50) throw std::logic_error("ZipWriter: no central directory where the ZIP64 record says");
         if (Little_(bytes, end + 12, 4) != directorySize || Little_(bytes, end + 16, 4) != directoryOffset)
            throw std::logic_error("ZipWriter: the classic record and the ZIP64 record disagree about the directory");
      }
   }
}
