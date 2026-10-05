// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// See PstFormat.h.

#include "StdAfx.h"

#include "PstFormat.h"

#include <cstdio>
#include <cstring>

namespace HM
{
   namespace
   {
      // [MS-PST] 5.1, mpbbCrypt: mpbbR (encode), mpbbS (the cyclic cipher's
      // middle step, its own inverse) and mpbbI (decode, the inverse of mpbbR).
      // PstTester checks that mpbbI inverts mpbbR and that mpbbS inverts itself.
      const unsigned char CryptTableBytes[768] =
      {
         65, 54, 19, 98, 168, 33, 110, 187, 244, 22, 204, 4, 127, 100, 232, 93,
         30, 242, 203, 42, 116, 197, 94, 53, 210, 149, 71, 158, 150, 45, 154, 136,
         76, 125, 132, 63, 219, 172, 49, 182, 72, 95, 246, 196, 216, 57, 139, 231,
         35, 59, 56, 142, 200, 193, 223, 37, 177, 32, 165, 70, 96, 78, 156, 251,
         170, 211, 86, 81, 69, 124, 85, 0, 7, 201, 43, 157, 133, 155, 9, 160,
         143, 173, 179, 15, 99, 171, 137, 75, 215, 167, 21, 90, 113, 102, 66, 191,
         38, 74, 107, 152, 250, 234, 119, 83, 178, 112, 5, 44, 253, 89, 58, 134,
         126, 206, 6, 235, 130, 120, 87, 199, 141, 67, 175, 180, 28, 212, 91, 205,
         226, 233, 39, 79, 195, 8, 114, 128, 207, 176, 239, 245, 40, 109, 190, 48,
         77, 52, 146, 213, 14, 60, 34, 50, 229, 228, 249, 159, 194, 209, 10, 129,
         18, 225, 238, 145, 131, 118, 227, 151, 230, 97, 138, 23, 121, 164, 183, 220,
         144, 122, 92, 140, 2, 166, 202, 105, 222, 80, 26, 17, 147, 185, 82, 135,
         88, 252, 237, 29, 55, 73, 27, 106, 224, 41, 51, 153, 189, 108, 217, 148,
         243, 64, 84, 111, 240, 198, 115, 184, 214, 62, 101, 24, 68, 31, 221, 103,
         16, 241, 12, 25, 236, 174, 3, 161, 20, 123, 169, 11, 255, 248, 163, 192,
         162, 1, 247, 46, 188, 36, 104, 117, 13, 254, 186, 47, 181, 208, 218, 61,
         20, 83, 15, 86, 179, 200, 122, 156, 235, 101, 72, 23, 22, 21, 159, 2,
         204, 84, 124, 131, 0, 13, 12, 11, 162, 98, 168, 118, 219, 217, 237, 199,
         197, 164, 220, 172, 133, 116, 214, 208, 167, 155, 174, 154, 150, 113, 102, 195,
         99, 153, 184, 221, 115, 146, 142, 132, 125, 165, 94, 209, 93, 147, 177, 87,
         81, 80, 128, 137, 82, 148, 79, 78, 10, 107, 188, 141, 127, 110, 71, 70,
         65, 64, 68, 1, 17, 203, 3, 63, 247, 244, 225, 169, 143, 60, 58, 249,
         251, 240, 25, 48, 130, 9, 46, 201, 157, 160, 134, 73, 238, 111, 77, 109,
         196, 45, 129, 52, 37, 135, 27, 136, 170, 252, 6, 161, 18, 56, 253, 76,
         66, 114, 100, 19, 55, 36, 106, 117, 119, 67, 255, 230, 180, 75, 54, 92,
         228, 216, 53, 61, 69, 185, 44, 236, 183, 49, 43, 41, 7, 104, 163, 14,
         105, 123, 24, 158, 33, 57, 190, 40, 26, 91, 120, 245, 35, 202, 42, 176,
         175, 62, 254, 4, 140, 231, 229, 152, 50, 149, 211, 246, 74, 232, 166, 234,
         233, 243, 213, 47, 112, 32, 242, 31, 5, 103, 173, 85, 16, 206, 205, 227,
         39, 59, 218, 186, 215, 194, 38, 212, 145, 29, 210, 28, 34, 51, 248, 250,
         241, 90, 239, 207, 144, 182, 139, 181, 189, 192, 191, 8, 151, 30, 108, 226,
         97, 224, 198, 193, 89, 171, 187, 88, 222, 95, 223, 96, 121, 126, 178, 138,
         71, 241, 180, 230, 11, 106, 114, 72, 133, 78, 158, 235, 226, 248, 148, 83,
         224, 187, 160, 2, 232, 90, 9, 171, 219, 227, 186, 198, 124, 195, 16, 221,
         57, 5, 150, 48, 245, 55, 96, 130, 140, 201, 19, 74, 107, 29, 243, 251,
         143, 38, 151, 202, 145, 23, 1, 196, 50, 45, 110, 49, 149, 255, 217, 35,
         209, 0, 94, 121, 220, 68, 59, 26, 40, 197, 97, 87, 32, 144, 61, 131,
         185, 67, 190, 103, 210, 70, 66, 118, 192, 109, 91, 126, 178, 15, 22, 41,
         60, 169, 3, 84, 13, 218, 93, 223, 246, 183, 199, 98, 205, 141, 6, 211,
         105, 92, 134, 214, 20, 247, 165, 102, 117, 172, 177, 233, 69, 33, 112, 12,
         135, 159, 116, 164, 34, 76, 111, 191, 31, 86, 170, 46, 179, 120, 51, 80,
         176, 163, 146, 188, 207, 25, 28, 167, 99, 203, 30, 77, 62, 75, 27, 155,
         79, 231, 240, 238, 173, 58, 181, 89, 4, 234, 64, 85, 37, 81, 229, 122,
         137, 56, 104, 82, 123, 252, 39, 174, 215, 189, 250, 7, 244, 204, 142, 95,
         239, 53, 156, 132, 43, 21, 213, 119, 52, 73, 182, 18, 10, 127, 113, 136,
         253, 157, 24, 65, 125, 147, 216, 88, 44, 206, 254, 36, 175, 222, 184, 54,
         200, 161, 128, 166, 153, 152, 168, 47, 14, 129, 101, 115, 228, 194, 162, 138,
         212, 225, 17, 208, 8, 139, 42, 242, 237, 154, 100, 63, 193, 108, 249, 236
      };

