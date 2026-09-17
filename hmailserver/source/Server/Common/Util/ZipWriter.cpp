// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"
#include "ZipWriter.h"

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

   bool
   ZipWriter::Deflate_(const AnsiString &bytes, AnsiString &deflated)
   {
      z_stream stream;
      memset(&stream, 0, sizeof(stream));

      // -15: a raw deflate stream, no zlib header, which is what a .zip entry
      // holds (the CRC lives in the entry's header instead).
      if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK)
         return false;

      const uLong bound = deflateBound(&stream, (uLong) bytes.size());
      std::vector<unsigned char> out(bound + 16);

      stream.next_in = (Bytef *) bytes.data();
      stream.avail_in = (uInt) bytes.size();
      stream.next_out = out.data();
      stream.avail_out = (uInt) out.size();

      const int code = deflate(&stream, Z_FINISH);
      const size_t produced = out.size() - stream.avail_out;
      deflateEnd(&stream);

      if (code != Z_STREAM_END)
         return false;

      deflated.assign((const char *) out.data(), produced);
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
      Put32_(end, 0x06054b50);
      Put16_(end, 0);
      Put16_(end, 0);
      Put16_(end, (unsigned int) entries_.size());
      Put16_(end, (unsigned int) entries_.size());
      Put32_(end, (unsigned int) directory.size());
      Put32_(end, directoryOffset);
      Put16_(end, 0);

      const bool written = Write_(directory) && Write_(end);
      file_.Close();

      return written && !failed_;
   }
}
