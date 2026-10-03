// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <boost/asio.hpp>

namespace HM
{
   /*
      Represents an IP address.

      If the second address is zero, that means that it's
      an IPv4 address.

   */
   class IPAddress
   {
   public:
      IPAddress(void);
      ~IPAddress(void);
      
      IPAddress(boost::asio::ip::address address);
      
      IPAddress(__int64 address1, __int64 address2);
      IPAddress(__int64 address1);

      bool operator==(const IPAddress &other);
      bool operator!=(const IPAddress &other);
      bool operator>(const IPAddress &other);
      bool operator<(const IPAddress &other);

      bool TryParse(const AnsiString &string);
      bool TryParse(const AnsiString &string, bool reportOnFail);

      bool IsAny() const;

      // 127.0.0.0/8 and ::1, and an IPv4 loopback address in its IPv6-mapped
      // form, which a dual-stack listener may hand over as ::ffff:127.0.0.1: a
      // connection from this computer itself.
      bool IsLoopback() const;

      enum Type
      {
         Unknown = 0,
         IPV4 = 1,
         IPV6 = 2
      };

      AnsiString ToString() const;
      AnsiString ToLongString() const;

      
      Type GetType() const;

      unsigned __int64 GetAddress1() const;
      unsigned __int64 GetAddress2() const;

      boost::asio::ip::address GetAddress() const {return address_; }     


      bool WithinRange(const IPAddress &lower, const IPAddress &upper) const;

      static bool IsValid(const AnsiString &address);

      // The address a text is, read the same way on every platform: four decimal
      // parts for IPv4, RFC 4291's text for IPv6 with a zone after a '%' if it
      // has one, and nothing around either - no brackets, no port, none of the
      // older IPv4 forms. Boost's make_address reads text with the platform's
      // own parser, which on Windows reads all of those as well (IPAddress.cpp
      // says which), so an address in text is read with this and never with
      // make_address (build/check-address-parsing.py holds the tree to that).
      static bool Parse(const AnsiString &text, boost::asio::ip::address &address);

   private:

      void SetIPV4Address_(__int64 address1);
      void SetIPV6Address_(__int64 address1, __int64 address2);

      boost::asio::ip::address address_;

   };

   class IPAddressTester
   {
   public:
      void Test();
   };
}