      struct CrcTable
      {
         uint32_t entries[256] = {};

         CrcTable()
         {
            for (uint32_t i = 0; i < 256; i++)
            {
               uint32_t c = i;
               for (int k = 0; k < 8; k++)
                  c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
               entries[i] = c;
            }
         }
      };

      const CrcTable &Table()
      {
         static const CrcTable table;
         return table;
      }

      void Need(const std::string &data, size_t offset, size_t length, const char *what)
      {
         if (!PstFormat::Fits(offset, length, data.size()))
            throw PstError(std::string("The file ends inside ") + what + ".");
      }

      // Days from 1970-01-01 to a civil date, and back: Howard Hinnant's
      // algorithms, which hold for every date the proleptic Gregorian calendar has.
      void CivilFromDays(int64_t days, int &year, int &month, int &day)
      {
         days += 719468;
         const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
         const int64_t dayOfEra = days - era * 146097;
         const int64_t yearOfEra = (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;
         const int64_t dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
         const int64_t mp = (5 * dayOfYear + 2) / 153;
         day = (int) (dayOfYear - (153 * mp + 2) / 5 + 1);
         month = (int) (mp < 10 ? mp + 3 : mp - 9);
         year = (int) (yearOfEra + era * 400 + (month <= 2 ? 1 : 0));
      }
   }

   std::string
   PstFormat::BmpOnly(const std::string &utf8)
   {
      std::string out;
      out.reserve(utf8.size());
      for (size_t i = 0; i < utf8.size(); i++)
      {
         const unsigned char lead = (unsigned char) utf8[i];
         if (lead >= 0xF0 && lead <= 0xF7)
         {
            // A four-byte sequence, and the continuation bytes it has.
            size_t k = 1;
            while (k < 4 && i + k < utf8.size() && ((unsigned char) utf8[i + k] & 0xC0) == 0x80)
               k++;
            out += "\xEF\xBF\xBD";
            i += k - 1;
            continue;
         }
         out += (char) lead;
      }
      return out;
   }

   const uint64_t PstBudget::UnitBytes;
   const uint64_t PstBudget::UnitItems;
   const uint64_t PstBudget::FileItems;
   const uint64_t PstBudget::FileBytesPerItem;
   const uint64_t PstBudget::FileBytes;
   const uint64_t PstBudget::FileBytesPerByte;

   PstBudget::PstBudget() :
      file_bytes_limit_(0),
      file_items_limit_(0),
      file_bytes_(0),
      file_items_(0),
      unit_bytes_(0),
      unit_items_(0),
      file_exhausted_(false)
   {
   }

