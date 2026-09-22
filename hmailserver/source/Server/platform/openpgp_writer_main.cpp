// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The server's OpenPGP writer as a program of its own, for the POSIX build:
// the one thing this server writes in OpenPGP - a message encrypted to a
// public key, which is what a zero-access mailbox gets on delivery
// (Common/Util/OpenPgpWriter.h, docs/ZeroAccess.md) - handed to GnuPG.
//
// The writer's own tester runs inside the server (ClassTester on Windows,
// reached by the suite through Utilities.RunTestSuite) and proves the packets
// against RFC 9580 and the arithmetic against RFC 7748, but a test that
// agrees with itself is not proof that another implementation opens what it
// wrote. This is how that other implementation is reached:
// .github/workflows/linux-build.yml runs it against the fixture keys GnuPG
// 2.4 made (build/fixtures/openpgp) and hands the output to gpg --decrypt on
// the runner, once for the Curve25519 key and once for the RSA one, and the
// bytes gpg prints have to be the bytes this was given.
//
//   openpgp-writer <public key file, armoured or binary> <input file> [--mime]
//
// Writes the armoured OpenPGP message to standard output; with --mime, the
// whole input is read as an RFC 5322 message and the output is the PGP/MIME
// message a mailbox receives (RFC 3156), which is what the page's module and
// gpg both open in build/check-portal-openpgp.js. Exit code 0 on success, 1
// with the writer's own sentence on standard error otherwise.
//
// Nothing here touches the network, the configuration or a database.

#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "../Common/Util/OpenPgpKey.h"
#include "../Common/Util/OpenPgpWriter.h"

namespace
{
   bool ReadWhole(const char *path, std::string &out)
   {
      std::ifstream in(path, std::ios::binary);
      if (!in)
         return false;
      out.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      return true;
   }
}

int
main(int argc, char *argv[])
{
   if (argc < 3)
   {
      fprintf(stderr, "usage: openpgp-writer <public key> <input> [--mime]\n");
      return 2;
   }

   const bool mime = argc > 3 && strcmp(argv[3], "--mime") == 0;

   std::string keyText;
   std::string input;
   if (!ReadWhole(argv[1], keyText) || !ReadWhole(argv[2], input))
   {
      fprintf(stderr, "openpgp-writer: a file could not be read\n");
      return 1;
   }

   // Armoured or binary, as the ring holds either.
   std::vector<unsigned char> keyBytes;
   if (keyText.find("-----BEGIN PGP") != std::string::npos)
   {
      HM::AnsiString problem;
      if (!HM::OpenPgpKey::Dearmour(HM::AnsiString(keyText), keyBytes, problem))
      {
         fprintf(stderr, "openpgp-writer: %s\n", problem.c_str());
         return 1;
      }
   }
   else
      keyBytes.assign(keyText.begin(), keyText.end());

   const HM::OpenPgpWriter::EncryptionKey key = HM::OpenPgpWriter::ChooseEncryptionKey(&keyBytes[0], keyBytes.size(), (long long) time(nullptr));
   if (!key.ok)
   {
      fprintf(stderr, "openpgp-writer: %s\n", key.error.c_str());
      return 1;
   }

   HM::AnsiString error;
   if (mime)
   {
      HM::AnsiString sealed;
      if (!HM::OpenPgpWriter::SealMessage(key, HM::AnsiString(input), sealed, error))
      {
         fprintf(stderr, "openpgp-writer: %s\n", error.c_str());
         return 1;
      }
      fwrite(sealed.c_str(), 1, (size_t) sealed.GetLength(), stdout);
      return 0;
   }

   std::vector<unsigned char> message;
   if (!HM::OpenPgpWriter::Encrypt(key, (const unsigned char *) input.data(), input.size(), message, error))
   {
      fprintf(stderr, "openpgp-writer: %s\n", error.c_str());
      return 1;
   }

   const HM::AnsiString armoured = HM::OpenPgpWriter::Armour(message);
   fwrite(armoured.c_str(), 1, (size_t) armoured.GetLength(), stdout);
   fprintf(stderr, "openpgp-writer: encrypted %u bytes to %s (%s)\n", (unsigned) input.size(), key.fingerprint.c_str(),
           key.algorithm == 18 ? "Curve25519" : "RSA");
   return 0;
}
