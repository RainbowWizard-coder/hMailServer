// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace HM
{
   // The legacy Blowfish scheme of stored secrets (Crypt::ETBlowFish): a
   // database password in hMailServer.ini written with PasswordEncryption=1; a
   // route, fetch account, relayer or other protected secret stored while
   // ProtectStoredSecretsWithDPAPI is 0 or DPAPI failed; and, whatever that
   // setting says, the passwords and OAuth2 tokens of fetch accounts, the
   // passwords of routes and the private-key passwords of certificates in every
   // backup archive (FetchAccount, Route and SSLCertificate::XMLStore), and a
   // fetch account's in a person's export, which writes them the backup's way
   // and then leaves them out. COM's Utilities.BlowfishEncrypt and
   // BlowfishDecrypt call it too. Its key is a constant in the source, so it
   // protects a secret from nobody who has the binary; it is kept so that every
   // value an earlier hMailServer wrote still opens, and what it writes is
   // exactly what every earlier version wrote.
   //
   // The project's own code, written in September 2026 from the published
   // description of the cipher - B. Schneier, "Description of a New
   // Variable-Length Key, 64-Bit Block Cipher (Blowfish)", Fast Software
   // Encryption, Cambridge Security Workshop (December 1993), Springer-Verlag
   // 1994, pp. 191-204; the algorithm is unpatented and its designer placed it in
   // the public domain - and from the scheme's observable behaviour, which it
   // keeps byte for byte:
   //
   //  - the key is the 22 ASCII bytes THIS_KEY_IS_NOT_SECRET;
   //  - Blowfish, 16 rounds, each 8-byte block on its own (ECB), no IV;
   //  - each block's two 32-bit halves are its bytes 0-3 and 4-7 read
   //    little-endian (the byte order of every machine the scheme ran on, where
   //    it read them as the machine's own words; the published test vectors
   //    read them big-endian, which EncryptBlock/DecryptBlock below take);
   //  - text is taken as Unicode::ToANSI makes it, at most as many bytes as the
   //    text has characters, zero-padded to whole blocks, and written as
   //    lowercase hexadecimal; hexadecimal is read two characters at a time as
   //    strtol reads them, and the text ends at the first zero byte.
   class LegacyBlowfish
   {
   public:
      // The longest value EncryptToString takes, in characters: the scheme wrote
      // from a 255-byte buffer in whole 8-byte blocks, and 248 is the most that
      // fits (versions before 6.3.4 wrote past the buffer for 249 to 254, and
      // what they wrote still reads). A longer value is refused (HM7277) and
      // answered with an empty string, as 6.3.4 did.
      static constexpr int MaxPlainLength = 248;

      // Keyed with the scheme's fixed key.
      LegacyBlowfish();

      // Keyed with the given key, one byte or more: the published algorithm's
      // keys are 4 to 56 bytes, and the schedule cycles through whatever it is
      // given. For the known-answer tests.
      LegacyBlowfish(const unsigned char *key, size_t keyLength);

      // The scheme's two operations (see above). Empty gives empty both ways.
      // Neither changes the object, so one instance serves every thread.
      String EncryptToString(const String &plainText) const;
      String DecryptFromString(const String &hexText) const;

      // One block, its halves as the published algorithm and its test vectors
      // name them: left is bytes 0-3 read big-endian, right bytes 4-7.
      void EncryptBlock(uint32_t &left, uint32_t &right) const;
      void DecryptBlock(uint32_t &left, uint32_t &right) const;

   private:
      void SetKey_(const unsigned char *key, size_t keyLength);
      uint32_t F_(uint32_t half) const;

      // Each block of the scheme, in place: bytes' length a multiple of 8.
      void EncryptBlocks_(std::vector<unsigned char> &bytes) const;
      void DecryptBlocks_(std::vector<unsigned char> &bytes) const;

      // P1 to P18, and the four S-boxes: pi's digits and then the key's schedule
      // (SetKey_), which every constructor runs.
      uint32_t p_[18] = {};
      uint32_t s_[4][256] = {};
   };

   // Blowfish against Eric Young's published test vectors, and the scheme
   // against values earlier versions wrote. Run() answers one line per failure;
   // Test() reports them and throws, for ClassTester. On Linux the program
   // legacy-blowfish-tests runs it (platform/legacy_blowfish_tests_main.cpp).
   class LegacyBlowfishTester
   {
   public:
      std::vector<AnsiString> Run();
      void Test();
   };
}
