// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

namespace HM
{
   // The SNMP agent's decoder and engine (SnmpBer.h, SnmpUsm.h, SnmpEngine.h),
   // from values: the BER both ways, the reader's bounds, the cryptography -
   // the key localisation on RFC 3414 A.3's password and engine ID with SHA-256
   // (a value computed here; RFC 3414 publishes it for MD5 and SHA-1 only),
   // HMAC-SHA-256-192 against Python's hmac module on the same inputs, and
   // AES-128-CFB against NIST SP 800-38A F.3.13 - SNMPv2c and SNMPv3 exchanges
   // with an engine the test makes and a clock it sets, and packets that are
   // truncated (whole, and one element at a time inside true lengths),
   // malformed, nested too deep, too long or turned over byte by byte, the
   // SNMPv3 plaintext among them signed so that the digest holds - none of
   // which may throw, be answered with anything unreadable, or go uncounted.
   //
   // On Windows it runs inside the service with the other class tests
   // (ClassTester, which Utilities.RunTestSuite reaches); on Linux it is the
   // snmp-tests program (platform/snmp_tests_main.cpp). It needs no database,
   // no setting, no socket and no network. It throws std::logic_error naming the
   // first thing that is wrong.
   class SnmpTester
   {
   public:
      void Test();

   private:
      void TestBer_();
      void TestReaderBounds_();
      void TestCryptography_();
      void TestV2c_();
      void TestV3_();
      void TestWalkAndBulk_();
      void TestMalformed_();
      void TestInnerTruncations_();
   };
}