   void
   PstBudget::SetFileLimits(uint64_t bytes, uint64_t items)
   {
      file_bytes_limit_ = bytes;
      file_items_limit_ = items;
   }

   void
   PstBudget::SetStop(const std::function<bool()> &stop)
   {
      stop_ = stop;
   }

   void
   PstBudget::SetFileLimitsFor(uint64_t fileSize)
   {
      const uint64_t measured = fileSize < (1ULL << 40) ? fileSize : (1ULL << 40);
      SetFileLimits(FileBytes + measured * FileBytesPerByte, FileItems + measured / FileBytesPerItem);
   }

   void
   PstBudget::BeginUnit()
   {
      unit_bytes_ = 0;
      unit_items_ = 0;
   }

   void
   PstBudget::CheckStop()
   {
      if (stop_ && stop_())
         throw PstStopped();
   }

   void
   PstBudget::Exhausted_(bool wholeFile)
   {
      if (wholeFile)
      {
         file_exhausted_ = true;
         throw PstBudgetExhausted("Reading this file has cost more work than any PST Outlook writes needs for its size; it is damaged, or built to exhaust the reader, and is not read further.", true);
      }

      throw PstBudgetExhausted("Reading this part of the file took more work than any file Outlook writes needs (more than 4,000,000 reads or 512 MB decoded); the file is damaged here, or built to exhaust the reader.", false);
   }

   void
   PstBudget::Bytes(uint64_t bytes)
   {
      unit_bytes_ += bytes;
      file_bytes_ += bytes;

      if (file_bytes_limit_ != 0 && file_bytes_ > file_bytes_limit_)
         Exhausted_(true);
      if (unit_bytes_ > UnitBytes)
         Exhausted_(false);
   }

   void
   PstBudget::Item()
   {
      CheckStop();

      unit_items_++;
      file_items_++;

      if (file_items_limit_ != 0 && file_items_ > file_items_limit_)
         Exhausted_(true);
      if (unit_items_ > UnitItems)
         Exhausted_(false);
   }

