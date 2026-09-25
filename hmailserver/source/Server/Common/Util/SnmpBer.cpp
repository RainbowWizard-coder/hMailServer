// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The BER the SNMP agent reads and writes. See SnmpBer.h.

#include "StdAfx.h"

#include "SnmpBer.h"

#include <cstring>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   const int SnmpBerReader::MaxDepth;
   const size_t SnmpBer::MaxOidLength;

   SnmpBerReader::SnmpBerReader() :
      data_(nullptr),
      length_(0),
      position_(0),
      depth_(0)
   {
   }

   SnmpBerReader::SnmpBerReader(const unsigned char *data, size_t length) :
      data_(data),
      length_(data ? length : 0),
      position_(0),
      depth_(0)
   {
   }

   SnmpBerReader::SnmpBerReader(const unsigned char *data, size_t length, int depth) :
      data_(data),
      length_(data ? length : 0),
      position_(0),
      depth_(depth)
   {
   }

   bool
   SnmpBerReader::Next(SnmpTlv &tlv)
   {
      if (!data_ || position_ >= length_)
         return false;

      const size_t remaining = length_ - position_;
      const unsigned char *element = data_ + position_;

      // Two octets at the least: a tag and a length.
      if (remaining < 2)
         return false;

      const unsigned char tag = element[0];

      // A tag number of 31 or more takes further octets. Nothing in SNMP has one,
      // so one is a malformed packet rather than something to decode.
      if ((tag & 0x1F) == 0x1F)
         return false;

      const unsigned char first = element[1];

      size_t header = 2;
      size_t length = 0;

      if (first < 0x80)
      {
         length = first;
      }
      else
      {
         // 0x80 alone is the indefinite form, which SNMP does not use (RFC 3417
         // section 8); more than four length octets would describe an element
         // larger than any datagram.
         const size_t count = (size_t) (first & 0x7F);

         if (count == 0 || count > 4)
            return false;

         if (remaining < 2 + count)
            return false;

         for (size_t i = 0; i < count; i++)
            length = (length << 8) | (size_t) element[2 + i];

         header = 2 + count;
      }

      // The contents must fit in what remains, checked before anything reads them.
      if (length > remaining - header)
         return false;

      tlv.tag = tag;
      tlv.start = element;
      tlv.total = header + length;
      tlv.value = element + header;
      tlv.length = length;

      position_ += header + length;
      return true;
   }

   bool
   SnmpBerReader::Next(unsigned char expectedTag, SnmpTlv &tlv)
   {
      const size_t before = position_;

      SnmpTlv candidate;
      if (!Next(candidate))
         return false;

      if (candidate.tag != expectedTag)
      {
         position_ = before;
         return false;
      }

      tlv = candidate;
      return true;
   }

   bool
   SnmpBerReader::NextInteger(long long &value)
   {
      const size_t before = position_;

      SnmpTlv tlv;
      if (!Next(SnmpTagInteger, tlv))
         return false;

      if (!SnmpBer::DecodeInteger(tlv.value, tlv.length, value))
      {
         position_ = before;
         return false;
      }

      return true;
   }

   bool
   SnmpBerReader::NextOid(SnmpOid &oid)
   {
      const size_t before = position_;

      SnmpTlv tlv;
      if (!Next(SnmpTagObjectIdentifier, tlv))
         return false;

      if (!SnmpBer::DecodeOid(tlv.value, tlv.length, oid))
      {
         position_ = before;
         return false;
      }

      return true;
   }

   bool
   SnmpBerReader::Enter(const SnmpTlv &tlv, SnmpBerReader &child) const
   {
      if (depth_ + 1 > MaxDepth)
         return false;

      if (!tlv.value && tlv.length > 0)
         return false;

      child = SnmpBerReader(tlv.value, tlv.length, depth_ + 1);
      return true;
   }

   bool
   SnmpBer::DecodeInteger(const unsigned char *value, size_t length, long long &result)
   {
      // Every INTEGER an SNMP message carries fits in 32 bits (a 32-bit value
      // with its top bit set takes a fifth, leading zero octet); eight is room to
      // spare and the most a long long holds.
      if (!value || length == 0 || length > 8)
         return false;

      unsigned long long accumulated = (value[0] & 0x80) ? ~0ULL : 0ULL;

      for (size_t i = 0; i < length; i++)
         accumulated = (accumulated << 8) | (unsigned long long) value[i];

      result = (long long) accumulated;
      return true;
   }

   bool
   SnmpBer::DecodeOid(const unsigned char *value, size_t length, SnmpOid &oid)
   {
      oid.clear();

      if (!value || length == 0)
         return false;

      // No more contents than 128 sub-identifiers of five octets could take.
      if (length > MaxOidLength * 5)
         return false;

      unsigned long long current = 0;
      size_t octetsInCurrent = 0;
      bool first = true;

      for (size_t i = 0; i < length; i++)
      {
         const unsigned char octet = value[i];

         current = (current << 7) | (unsigned long long) (octet & 0x7F);
         octetsInCurrent++;

         // Five octets carry 35 bits; a sub-identifier past 2^32-1 is refused,
         // and so is the sixth octet of one that has not ended.
         if (octetsInCurrent > 5)
            return false;

         if (octet & 0x80)
            continue;

         if (first)
         {
            // The first octet group carries the first two arcs: 40 * X + Y, with X
            // 0, 1 or 2 and Y below 40 unless X is 2.
            if (current < 40)
            {
               oid.push_back(0);
               oid.push_back((unsigned int) current);
            }
            else if (current < 80)
            {
               oid.push_back(1);
               oid.push_back((unsigned int) (current - 40));
            }
            else
            {
               if (current - 80 > 0xFFFFFFFFULL)
                  return false;

               oid.push_back(2);
               oid.push_back((unsigned int) (current - 80));
            }

            first = false;
         }
         else
         {
            if (current > 0xFFFFFFFFULL)
               return false;

            oid.push_back((unsigned int) current);
         }

         if (oid.size() > MaxOidLength)
            return false;

         current = 0;
         octetsInCurrent = 0;
      }

      // The last octet had its continuation bit set: a sub-identifier that never
      // ended.
      if (octetsInCurrent != 0)
         return false;

      return !oid.empty();
   }

   size_t
   SnmpBer::HeaderLength(size_t contentLength)
   {
      if (contentLength < 0x80)
         return 2;

      size_t octets = 0;
      size_t remaining = contentLength;

      while (remaining > 0)
      {
         octets++;
         remaining >>= 8;
      }

      return 2 + octets;
   }

   void
   SnmpBer::AppendLength(SnmpBytes &out, size_t length)
   {
      if (length < 0x80)
      {
         out.push_back((unsigned char) length);
         return;
      }

      unsigned char octets[sizeof(size_t)];
      int count = 0;
      size_t remaining = length;

      while (remaining > 0 && count < (int) sizeof(size_t))
      {
         octets[count++] = (unsigned char) (remaining & 0xFF);
         remaining >>= 8;
      }

      out.push_back((unsigned char) (0x80 | count));

      while (count > 0)
         out.push_back(octets[--count]);
   }

   void
   SnmpBer::Append(SnmpBytes &out, const SnmpBytes &more)
   {
      out.insert(out.end(), more.begin(), more.end());
   }

   void
   SnmpBer::AppendTlv(SnmpBytes &out, unsigned char tag, const unsigned char *content, size_t length)
   {
      out.push_back(tag);
      AppendLength(out, length);

      if (content && length > 0)
         out.insert(out.end(), content, content + length);
   }

   SnmpBytes
   SnmpBer::Wrap(unsigned char tag, const SnmpBytes &content)
   {
      SnmpBytes out;
      out.reserve(content.size() + 6);
      AppendTlv(out, tag, content.empty() ? nullptr : &content[0], content.size());
      return out;
   }

   SnmpBytes
   SnmpBer::EncodeInteger(long long value)
   {
      // Two's complement, big-endian, in the fewest octets: a leading 0x00 is
      // dropped while the next octet's top bit is clear, and a leading 0xFF while
      // it is set.
      const unsigned long long bits = (unsigned long long) value;

      unsigned char octets[8];
      for (int i = 0; i < 8; i++)
         octets[i] = (unsigned char) ((bits >> (8 * (7 - i))) & 0xFF);

      int startAt = 0;
      while (startAt < 7)
      {
         const bool redundantZero = octets[startAt] == 0x00 && (octets[startAt + 1] & 0x80) == 0;
         const bool redundantOnes = octets[startAt] == 0xFF && (octets[startAt + 1] & 0x80) != 0;

         if (!redundantZero && !redundantOnes)
            break;

         startAt++;
      }

      SnmpBytes out;
      AppendTlv(out, SnmpTagInteger, octets + startAt, (size_t) (8 - startAt));
      return out;
   }

   SnmpBytes
   SnmpBer::EncodeUnsigned(unsigned char tag, unsigned long long value)
   {
      // The application types are unsigned but encoded as INTEGERs are: an
      // octet of zeros goes in front of a value whose top bit is set, or it would
      // read as negative. A Counter64 at its largest takes nine octets.
      unsigned char octets[9];
      octets[0] = 0;
      for (int i = 0; i < 8; i++)
         octets[1 + i] = (unsigned char) ((value >> (8 * (7 - i))) & 0xFF);

      int startAt = 1;
      while (startAt < 8 && octets[startAt] == 0x00)
         startAt++;

      if (octets[startAt] & 0x80)
         startAt--;

      SnmpBytes out;
      AppendTlv(out, tag, octets + startAt, (size_t) (9 - startAt));
      return out;
   }

   SnmpBytes
   SnmpBer::EncodeOctets(const SnmpBytes &value)
   {
      return Wrap(SnmpTagOctetString, value);
   }

   SnmpBytes
   SnmpBer::EncodeOctets(const std::string &value)
   {
      SnmpBytes out;
      AppendTlv(out, SnmpTagOctetString, (const unsigned char *) value.data(), value.size());
      return out;
   }

   SnmpBytes
   SnmpBer::EncodeOctets(const unsigned char *value, size_t length)
   {
      SnmpBytes out;
      AppendTlv(out, SnmpTagOctetString, value, length);
      return out;
   }

   SnmpBytes
   SnmpBer::EncodeNull(unsigned char tag)
   {
      SnmpBytes out;
      out.push_back(tag);
      out.push_back(0);
      return out;
   }

   SnmpBytes
   SnmpBer::EncodeOid(const SnmpOid &oid)
   {
      SnmpBytes content;

      // Every OID this agent writes has at least two arcs; a shorter one is
      // written as 0.0 rather than as nothing, which no decoder accepts.
      unsigned long long firstTwo = 0;
      if (oid.size() >= 2)
         firstTwo = (unsigned long long) oid[0] * 40ULL + (unsigned long long) oid[1];

      std::vector<unsigned long long> arcs;
      arcs.push_back(firstTwo);

      for (size_t i = 2; i < oid.size(); i++)
         arcs.push_back((unsigned long long) oid[i]);

      for (size_t i = 0; i < arcs.size(); i++)
      {
         unsigned char groups[10];
         int count = 0;
         unsigned long long remaining = arcs[i];

         do
         {
            groups[count++] = (unsigned char) (remaining & 0x7F);
            remaining >>= 7;
         }
         while (remaining > 0 && count < 10);

         while (count > 0)
         {
            count--;
            content.push_back((unsigned char) (groups[count] | (count > 0 ? 0x80 : 0x00)));
         }
      }

      return Wrap(SnmpTagObjectIdentifier, content);
   }

   int
   SnmpBer::Compare(const SnmpOid &left, const SnmpOid &right)
   {
      const size_t shorter = left.size() < right.size() ? left.size() : right.size();

      for (size_t i = 0; i < shorter; i++)
      {
         if (left[i] < right[i])
            return -1;

         if (left[i] > right[i])
            return 1;
      }

      if (left.size() == right.size())
         return 0;

      return left.size() < right.size() ? -1 : 1;
   }

   bool
   SnmpBer::StartsWith(const SnmpOid &oid, const SnmpOid &prefix)
   {
      if (prefix.size() > oid.size())
         return false;

      for (size_t i = 0; i < prefix.size(); i++)
      {
         if (oid[i] != prefix[i])
            return false;
      }

      return true;
   }

   bool
   SnmpBer::ParseOid(const std::string &dotted, SnmpOid &oid)
   {
      oid.clear();

      size_t position = 0;
      if (!dotted.empty() && dotted[0] == '.')
         position = 1;

      if (position >= dotted.size())
         return false;

      while (position <= dotted.size())
      {
         unsigned long long arc = 0;
         size_t digits = 0;

         while (position < dotted.size() && dotted[position] >= '0' && dotted[position] <= '9')
         {
            arc = arc * 10ULL + (unsigned long long) (dotted[position] - '0');
            digits++;
            position++;

            if (arc > 0xFFFFFFFFULL)
               return false;
         }

         if (digits == 0)
            return false;

         oid.push_back((unsigned int) arc);

         if (oid.size() > MaxOidLength)
            return false;

         if (position == dotted.size())
            break;

         if (dotted[position] != '.')
            return false;

         position++;

         // A trailing dot names no arc.
         if (position == dotted.size())
            return false;
      }

      return oid.size() >= 2;
   }

   SnmpOid
   SnmpBer::Oid(const char *dotted)
   {
      SnmpOid oid;
      ParseOid(std::string(dotted ? dotted : ""), oid);
      return oid;
   }

   std::string
   SnmpBer::OidToString(const SnmpOid &oid)
   {
      std::string text;

      for (size_t i = 0; i < oid.size(); i++)
      {
         if (i > 0)
            text += ".";

         text += std::to_string(oid[i]);
      }

      return text;
   }

   std::string
   SnmpBer::ToHex(const SnmpBytes &bytes)
   {
      static const char digits[] = "0123456789abcdef";

      std::string text;
      text.reserve(bytes.size() * 2);

      for (size_t i = 0; i < bytes.size(); i++)
      {
         text += digits[(bytes[i] >> 4) & 0x0F];
         text += digits[bytes[i] & 0x0F];
      }

      return text;
   }

   bool
   SnmpBer::FromHex(const std::string &hex, SnmpBytes &bytes)
   {
      bytes.clear();

      if (hex.size() % 2 != 0)
         return false;

      for (size_t i = 0; i < hex.size(); i += 2)
      {
         int value = 0;

         for (size_t j = 0; j < 2; j++)
         {
            const char c = hex[i + j];
            int nibble = 0;

            if (c >= '0' && c <= '9')
               nibble = c - '0';
            else if (c >= 'a' && c <= 'f')
               nibble = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F')
               nibble = c - 'A' + 10;
            else
               return false;

            value = value * 16 + nibble;
         }

         bytes.push_back((unsigned char) value);
      }

      return true;
   }
}
