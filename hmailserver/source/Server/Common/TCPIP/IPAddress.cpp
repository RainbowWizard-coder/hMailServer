// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"
#include "./IPAddress.h"
#include "../Util/Assert.h"

#include <cstring>
#include <stdexcept>
#include <string>

#ifdef HM_PLATFORM_POSIX
#include <net/if.h>
#else
#include <iphlpapi.h>
#endif

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   // What is an address here is decided by this file, the same on every platform,
   // and never by the platform's own parser. Boost.Asio's make_address hands the
   // text to inet_pton on Linux and to WSAStringToAddress on Windows, and the two
   // do not agree: WSAStringToAddress also reads the forms inet_aton has read
   // since 4.2BSD - 192.0.2 as 192.0.0.2, 3232235777 as 192.168.1.1, 0x7f.1 as
   // 127.0.0.1, 0300.0250.0.1 as 192.168.0.1 and 010.0.0.1 as 8.0.0.1, its parts
   // octal for a leading zero - and an address with a port after it
   // (192.0.2.7:25, [::1]:25) or in brackets ([2001:db8::7]). So on Windows alone a
   // HELO of 192.0.2 was an address, an IP range typed as 010.0.0.1 started at
   // 8.0.0.1, and a name the never-ban list or a route held could be read as an
   // address the same text was not on Linux. Measured on both on 2 October 2026,
   // with WSAStringToAddressW on Windows 11 and glibc 2.43's inet_pton.
   namespace
   {
      // The dotted-decimal form of an IPv4 address and nothing else: four parts of
      // one to three decimal digits, none past 255 and none with a leading zero.
      // It is what inet_pton reads (glibc's and Windows' InetPton alike), and what
      // an address is in an SMTP address literal (RFC 5321, Snum) and in a URI
      // (RFC 3986, dec-octet).
      bool ParseIPv4_(const char *text, size_t length, unsigned char bytes[4])
      {
         size_t position = 0;

         for (int part = 0; part < 4; part++)
         {
            if (part > 0)
            {
               if (position >= length || text[position] != '.')
                  return false;

               position++;
            }

            const size_t start = position;
            unsigned int value = 0;

            while (position < length && position - start < 3 && text[position] >= '0' && text[position] <= '9')
            {
               value = value * 10 + (unsigned int) (text[position] - '0');
               position++;
            }

            const size_t digits = position - start;
            if (digits == 0 || value > 255 || (digits > 1 && text[start] == '0'))
               return false;

            bytes[part] = (unsigned char) value;
         }

         return position == length;
      }

      int HexDigit_(char c)
      {
         if (c >= '0' && c <= '9')
            return c - '0';
         if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
         if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;

         return -1;
      }

      // An IPv6 address as RFC 4291 section 2.2 writes one: eight groups of one to
      // four hexadecimal digits; one run of zero groups, of one group or more,
      // written once as ::; and the last two groups as an IPv4 address in the form
      // above. Nothing around it - no brackets and no port, which are the text an
      // address sits in, not the address.
      bool ParseIPv6_(const char *text, size_t length, unsigned char bytes[16])
      {
         unsigned char head[16];
         unsigned char tail[16];
         size_t headCount = 0;
         size_t tailCount = 0;
         bool compressed = false;
         size_t position = 0;

         if (length >= 2 && text[0] == ':' && text[1] == ':')
         {
            compressed = true;
            position = 2;
         }
         else if (length >= 1 && text[0] == ':')
            return false;

         while (position < length)
         {
            unsigned char *out = compressed ? tail : head;
            size_t &count = compressed ? tailCount : headCount;

            // A group with a dot in it is the IPv4 address that ends the text.
            size_t end = position;
            bool dotted = false;
            while (end < length && text[end] != ':')
            {
               if (text[end] == '.')
                  dotted = true;
               end++;
            }

            if (dotted)
            {
               if (end != length || headCount + tailCount + 4 > 16)
                  return false;

               if (!ParseIPv4_(text + position, length - position, out + count))
                  return false;

               count += 4;
               position = length;
               break;
            }

            const size_t start = position;
            unsigned int value = 0;

            while (position < length && position - start < 4 && HexDigit_(text[position]) >= 0)
            {
               value = value * 16 + (unsigned int) HexDigit_(text[position]);
               position++;
            }

            if (position == start || (position < length && text[position] != ':'))
               return false;

            if (headCount + tailCount + 2 > 16)
               return false;

            out[count++] = (unsigned char) (value >> 8);
            out[count++] = (unsigned char) (value & 0xFF);

            if (position == length)
               break;

            // A colon, then another group, or a second colon for the run of zeros.
            position++;

            if (position < length && text[position] == ':')
            {
               if (compressed)
                  return false;

               compressed = true;
               position++;
            }
            else if (position == length)
               return false;
         }

         const size_t total = headCount + tailCount;

         if (compressed ? total > 14 : total != 16)
            return false;

         memset(bytes, 0, 16);
         memcpy(bytes, head, headCount);
         memcpy(bytes + 16 - tailCount, tail, tailCount);
         return true;
      }

      // The zone after an address's '%' (RFC 4007 section 11): its index as a
      // number, or an interface's name, which is what Linux writes for a link-local
      // peer (fe80::1%eth0) and is looked up as Boost looked it up there - an
      // interface that does not exist is zone 0.
      bool ParseZone_(const char *text, size_t length, unsigned long &zone)
      {
         if (length == 0 || length > 64)
            return false;

         bool numeric = true;

         for (size_t i = 0; i < length; i++)
         {
            const char c = text[i];

            if (c >= '0' && c <= '9')
               continue;

            numeric = false;

            const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
            if (!letter && c != '.' && c != '-' && c != '_')
               return false;
         }

         if (numeric)
         {
            unsigned long long value = 0;

            for (size_t i = 0; i < length; i++)
            {
               value = value * 10 + (unsigned long long) (text[i] - '0');
               if (value > 0xFFFFFFFFULL)
                  return false;
            }

            zone = (unsigned long) value;
            return true;
         }

         zone = (unsigned long) if_nametoindex(std::string(text, length).c_str());
         return true;
      }
   }

   bool
   IPAddress::Parse(const AnsiString &text, boost::asio::ip::address &address)
   {
      const char *characters = text.c_str();
      const size_t length = (size_t) text.GetLength();

      if (memchr(characters, ':', length) == nullptr)
      {
         boost::asio::ip::address_v4::bytes_type bytes;
         if (!ParseIPv4_(characters, length, bytes.data()))
            return false;

         address = boost::asio::ip::address_v4(bytes);
         return true;
      }

      const char *percent = (const char *) memchr(characters, '%', length);
      const size_t addressLength = percent ? (size_t) (percent - characters) : length;

      unsigned long zone = 0;
      if (percent && !ParseZone_(percent + 1, length - addressLength - 1, zone))
         return false;

      boost::asio::ip::address_v6::bytes_type bytes;
      if (!ParseIPv6_(characters, addressLength, bytes.data()))
         return false;

      address = boost::asio::ip::address_v6(bytes, (boost::asio::ip::scope_id_type) zone);
      return true;
   }

   IPAddress::IPAddress(void)
   {
    
   }

   IPAddress::~IPAddress(void)
   {

   }

   bool
   IPAddress::operator!=(const IPAddress &other)
   {
      return !(*this == other);
   }

   bool
   IPAddress::operator==(const IPAddress &other)
   {
      return (other.GetAddress1() == GetAddress1() &&
              other.GetAddress2() == GetAddress2());

   }

   bool
   IPAddress::operator>(const IPAddress &other)
   {
      bool inputIsSmaller =
         GetAddress1() > other.GetAddress1() ||
         (other.GetAddress1() == GetAddress1() && GetAddress2() > other.GetAddress2());

      return inputIsSmaller;
   }

   bool
   IPAddress::operator<(const IPAddress &other)
   {
      bool inputIsGreater =
         other.GetAddress1() > GetAddress1() ||
         (other.GetAddress1() == GetAddress1() && other.GetAddress2() > GetAddress2());

      return inputIsGreater;
   }

   IPAddress::IPAddress(boost::asio::ip::address address)
   {
      address_ = address;
   }

   IPAddress::IPAddress(__int64 address1)
   {
      SetIPV4Address_(address1);
   }

   IPAddress::IPAddress(__int64 address1, __int64 address2)
   {
      SetIPV6Address_(address1, address2);
   }

   bool
   IPAddress::TryParse(const AnsiString &addressString)
   {
      return TryParse(addressString, true);
   }

   bool
   IPAddress::TryParse(const AnsiString &addressString, bool reportOnFail)
   {
      boost::asio::ip::address parsed;

      if (!Parse(addressString, parsed))
      {
         // What Boost's make_address_v6 and make_address_v4 left behind when they
         // refused the text, which callers have always been given: the unspecified
         // address of the family the text looked like.
         if (addressString.Find(":") >= 0)
            address_ = boost::asio::ip::address_v6();
         else
            address_ = boost::asio::ip::address_v4();

         if (reportOnFail)
         {
            // Parsing of the TCP/IP address failed. Report error to debug log.
            String formattedMessage;
            formattedMessage.Format(_T("Unable to parse TCP/IP address. Address: %s"), String(addressString).c_str());
            LOG_DEBUG(formattedMessage);
         }

         return false;
      }

      address_ = parsed;
      return true;
   }

   void 
   IPAddress::SetIPV4Address_(__int64 address1)
   {
      unsigned char i1 = (address1 >> 24) & 0xFF;
      unsigned char i2 = (address1 >> 16) & 0xFF;
      unsigned char i3 = (address1 >> 8) & 0xFF;
      unsigned char i4 = address1 & 0xFF;

      boost::asio::ip::address_v4::bytes_type bytes_value = { { i1, i2, i3, i4 } };

      address_ = boost::asio::ip::address_v4(bytes_value);
   }

   void 
   IPAddress::SetIPV6Address_(__int64 address1, __int64 address2)
   {
      unsigned char i1 = (address1 >> 56) & 0xFF;
      unsigned char  i2 = (address1 >> 48) & 0xFF;
      unsigned char  i3 = (address1 >> 40) & 0xFF;
      unsigned char  i4 = (address1 >> 32) & 0xFF;
      unsigned char  i5 = (address1 >> 24) & 0xFF;
      unsigned char  i6 = (address1 >> 16) & 0xFF;
      unsigned char  i7 = (address1 >> 8) & 0xFF;
      unsigned char  i8 = address1 & 0xFF;

      unsigned char  b1 = (address2 >> 56) & 0xFF;
      unsigned char  b2 = (address2 >> 48) & 0xFF;
      unsigned char  b3 = (address2 >> 40) & 0xFF;
      unsigned char  b4 = (address2 >> 32) & 0xFF;
      unsigned char  b5 = (address2 >> 24) & 0xFF;
      unsigned char  b6 = (address2 >> 16) & 0xFF;
      unsigned char  b7 = (address2 >> 8) & 0xFF;
      unsigned char  b8 = address2 & 0xFF;

      boost::asio::ip::address_v6::bytes_type bytes_value = { { i1, i2, i3, i4, i5, i6, i7, i8, b1, b2, b3, b4, b5, b6, b7, b8 } };

      address_ = boost::asio::ip::address_v6(bytes_value);

   }

   IPAddress::Type 
   IPAddress::GetType() const
   {
      if (address_.is_v4())
         return IPAddress::IPV4;
      else if (address_.is_v6())
         return IPAddress::IPV6;
      else
      {
         HM_ASSERT(0);
         return IPAddress::Unknown;
      }
   }
   
   AnsiString 
   IPAddress::ToString() const
   {
      try
      {
         return address_.to_string();
      }
      catch (boost::system::system_error&)
      {
         return "";
      }
   }

   AnsiString
   IPAddress::ToLongString() const
   {
      if (GetType() == IPV4)
         return ToString();

      const int buffer_length = 40;

      AnsiString result;
      char *buffer = result.GetBuffer(40);

      int pos = 0;

      std::array<unsigned char, 16U> bytes = address_.to_v6().to_bytes();

      for (unsigned int source_position = 1; source_position <= bytes.size(); source_position++)
      {
         auto byte = bytes[source_position-1];

         char* position = buffer + pos;
         int remaining = buffer_length - pos;

#ifdef HM_PLATFORM_POSIX
         // snprintf IS _snprintf_s with _TRUNCATE: both write at most the given
         // number of bytes, both terminate what they write, and both stop rather
         // than fail when the text does not fit. The difference is only in what
         // they return on truncation, and nothing here reads the return value.
         ::snprintf(position, (size_t) remaining, "%02x", byte);
#else
         _snprintf_s(position, remaining, _TRUNCATE, "%02x", byte);
#endif
         pos += 2;

         if (source_position % 2 == 0 && source_position < bytes.size())
         {
            position = buffer + pos;
            remaining = buffer_length - pos;

#ifdef HM_PLATFORM_POSIX
            // See above: snprintf is _snprintf_s with _TRUNCATE.
            ::snprintf(position, (size_t) remaining, ":");
#else
            _snprintf_s(position, remaining, _TRUNCATE, ":");
#endif
            pos++;
         }
      }

      result.ReleaseBuffer();
      return result;
   }

   unsigned __int64 
   IPAddress::GetAddress1() const
   {
      if (address_.is_v4())
      {
         boost::asio::ip::address_v4::bytes_type bytes_value = address_.to_v4().to_bytes();

         __int64 ret = 0xFFFFFFFF & ((bytes_value[0] << 24) | (bytes_value[1] << 16) | (bytes_value[2] << 8) | bytes_value[3]);

         return ret;
      }
      else if (address_.is_v6())
      {
         boost::asio::ip::address_v6::bytes_type bytes_value = address_.to_v6().to_bytes();
         
         unsigned __int64 ret = 0xFFFFFFFFFFFFFFFF & (__int64) bytes_value[0] << 56;
         ret |=  (__int64) bytes_value[1] << 48;
         ret |=  (__int64) bytes_value[2] << 40;
         ret |=  (__int64) bytes_value[3] << 32;
         ret |=  (__int64) bytes_value[4] << 24;
         ret |=  (__int64) bytes_value[5] << 16;
         ret |=  (__int64) bytes_value[6] << 8;
         ret |=  (__int64) bytes_value[7] << 0;

         return ret;
      }
      else
         return 0;
   }
   
   unsigned __int64 
   IPAddress::GetAddress2() const
   {
      if (address_.is_v4())
         return 0;
      else if (address_.is_v6())
      {
         boost::asio::ip::address_v6::bytes_type bytes_value = address_.to_v6().to_bytes();

         unsigned __int64 ret = 0xFFFFFFFFFFFFFFFF & (__int64) bytes_value[8] << 56;
         ret |=  (__int64) bytes_value[9] << 48;
         ret |=  (__int64) bytes_value[10] << 40;
         ret |=  (__int64) bytes_value[11] << 32;
         ret |=  (__int64) bytes_value[12] << 24;
         ret |=  (__int64) bytes_value[13] << 16;
         ret |=  (__int64) bytes_value[14] << 8;
         ret |=  (__int64) bytes_value[15] << 0;

         return ret;
      }
      else 
         return 0;
   }

   bool 
   IPAddress::IsAny() const
   {
      if (address_.is_v4())
      {
         if (address_.to_v4().to_bytes() == boost::asio::ip::address_v4::any().to_bytes())
         {
            return true;
         }
      }
      else if (address_.is_v6())
      {
         if (address_.to_v6().to_bytes() == boost::asio::ip::address_v6::any().to_bytes())
         {
            return true;
         }
      }
      
      return false;
   }

   bool 
   IPAddress::WithinRange(const IPAddress &lower, const IPAddress &upper) const
   {
#ifdef _DEBUG
      String lower1Str = lower.ToString();
      String lower2Str = upper.ToString();
      String thisIP = ToString();
#endif

      unsigned __int64 addressToCheck1 = GetAddress1();
      unsigned __int64 addressToCheck2 = GetAddress2();
      
      // If it's an IPv4 address, we don't need to compare the second part.
      if (address_.is_v4())
      {
         if (addressToCheck1 >= lower.GetAddress1() && addressToCheck1 <= upper.GetAddress1())
            return true;
      }
      else if (address_.is_v6())
      {
         // Check that it's same or higher as the lower bound.
         if (addressToCheck1 >  lower.GetAddress1() || 
            (addressToCheck1 == lower.GetAddress1() && addressToCheck2 >= lower.GetAddress2()))
         {
            // Check that it's same or less than upper bound.
            if (addressToCheck1 <  upper.GetAddress1() || 
               (addressToCheck1 == upper.GetAddress1() && addressToCheck2 <= upper.GetAddress2()))
            {
               return true;
            }
         }
      }

      return false   ;
   }

   bool
   IPAddress::IsValid(const AnsiString &address)
   {
      boost::asio::ip::address parsed;
      return Parse(address, parsed);
   }

   namespace
   {
      void ExpectAddress_(const AnsiString &text, unsigned __int64 high, unsigned __int64 low, unsigned long zone = 0)
      {
         IPAddress address;
         if (!address.TryParse(text, false) || !IPAddress::IsValid(text))
            throw std::logic_error(std::string("IPAddress: \"") + text.c_str() + "\" was refused, and it is an address.");

         if (address.GetAddress1() != high || address.GetAddress2() != low)
            throw std::logic_error(std::string("IPAddress: \"") + text.c_str() + "\" was read as " + address.ToString().c_str() + ".");

         boost::asio::ip::address parsed;
         if (!IPAddress::Parse(text, parsed) || parsed != address.GetAddress())
            throw std::logic_error(std::string("IPAddress::Parse: \"") + text.c_str() + "\" was not read as TryParse read it.");

         if (address.GetType() == IPAddress::IPV6 && address.GetAddress().to_v6().scope_id() != zone)
            throw std::logic_error(std::string("IPAddress: \"") + text.c_str() + "\" was read with zone " +
                                   std::to_string(address.GetAddress().to_v6().scope_id()) + ", not " + std::to_string(zone) + ".");
      }

      void ExpectRefused_(const AnsiString &text)
      {
         IPAddress address;
         if (address.TryParse(text, false))
            throw std::logic_error(std::string("IPAddress::TryParse took \"") + text.c_str() + "\", which is not an address, as " +
                                   address.ToString().c_str() + ".");

         if (IPAddress::IsValid(text))
            throw std::logic_error(std::string("IPAddress::IsValid took \"") + text.c_str() + "\", which is not an address.");

         boost::asio::ip::address parsed;
         if (IPAddress::Parse(text, parsed))
            throw std::logic_error(std::string("IPAddress::Parse took \"") + text.c_str() + "\", which is not an address.");
      }

      void TestTheSameOnEveryPlatform_()
      {
         // The forms both platforms' inet_pton read, read here the same way.
         ExpectAddress_("192.0.2.7", 0xC0000207ULL, 0);
         ExpectAddress_("0.0.0.0", 0, 0);
         ExpectAddress_("255.255.255.255", 0xFFFFFFFFULL, 0);
         ExpectAddress_("10.0.0.1", 0x0A000001ULL, 0);
         ExpectAddress_("::", 0, 0);
         ExpectAddress_("::1", 0, 1);
         ExpectAddress_("2001:db8::7", 0x20010DB800000000ULL, 7);
         ExpectAddress_("2001:DB8::7", 0x20010DB800000000ULL, 7);
         ExpectAddress_("1:2:3:4:5:6:7:8", 0x0001000200030004ULL, 0x0005000600070008ULL);
         ExpectAddress_("1:2:3:4:5:6:7::", 0x0001000200030004ULL, 0x0005000600070000ULL);
         ExpectAddress_("::1:2:3:4:5:6:7", 0x0000000100020003ULL, 0x0004000500060007ULL);
         ExpectAddress_("0001::", 0x0001000000000000ULL, 0);
         ExpectAddress_("::ffff:192.0.2.7", 0, 0x0000FFFFC0000207ULL);
         ExpectAddress_("::1.2.3.4", 0, 0x0000000001020304ULL);
         ExpectAddress_("1:2:3:4:5:6:1.2.3.4", 0x0001000200030004ULL, 0x0005000601020304ULL);
         ExpectAddress_("1:2:3:4:5::1.2.3.4", 0x0001000200030004ULL, 0x0005000001020304ULL);

         // A zone: a number, the largest, and the name of an interface there is not.
         ExpectAddress_("fe80::1%3", 0xFE80000000000000ULL, 1, 3);
         ExpectAddress_("fe80::1%4294967295", 0xFE80000000000000ULL, 1, 4294967295UL);
         ExpectAddress_("fe80::1%no-such-if0", 0xFE80000000000000ULL, 1, 0);

         // The older IPv4 forms Windows' WSAStringToAddress reads and inet_pton does
         // not: fewer than four parts, one 32-bit number, hexadecimal, octal by a
         // leading zero (0300 is 192 and 010 is 8 there), and the largest number.
         ExpectRefused_("192.0.2");
         ExpectRefused_("192.0");
         ExpectRefused_("192");
         ExpectRefused_("127.0.0");
         ExpectRefused_("3232235777");
         ExpectRefused_("4294967295");
         ExpectRefused_("0x7f.1");
         ExpectRefused_("0x7f.0.0.1");
         ExpectRefused_("0xffffffff");
         ExpectRefused_("0XFFFFFFFF");
         ExpectRefused_("0300.0250.0.1");
         ExpectRefused_("192.168.001.001");
         ExpectRefused_("010.0.0.1");
         ExpectRefused_("1.2.3.04");
         ExpectRefused_("00.0.0.0");

         // An address with something around it: a port, brackets, spaces, a root dot.
         ExpectRefused_("192.0.2.7:25");
         ExpectRefused_("[::1]");
         ExpectRefused_("[::1]:25");
         ExpectRefused_("[2001:db8::7]");
         ExpectRefused_("[192.0.2.7]");
         ExpectRefused_(" 192.0.2.7");
         ExpectRefused_("192.0.2.7 ");
         ExpectRefused_("2001:db8::7 ");
         ExpectRefused_("192.0.2.7.");
         ExpectRefused_("IPv6:2001:db8::7");

         // And what is no address on any platform.
         ExpectRefused_("");
         ExpectRefused_(".");
         ExpectRefused_("256.0.0.1");
         ExpectRefused_("1.2.3.4x");
         ExpectRefused_("1..2.3");
         ExpectRefused_("1.2.3.4.5");
         ExpectRefused_("1234.0.0.1");
         ExpectRefused_("::ffff:1.2.3");
         ExpectRefused_("::ffff:01.2.3.4");
         ExpectRefused_("::ffff:0x7f.0.0.1");
         ExpectRefused_("::1.2.3.4.5");
         ExpectRefused_("1.2.3.4::");
         ExpectRefused_("1:2:3:4:5:6:7:1.2.3.4");
         ExpectRefused_("1:2:3:4:5:6:7:8::");
         ExpectRefused_("::1:2:3:4:5:6:7:8");
         ExpectRefused_("1:2:3:4:5:6:7:8:9");
         ExpectRefused_("1:2:3:4:5:6:7");
         ExpectRefused_(":1::");
         ExpectRefused_("1::2::3");
         ExpectRefused_(":::");
         ExpectRefused_("1::2:");
         ExpectRefused_("00001::");
         ExpectRefused_("fe80::1%");
         ExpectRefused_("fe80::1%4294967296");
         ExpectRefused_("fe80::1%eth 0");
         ExpectRefused_("fe80::1%3%4");

         // The whole text is the address: what follows a NUL is not left unread, as
         // it was by a parser handed the text's C string.
         AnsiString withNul("192.0.2.7");
         withNul += (char) 0;
         withNul += "1";
         ExpectRefused_(withNul);
      }
   }

   void
   IPAddressTester::Test()
   {
      // IPv4 parsing
      IPAddress ipv4;
      Assert::IsTrue(ipv4.TryParse("192.168.1.1", false));
      Assert::AreEqual("192.168.1.1", ipv4.ToString());
      Assert::IsTrue(ipv4.GetType() == IPAddress::IPV4);

      // IPv6 parsing (exercises make_address_v6)
      IPAddress ipv6;
      Assert::IsTrue(ipv6.TryParse("::1", false));
      Assert::AreEqual("::1", ipv6.ToString());
      Assert::IsTrue(ipv6.GetType() == IPAddress::IPV6);

      // Invalid address
      IPAddress invalid;
      Assert::IsFalse(invalid.TryParse("not-an-address", false));

      // IsValid (exercises make_address)
      Assert::IsTrue(IPAddress::IsValid("10.0.0.1"));
      Assert::IsTrue(IPAddress::IsValid("2001:db8::1"));
      Assert::IsFalse(IPAddress::IsValid("999.999.999.999"));

      // Special case: 255.255.255.255 (Windows workaround path)
      IPAddress broadcast;
      Assert::IsTrue(broadcast.TryParse("255.255.255.255", false));
      Assert::AreEqual("255.255.255.255", broadcast.ToString());

      // What is an address and what is not, one form at a time, answered the same
      // on Windows and on Linux.
      TestTheSameOnEveryPlatform_();
   }
}