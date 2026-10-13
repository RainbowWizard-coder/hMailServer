// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The hosts 6.3.6 handed over for a link's host part - SURBL.cpp's
// HostDecoded_ and HostsOfHostPart_ as they are in the v6.3.6 tag, with the
// helpers they call, kept here unchanged but for the types (std::wstring for
// HM::String, the code points of the input for a wide string, which on Linux
// are one), so that the differential fuzz (idna_ada_fuzzer.cpp, check 5) can
// hold the server to handing over every host 6.3.6 did, whatever it does
// beside: percent-decoded as UTF-8, the code points UTS 46 marks ignored
// (IdnaMappingTable.txt of Unicode 18.0.0, as 6.3.6 listed them) deleted, ASCII
// in lower case; then an address, or the name up to the first character a
// name cannot hold and the labels after the last label that holds one. Not
// the server's code, which must not be read to judge itself: a copy of what
// was released.

#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace surbl636
{
   const size_t maxNameLength = 253;
   const size_t maxLabelLength = 63;
   const unsigned int replacementCharacter = 0xFFFD;

   inline bool IsHostCharacter(wchar_t c)
   {
      return (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c == L'-' || c == L'_';
   }

   inline int HexValue(wchar_t c)
   {
      if (c >= L'0' && c <= L'9')
         return c - L'0';
      if (c >= L'a' && c <= L'f')
         return c - L'a' + 10;
      if (c >= L'A' && c <= L'F')
         return c - L'A' + 10;
      return -1;
   }

   struct CodePointRange
   {
      unsigned int first;
      unsigned int last;
   };

   // 6.3.6's ignoredInAHost: every line of IdnaMappingTable.txt 18.0.0 whose
   // status is "ignored".
   const CodePointRange ignoredInAHost[] =
   {
      { 0x00AD, 0x00AD }, { 0x034F, 0x034F }, { 0x115F, 0x1160 }, { 0x17B4, 0x17B5 }, { 0x180B, 0x180E },
      { 0x180F, 0x180F }, { 0x200B, 0x200B }, { 0x2060, 0x2063 }, { 0x2064, 0x2064 }, { 0x206A, 0x206F },
      { 0x3164, 0x3164 }, { 0xFE00, 0xFE0F }, { 0xFEFF, 0xFEFF }, { 0xFFA0, 0xFFA0 }, { 0x1BCA0, 0x1BCA3 },
      { 0x1D173, 0x1D17A }, { 0xE0100, 0xE01EF },
   };

   inline bool IsIgnoredInAHost(unsigned int codePoint)
   {
      for (const CodePointRange &range : ignoredInAHost)
         if (codePoint >= range.first && codePoint <= range.last)
            return true;
      return false;
   }

   inline void AppendUtf8(unsigned int codePoint, std::string &bytes)
   {
      if (codePoint < 0x80)
      {
         bytes += static_cast<char>(codePoint);
      }
      else if (codePoint < 0x800)
      {
         bytes += static_cast<char>(0xC0 | (codePoint >> 6));
         bytes += static_cast<char>(0x80 | (codePoint & 0x3F));
      }
      else if (codePoint < 0x10000)
      {
         bytes += static_cast<char>(0xE0 | (codePoint >> 12));
         bytes += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
         bytes += static_cast<char>(0x80 | (codePoint & 0x3F));
      }
      else
      {
         bytes += static_cast<char>(0xF0 | (codePoint >> 18));
         bytes += static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F));
         bytes += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
         bytes += static_cast<char>(0x80 | (codePoint & 0x3F));
      }
   }

   // The Encoding standard's UTF-8 decoder, as 6.3.6's DecodeUtf8_.
   inline std::vector<unsigned int> DecodeUtf8(const std::string &bytes)
   {
      std::vector<unsigned int> codePoints;
      unsigned int codePoint = 0;
      unsigned int bytesNeeded = 0;
      unsigned int bytesSeen = 0;
      unsigned int lowerBoundary = 0x80;
      unsigned int upperBoundary = 0xBF;

      size_t i = 0;
      while (i < bytes.size())
      {
         const unsigned int byte = static_cast<unsigned char>(bytes[i]);

         if (bytesNeeded == 0)
         {
            i++;
            if (byte <= 0x7F)
            {
               codePoints.push_back(byte);
            }
            else if (byte >= 0xC2 && byte <= 0xDF)
            {
               bytesNeeded = 1;
               codePoint = byte & 0x1F;
            }
            else if (byte >= 0xE0 && byte <= 0xEF)
            {
               if (byte == 0xE0)
                  lowerBoundary = 0xA0;
               if (byte == 0xED)
                  upperBoundary = 0x9F;
               bytesNeeded = 2;
               codePoint = byte & 0xF;
            }
            else if (byte >= 0xF0 && byte <= 0xF4)
            {
               if (byte == 0xF0)
                  lowerBoundary = 0x90;
               if (byte == 0xF4)
                  upperBoundary = 0x8F;
               bytesNeeded = 3;
               codePoint = byte & 0x7;
            }
            else
            {
               codePoints.push_back(replacementCharacter);
            }
            continue;
         }

         if (byte < lowerBoundary || byte > upperBoundary)
         {
            codePoint = bytesNeeded = bytesSeen = 0;
            lowerBoundary = 0x80;
            upperBoundary = 0xBF;
            codePoints.push_back(replacementCharacter);
            continue;
         }

         i++;
         lowerBoundary = 0x80;
         upperBoundary = 0xBF;
         codePoint = (codePoint << 6) | (byte & 0x3F);
         bytesSeen++;

         if (bytesSeen != bytesNeeded)
            continue;

         codePoints.push_back(codePoint);
         codePoint = bytesNeeded = bytesSeen = 0;
      }

      if (bytesNeeded != 0)
         codePoints.push_back(replacementCharacter);

      return codePoints;
   }

   // 6.3.6's PercentDecodedCodePoints_, on code points.
   inline std::vector<unsigned int> PercentDecodedCodePoints(const std::vector<unsigned int> &value)
   {
      std::string bytes;
      for (size_t i = 0; i < value.size();)
      {
         if (value[i] == '%' && i + 2 < value.size())
         {
            const int high = value[i + 1] < 0x80 ? HexValue(static_cast<wchar_t>(value[i + 1])) : -1;
            const int low = value[i + 2] < 0x80 ? HexValue(static_cast<wchar_t>(value[i + 2])) : -1;
            if (high >= 0 && low >= 0)
            {
               bytes += static_cast<char>(high * 16 + low);
               i += 3;
               continue;
            }
         }

         const unsigned int codePoint = value[i];
         AppendUtf8((codePoint >= 0xD800 && codePoint <= 0xDFFF) || codePoint > 0x10FFFF ? replacementCharacter : codePoint, bytes);
         i++;
      }

      return DecodeUtf8(bytes);
   }

   // 6.3.6's HostDecoded_.
   inline std::wstring HostDecoded(const std::vector<unsigned int> &hostPart)
   {
      std::wstring host;
      for (unsigned int codePoint : PercentDecodedCodePoints(hostPart))
      {
         if (IsIgnoredInAHost(codePoint))
            continue;
         if (codePoint >= 'A' && codePoint <= 'Z')
            codePoint += 'a' - 'A';
         host += static_cast<wchar_t>(codePoint);
      }
      return host;
   }

   inline std::vector<std::wstring> SplitOnDots(const std::wstring &value)
   {
      std::vector<std::wstring> parts;
      size_t start = 0;
      for (;;)
      {
         const size_t dot = value.find(L'.', start);
         if (dot == std::wstring::npos)
         {
            parts.push_back(value.substr(start));
            return parts;
         }
         parts.push_back(value.substr(start, dot - start));
         start = dot + 1;
      }
   }

   inline std::wstring WithoutTrailingDots(const std::wstring &value)
   {
      size_t end = value.size();
      while (end > 0 && value[end - 1] == L'.')
         end--;
      return value.substr(0, end);
   }

   inline bool ParseIPv4Number(const std::wstring &input, unsigned long long &value)
   {
      if (input.empty())
         return false;

      std::wstring digits = input;
      unsigned int radix = 10;
      if (digits.size() >= 2 && digits[0] == L'0' && (digits[1] == L'x' || digits[1] == L'X'))
      {
         digits = digits.substr(2);
         radix = 16;
      }
      else if (digits.size() >= 2 && digits[0] == L'0')
      {
         digits = digits.substr(1);
         radix = 8;
      }

      value = 0;
      for (size_t i = 0; i < digits.size(); i++)
      {
         const int digit = HexValue(digits[i]);
         if (digit < 0 || static_cast<unsigned int>(digit) >= radix)
            return false;
         if (value <= 0xFFFFFFFFULL)
            value = value * radix + static_cast<unsigned int>(digit);
      }

      if (value > 0xFFFFFFFFULL)
         value = 0x100000000ULL;
      return true;
   }

   inline bool EndsInANumber(const std::wstring &host)
   {
      std::vector<std::wstring> parts = SplitOnDots(host);
      if (parts.back().empty())
      {
         if (parts.size() == 1)
            return false;
         parts.pop_back();
      }

      const std::wstring &last = parts.back();
      if (last.empty())
         return false;

      bool digitsOnly = true;
      for (wchar_t c : last)
         if (c < L'0' || c > L'9')
            digitsOnly = false;
      if (digitsOnly)
         return true;

      unsigned long long ignored = 0;
      return last.size() >= 2 && last[0] == L'0' && (last[1] == L'x' || last[1] == L'X') && ParseIPv4Number(last, ignored);
   }

   inline bool ParseIPv4(const std::wstring &host, std::wstring &dotted)
   {
      std::vector<std::wstring> parts = SplitOnDots(host);
      if (parts.back().empty() && parts.size() > 1)
         parts.pop_back();
      if (parts.size() > 4)
         return false;

      std::vector<unsigned long long> numbers;
      for (const std::wstring &part : parts)
      {
         unsigned long long number = 0;
         if (!ParseIPv4Number(part, number))
            return false;
         numbers.push_back(number);
      }

      for (size_t i = 0; i + 1 < numbers.size(); i++)
         if (numbers[i] > 255)
            return false;

      unsigned long long limit = 1;
      for (size_t i = 0; i < 5 - numbers.size(); i++)
         limit *= 256;
      if (numbers.back() >= limit)
         return false;

      unsigned long long address = numbers.back();
      for (size_t i = 0; i + 1 < numbers.size(); i++)
      {
         unsigned long long weight = 1;
         for (size_t j = 0; j < 3 - i; j++)
            weight *= 256;
         address += numbers[i] * weight;
      }

      dotted = std::to_wstring((address >> 24) & 0xFF) + L"." + std::to_wstring((address >> 16) & 0xFF) + L"." +
               std::to_wstring((address >> 8) & 0xFF) + L"." + std::to_wstring(address & 0xFF);
      return true;
   }

   inline bool IsName(const std::wstring &name)
   {
      if (name.empty() || name.size() > maxNameLength)
         return false;

      std::vector<std::wstring> labels = SplitOnDots(name);
      if (labels.size() < 2)
         return false;

      for (const std::wstring &label : labels)
      {
         if (label.empty() || label.size() > maxLabelLength)
            return false;
         for (wchar_t c : label)
            if (!IsHostCharacter(c))
               return false;
      }
      return true;
   }

   inline std::wstring Canonical(const std::wstring &candidate)
   {
      const std::wstring name = WithoutTrailingDots(candidate);

      if (EndsInANumber(candidate))
      {
         std::wstring dotted;
         if (ParseIPv4(candidate, dotted))
            return dotted;
         return std::wstring();
      }

      if (!IsName(name))
         return std::wstring();

      if (name.size() > 4 && name.compare(0, 4, L"www.") == 0 && IsName(name.substr(4)))
         return name.substr(4);
      return name;
   }

   // 6.3.6's HostsOfHostPart_.
   inline std::vector<std::wstring> HostsOfHostPart(const std::vector<unsigned int> &hostPart)
   {
      std::vector<std::wstring> hosts;
      const std::wstring host = HostDecoded(hostPart);

      if (EndsInANumber(host))
      {
         std::wstring dotted;
         if (ParseIPv4(host, dotted))
            hosts.push_back(dotted);
         return hosts;
      }

      size_t run = 0;
      while (run < host.size() && (IsHostCharacter(host[run]) || host[run] == L'.'))
         run++;

      const std::wstring leading = Canonical(host.substr(0, run));
      if (!leading.empty())
         hosts.push_back(leading);

      if (run == host.size())
         return hosts;

      std::vector<std::wstring> labels = SplitOnDots(WithoutTrailingDots(host));
      size_t firstClean = 0;
      for (size_t i = 0; i < labels.size(); i++)
      {
         for (wchar_t c : labels[i])
         {
            if (!IsHostCharacter(c))
            {
               firstClean = i + 1;
               break;
            }
         }
      }

      if (firstClean == 0 || firstClean >= labels.size())
         return hosts;

      std::wstring suffix;
      for (size_t i = firstClean; i < labels.size(); i++)
      {
         if (!suffix.empty())
            suffix += L".";
         suffix += labels[i];
      }

      const std::wstring registered = Canonical(suffix);
      if (!registered.empty() && registered != leading)
         hosts.push_back(registered);
      return hosts;
   }
}
