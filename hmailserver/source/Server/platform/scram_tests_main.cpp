// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The iteration count SCRAM-SHA-256 asks a client for (Common/Util/Hashing/
// ScramSha256.cpp, ProcessClientFirst), as a program of its own for the POSIX
// build - the counterpart of spf-tests and backup-tests. On Windows the
// regression suite's IMAP Basics TestScramSha256StoredIterationCountIsBounded
// writes the stored hash through COM's Database, which the Linux suite cannot
// reach, so on Linux this is what holds it.
//
// The count of a stored $h1$ hash is held to 1-10,000,000, the bound a password
// logon holds it to (HashCreator::ValidatePBKDF2). Up to 6.3.5 it was read with
// atoi and refused only at 0 or below, so on Linux a count past an int was its
// low 32 bits - 4294967297 asked the client for 1 round, 99999999999 for
// 1215752191 - and the stored salt was offered with it. A hash refused takes the
// unknown account's answer: i=210000, and a salt derived from the name rather
// than the stored one. No database, configuration file or network. One line per
// case, and a non-zero exit on a failure.
//
// A password logon (HashCreator::ValidatePBKDF2) reads the same field, and up to
// 6.3.5 read it with atoi too, so on Linux it took 4294967297 as 1 round and
// checked the password against a key made with one. It now reads the count as
// SCRAM does, and the last cases hold the two to refusing the same hashes.

#include <cstdio>
#include <string>

#include <openssl/evp.h>

#include "../Common/Util/Hashing/HashCreator.h"
#include "../Common/Util/Hashing/ScramSha256.h"

namespace
{
   int failures = 0;

   // One attribute of a SCRAM message: "i" of "r=...,s=...,i=4096" is "4096".
   std::string Attribute(const std::string &message, char name)
   {
      const std::string prefix = std::string(1, name) + "=";
      size_t start = 0;
      while (start <= message.size())
      {
         size_t stop = message.find(',', start);
         if (stop == std::string::npos)
            stop = message.size();
         const std::string part = message.substr(start, stop - start);
         if (part.compare(0, prefix.size(), prefix) == 0)
            return part.substr(prefix.size());
         start = stop + 1;
      }
      return std::string();
   }

   // The server-first message for a client-first message, against a stored hash.
   std::string ServerFirst(const std::string &stored)
   {
      HM::ScramSha256 session;
      HM::AnsiString serverFirst;
      if (!session.ProcessClientFirst(HM::AnsiString("n,,n=rounds@example.test,r=0123456789abcdefghijklmn"),
                                      HM::AnsiString(stored.c_str()), serverFirst))
         return std::string();
      return std::string(serverFirst.c_str());
   }

   // PBKDF2-HMAC-SHA256 of password and salt (hex) over rounds, as hex: the key a
   // stored $h1$ hash carries.
   std::string KeyHex(const std::string &password, const std::string &saltHex, int rounds)
   {
      unsigned char salt[16] = {};
      for (size_t i = 0; i < sizeof(salt) && i * 2 + 1 < saltHex.size(); i++)
         salt[i] = static_cast<unsigned char>(std::stoi(saltHex.substr(i * 2, 2), nullptr, 16));
      unsigned char key[32] = {};
      if (PKCS5_PBKDF2_HMAC(password.c_str(), static_cast<int>(password.size()), salt, static_cast<int>(sizeof(salt)), rounds,
                            EVP_sha256(), static_cast<int>(sizeof(key)), key) != 1)
         return std::string();
      static const char digits[] = "0123456789abcdef";
      std::string hex;
      for (unsigned char byte : key)
      {
         hex += digits[byte >> 4];
         hex += digits[byte & 0x0f];
      }
      return hex;
   }
}

int
main(int /*argc*/, char * /*argv*/[])
{
   // A salt and a key of the right lengths; the key is no password's.
   const std::string saltHex = "00112233445566778899aabbccddeeff";
   const std::string saltBase64 = "ABEiM0RVZneImaq7zN3u/w==";
   const std::string keyHex(64, 'a');

   struct Case
   {
      const char *count;
      const char *offered;
      bool ownSalt;
   };
   const Case cases[] =
   {
      { "210000", "210000", true },
      { "1", "1", true },
      { "10000000", "10000000", true },
      { "10000001", "210000", false },
      { "2147483647", "210000", false },
      { "4294967297", "210000", false },
      { "99999999999", "210000", false },
      { "0", "210000", false },
      { "-5", "210000", false },
   };

   for (const Case &one : cases)
   {
      const std::string stored = "$h1$" + std::string(one.count) + "$" + saltHex + "$" + keyHex;
      const std::string serverFirst = ServerFirst(stored);
      const std::string offered = Attribute(serverFirst, 'i');
      const bool ownSalt = Attribute(serverFirst, 's') == saltBase64;
      const bool passed = !serverFirst.empty() && offered == one.offered && ownSalt == one.ownSalt;
      if (passed)
      {
         printf("ok    a stored count of %s: the client is offered i=%s, with %s\n", one.count, one.offered,
            one.ownSalt ? "the stored salt" : "an unknown account's salt");
      }
      else
      {
         printf("FAIL  a stored count of %s: wanted i=%s and %s; the server-first message was \"%s\"\n", one.count,
            one.offered, one.ownSalt ? "the stored salt" : "an unknown account's salt", serverFirst.c_str());
         failures++;
      }
   }

   // A password logon refuses what SCRAM refuses. The key is the password's over
   // one round and two, so that a count read as its low 32 bits - 4294967297 as
   // 1, 4294967298 as 2 - would let the password in; the counts said directly are
   // the control that the key is right.
   const std::string password = "SeC-r3t Pass!";
   struct Logon
   {
      const char *count;
      int keyRounds;
      bool accepted;
   };
   const Logon logons[] =
   {
      { "1", 1, true },
      { "2", 2, true },
      { "4294967297", 1, false },
      { "4294967298", 2, false },
      { "10000001", 1, false },
   };

   for (const Logon &one : logons)
   {
      const std::string key = KeyHex(password, saltHex, one.keyRounds);
      const std::string stored = "$h1$" + std::string(one.count) + "$" + saltHex + "$" + key;
      const bool accepted = !key.empty() &&
         HM::HashCreator::ValidatePBKDF2(HM::AnsiString(password.c_str()), HM::AnsiString(stored.c_str()));
      const bool scramOwnSalt = Attribute(ServerFirst(stored), 's') == saltBase64;
      if (accepted == one.accepted && scramOwnSalt == one.accepted)
      {
         printf("ok    a stored count of %s, the key over %d round(s): a password logon %s it, as SCRAM does\n", one.count,
            one.keyRounds, one.accepted ? "takes" : "refuses");
      }
      else
      {
         printf("FAIL  a stored count of %s, the key over %d round(s): a password logon %s it and SCRAM %s it; both should %s it\n",
            one.count, one.keyRounds, accepted ? "took" : "refused", scramOwnSalt ? "took" : "refused",
            one.accepted ? "take" : "refuse");
         failures++;
      }
   }

   return failures == 0 ? 0 : 1;
}