   uint32_t
   PstFormat::Crc(const unsigned char *data, size_t length, uint32_t initial)
   {
      const CrcTable &table = Table();
      uint32_t crc = initial;
      for (size_t i = 0; i < length; i++)
         crc = table.entries[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
      return crc;
   }

   uint32_t
   PstFormat::Crc(const std::string &data, uint32_t initial)
   {
      return Crc((const unsigned char *) data.data(), data.size(), initial);
   }

   const unsigned char *
   PstFormat::CryptTable()
   {
      return CryptTableBytes;
   }

   void
   PstFormat::DecodePermute(std::string &data)
   {
      const unsigned char *inverse = CryptTableBytes + 512;
      for (size_t i = 0; i < data.size(); i++)
         data[i] = (char) inverse[(unsigned char) data[i]];
   }

   void
   PstFormat::EncodePermute(std::string &data)
   {
      const unsigned char *forward = CryptTableBytes;
      for (size_t i = 0; i < data.size(); i++)
         data[i] = (char) forward[(unsigned char) data[i]];
   }

   void
   PstFormat::CryptCyclic(std::string &data, uint32_t key)
   {
      const unsigned char *tableR = CryptTableBytes;
      const unsigned char *tableS = CryptTableBytes + 256;
      const unsigned char *tableI = CryptTableBytes + 512;

      uint16_t w = (uint16_t) (key ^ (key >> 16));
      for (size_t i = 0; i < data.size(); i++)
      {
         unsigned char b = (unsigned char) data[i];
         b = (unsigned char) (b + (unsigned char) w);
         b = tableR[b];
         b = (unsigned char) (b + (unsigned char) (w >> 8));
         b = tableS[b];
         b = (unsigned char) (b - (unsigned char) (w >> 8));
         b = tableI[b];
         b = (unsigned char) (b - (unsigned char) w);
         data[i] = (char) b;
         w = (uint16_t) (w + 1);
      }
   }

   bool
   PstFormat::Fits(uint64_t offset, uint64_t length, uint64_t size)
   {
      return offset <= size && length <= size - offset;
   }

   uint8_t
   PstFormat::U8(const std::string &data, size_t offset, const char *what)
   {
      Need(data, offset, 1, what);
      return (uint8_t) data[offset];
   }

   uint16_t
   PstFormat::U16(const std::string &data, size_t offset, const char *what)
   {
      Need(data, offset, 2, what);
      const unsigned char *p = (const unsigned char *) data.data() + offset;
      return (uint16_t) (p[0] | (p[1] << 8));
   }

   uint32_t
   PstFormat::U32(const std::string &data, size_t offset, const char *what)
   {
      Need(data, offset, 4, what);
      const unsigned char *p = (const unsigned char *) data.data() + offset;
      return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
   }

   uint64_t
   PstFormat::U64(const std::string &data, size_t offset, const char *what)
   {
      Need(data, offset, 8, what);
      return (uint64_t) U32(data, offset, what) | ((uint64_t) U32(data, offset + 4, what) << 32);
   }

   void
   PstFormat::AppendUtf8(std::string &out, uint32_t codePoint)
   {
      if (codePoint > 0x10FFFF || (codePoint >= 0xD800 && codePoint <= 0xDFFF))
         codePoint = 0xFFFD;

      if (codePoint < 0x80)
         out += (char) codePoint;
      else if (codePoint < 0x800)
      {
         out += (char) (0xC0 | (codePoint >> 6));
         out += (char) (0x80 | (codePoint & 0x3F));
      }
      else if (codePoint < 0x10000)
      {
         out += (char) (0xE0 | (codePoint >> 12));
         out += (char) (0x80 | ((codePoint >> 6) & 0x3F));
         out += (char) (0x80 | (codePoint & 0x3F));
      }
      else
      {
         out += (char) (0xF0 | (codePoint >> 18));
         out += (char) (0x80 | ((codePoint >> 12) & 0x3F));
         out += (char) (0x80 | ((codePoint >> 6) & 0x3F));
         out += (char) (0x80 | (codePoint & 0x3F));
      }
   }

   std::string
   PstFormat::Utf16ToUtf8(const std::string &utf16)
   {
      std::string out;
      out.reserve(utf16.size() / 2);

      const unsigned char *p = (const unsigned char *) utf16.data();
      const size_t units = utf16.size() / 2;
      for (size_t i = 0; i < units; i++)
      {
         uint32_t unit = (uint32_t) p[2 * i] | ((uint32_t) p[2 * i + 1] << 8);
         if (unit >= 0xD800 && unit <= 0xDBFF && i + 1 < units)
         {
            const uint32_t low = (uint32_t) p[2 * i + 2] | ((uint32_t) p[2 * i + 3] << 8);
            if (low >= 0xDC00 && low <= 0xDFFF)
            {
               AppendUtf8(out, 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00));
               i++;
               continue;
            }
         }
         AppendUtf8(out, unit);
      }

      return out;
   }

   std::string
   PstFormat::WideToUtf8(const std::wstring &text)
   {
      std::string out;
      out.reserve(text.size());

      for (size_t i = 0; i < text.size(); i++)
      {
         uint32_t unit = (uint32_t) text[i];
         if (sizeof(wchar_t) == 2 && unit >= 0xD800 && unit <= 0xDBFF && i + 1 < text.size())
         {
            const uint32_t low = (uint32_t) text[i + 1];
            if (low >= 0xDC00 && low <= 0xDFFF)
            {
               AppendUtf8(out, 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00));
               i++;
               continue;
            }
         }
         AppendUtf8(out, unit);
      }

      return out;
   }

   std::string
   PstFormat::Utf8ToUtf16(const std::string &utf8)
   {
      std::string out;
      size_t i = 0;
      while (i < utf8.size())
      {
         const unsigned char c = (unsigned char) utf8[i];
         uint32_t codePoint = 0xFFFD;
         size_t length = 1;
         if (c < 0x80)
            codePoint = c;
         else if ((c & 0xE0) == 0xC0 && i + 1 < utf8.size())
         {
            codePoint = ((c & 0x1F) << 6) | ((unsigned char) utf8[i + 1] & 0x3F);
            length = 2;
         }
         else if ((c & 0xF0) == 0xE0 && i + 2 < utf8.size())
         {
            codePoint = ((c & 0x0F) << 12) | (((unsigned char) utf8[i + 1] & 0x3F) << 6) | ((unsigned char) utf8[i + 2] & 0x3F);
            length = 3;
         }
         else if ((c & 0xF8) == 0xF0 && i + 3 < utf8.size())
         {
            codePoint = ((c & 0x07) << 18) | (((unsigned char) utf8[i + 1] & 0x3F) << 12) |
                        (((unsigned char) utf8[i + 2] & 0x3F) << 6) | ((unsigned char) utf8[i + 3] & 0x3F);
            length = 4;
         }
         i += length;

         if (codePoint >= 0x10000 && codePoint <= 0x10FFFF)
         {
            const uint32_t v = codePoint - 0x10000;
            const uint32_t high = 0xD800 + (v >> 10);
            const uint32_t low = 0xDC00 + (v & 0x3FF);
            out += (char) (high & 0xFF);
            out += (char) (high >> 8);
            out += (char) (low & 0xFF);
            out += (char) (low >> 8);
         }
         else
         {
            if (codePoint > 0xFFFF)
               codePoint = 0xFFFD;
            out += (char) (codePoint & 0xFF);
            out += (char) (codePoint >> 8);
         }
      }
      return out;
   }

