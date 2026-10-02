// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The cryptography of mailbox encryption at rest (Common/Util/MailboxCrypto.h),
// as a program of its own for the POSIX build.
//
// On Windows the same test runs inside the shipping server: ClassTester::
// DoTests calls MailboxCrypto::Test, and the regression suite reaches it through
// Utilities.RunTestSuite. That route does not exist here, so the CMake build
// makes this instead and linux-build.sh runs it with every other *-tests
// program. Until 6.3.6 nothing ran it on Linux at all.
//
// What it holds: X25519 against RFC 7748's vector, HKDF-SHA256 against RFC
// 5869's first case, base64 both ways, every seal and wrap round-tripped with
// its refusals, a message's stage opened and refused - and ForgetOnExit, the
// guard every private key in MailboxKeys.cpp is watched by: the bytes written
// over when the scope ends by a return, early or late, or by an exception
// thrown through it, beside the control of the same exception with nothing
// watching, which leaves them; and ForgetEachOnExit, which watches the lines of
// a stored-secrets key file the same way. Nothing here touches the network or a database;
// the wraps take the password-hash work from the defaults, with no
// configuration read.
//
// And StoredSecrets::ImportText, which reads such a key file, gives no buffer
// that held a piece of the key back to the heap unwritten: every allocation of
// this program carries its size in front of it, so that a block freed while
// ImportText runs can be searched, as it is given back, for the key's base64.
// Until 6.3.6 the line being read grew a character at a time and freed the
// smaller buffers it outgrew as they stood - with the key's line, most of the
// key in base64.
//
// One line a check, and a non-zero exit on the first failure.

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <stdexcept>
#include <string>

#include "../Common/Util/MailboxCrypto.h"
#include "../Common/Util/StoredSecrets.h"

namespace
{
   // Room in front of every block for its size, kept to the alignment operator
   // new promises, so that what follows it is aligned as malloc's own answer is.
   const std::size_t BlockHeader = alignof(std::max_align_t) > sizeof(std::size_t) ? alignof(std::max_align_t) : sizeof(std::size_t);

   // While watching is set, each block freed is searched for the marker.
   std::atomic<bool> g_watching(false);
   const char *g_marker = nullptr;
   std::size_t g_markerLength = 0;
   std::atomic<int> g_freedWithMarker(0);
}

void *
operator new(std::size_t size)
{
   void *block = std::malloc(size + BlockHeader);
   if (block == nullptr)
      throw std::bad_alloc();
   *static_cast<std::size_t *>(block) = size;
   return static_cast<char *>(block) + BlockHeader;
}

void
operator delete(void *pointer) noexcept
{
   if (pointer == nullptr)
      return;

   char *block = static_cast<char *>(pointer) - BlockHeader;
   if (g_watching.load() && g_marker != nullptr)
   {
      const std::size_t size = *reinterpret_cast<const std::size_t *>(block);
      const char *begin = static_cast<const char *>(pointer);
      if (std::search(begin, begin + size, g_marker, g_marker + g_markerLength) != begin + size)
         g_freedWithMarker++;
   }
   std::free(block);
}

void *operator new[](std::size_t size) { return operator new(size); }
void operator delete[](void *pointer) noexcept { operator delete(pointer); }
void operator delete(void *pointer, std::size_t) noexcept { operator delete(pointer); }
void operator delete[](void *pointer, std::size_t) noexcept { operator delete(pointer); }

void *
operator new(std::size_t size, const std::nothrow_t &) noexcept
{
   try
   {
      return operator new(size);
   }
   catch (...)
   {
      return nullptr;
   }
}

void *operator new[](std::size_t size, const std::nothrow_t &tag) noexcept { return operator new(size, tag); }
void operator delete(void *pointer, const std::nothrow_t &) noexcept { operator delete(pointer); }
void operator delete[](void *pointer, const std::nothrow_t &) noexcept { operator delete(pointer); }

namespace
{
   // A key file as --export-stored-secrets-key writes one, with LF line ends, but
   // with an id line that names another key: ImportText reads every line and
   // decodes the key, and refuses the file at the id check, before it asks for
   // a database. Nothing it freed on the way may hold the key's base64.
   std::string CheckImportTextFreesNoKey()
   {
      std::string key(HM::MailboxCrypto::KeyBytes, '\0');
      for (std::size_t i = 0; i < key.size(); i++)
         key[i] = static_cast<char>(0xA5 ^ (i * 29 + 7));

      const std::string encoded = HM::MailboxCrypto::Base64(key);
      if (encoded.size() < 40)
         return "the key's base64 could not be made";

      // The first sixteen characters of the base64: the first buffer that a line
      // grown a character at a time outgrows holds them.
      const std::string marker = encoded.substr(0, 16);

      const HM::AnsiString text = HM::AnsiString("hMailServer stored secrets key\nid 0123456789abcdef\nkey ") +
                                  HM::AnsiString(encoded.c_str()) + HM::AnsiString("\n");
      HM::AnsiString keyId;
      HM::String problem;

      g_marker = marker.data();
      g_markerLength = marker.size();
      g_freedWithMarker = 0;
      g_watching = true;
      const bool imported = HM::StoredSecrets::ImportText(text, keyId, problem);
      g_watching = false;
      const int freed = g_freedWithMarker.load();
      g_marker = nullptr;

      if (imported)
         return "a key file whose id line names another key was imported";

      const std::string why(HM::AnsiString(problem).c_str());
      if (why.find("is not the key its id line names") == std::string::npos)
         return "the file was refused before its key was read: " + why;

      if (freed != 0)
         return std::to_string(freed) + " buffer(s) holding the key's base64 were freed as they stood";

      return "";
   }
}

int
main(int /*argc*/, char * /*argv*/[])
{
   try
   {
      HM::MailboxCrypto::Test();
   }
   catch (const std::exception &failure)
   {
      printf("FAIL  mailbox encryption's cryptography: %s\n", failure.what());
      return 1;
   }

   printf("ok    mailbox encryption's cryptography: X25519, HKDF, base64, seals, wraps and stages with their refusals, and a key forgotten on every way out of its scope\n");

   std::string failed;
   try
   {
      failed = CheckImportTextFreesNoKey();
   }
   catch (const std::exception &failure)
   {
      failed = failure.what();
   }

   if (!failed.empty())
   {
      printf("FAIL  a stored-secrets key file read: %s\n", failed.c_str());
      return 1;
   }

   printf("ok    a stored-secrets key file read: no buffer that held a piece of the key freed as it stood\n");
   return 0;
}