   bool
   PstFormat::IsUtf8(const std::string &bytes)
   {
      size_t i = 0;
      while (i < bytes.size())
      {
         const unsigned char c = (unsigned char) bytes[i];
         size_t length = 0;
         uint32_t minimum = 0;
         uint32_t codePoint = 0;
         if (c < 0x80)
         {
            i++;
            continue;
         }
         else if ((c & 0xE0) == 0xC0) { length = 2; minimum = 0x80; codePoint = c & 0x1F; }
         else if ((c & 0xF0) == 0xE0) { length = 3; minimum = 0x800; codePoint = c & 0x0F; }
         else if ((c & 0xF8) == 0xF0) { length = 4; minimum = 0x10000; codePoint = c & 0x07; }
         else
            return false;

         if (i + length > bytes.size())
            return false;

         for (size_t k = 1; k < length; k++)
         {
            const unsigned char next = (unsigned char) bytes[i + k];
            if ((next & 0xC0) != 0x80)
               return false;
            codePoint = (codePoint << 6) | (next & 0x3F);
         }

         if (codePoint < minimum || codePoint > 0x10FFFF || (codePoint >= 0xD800 && codePoint <= 0xDFFF))
            return false;

         i += length;
      }
      return true;
   }

   bool
   PstFormat::FileTimeToUnix(uint64_t fileTime, int64_t &unixSeconds)
   {
      // 1601-01-01 to 1970-01-01 is 11,644,473,600 seconds; 9999-12-31 is
      // 253,402,300,799 seconds after 1970.
      if (fileTime == 0)
         return false;

      const int64_t seconds = (int64_t) (fileTime / 10000000ULL) - 11644473600LL;
      if (seconds > 253402300799LL)
         return false;

      unixSeconds = seconds;
      return true;
   }

   void
   PstFormat::UnixToCivil(int64_t unixSeconds, int &year, int &month, int &day, int &hour, int &minute, int &second, int &weekday)
   {
      int64_t days = unixSeconds / 86400;
      int64_t rest = unixSeconds % 86400;
      if (rest < 0)
      {
         rest += 86400;
         days--;
      }

      CivilFromDays(days, year, month, day);
      hour = (int) (rest / 3600);
      minute = (int) ((rest % 3600) / 60);
      second = (int) (rest % 60);

      // 1970-01-01 was a Thursday; 0 is Sunday.
      int64_t w = (days + 4) % 7;
      if (w < 0)
         w += 7;
      weekday = (int) w;
   }

   bool
   PstFormat::NidOfEntryId(const std::string &entryId, const std::string &recordKey, uint32_t &nid)
   {
      if (entryId.size() != 24)
         return false;

      if (recordKey.size() == 16 && entryId.compare(4, 16, recordKey) != 0)
         return false;

      nid = U32(entryId, 20, "an EntryID");
      return true;
   }

   std::string
   PstFormat::Hex(const std::string &bytes)
   {
      static const char digits[] = "0123456789abcdef";
      std::string out;
      out.reserve(bytes.size() * 2);
      for (size_t i = 0; i < bytes.size(); i++)
      {
         const unsigned char c = (unsigned char) bytes[i];
         out += digits[c >> 4];
         out += digits[c & 0x0F];
      }
      return out;
   }

   std::string
   PstFormat::Hex32(uint32_t value)
   {
      char text[16];
      snprintf(text, sizeof(text), "%x", (unsigned int) value);
      return text;
   }

   std::string
   PstFormat::CutUtf8(const std::string &utf8, size_t maximum)
   {
      if (utf8.size() <= maximum)
         return utf8;
      if (maximum < 3)
         return std::string(maximum, '.');

      size_t cut = maximum - 3;
      while (cut > 0 && ((unsigned char) utf8[cut] & 0xC0) == 0x80)
         cut--;
      return utf8.substr(0, cut) + "...";
   }
}
