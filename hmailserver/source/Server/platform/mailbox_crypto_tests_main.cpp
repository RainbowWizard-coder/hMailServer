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
// ImportText runs can be searched, as it is given back, for the key's base64
// and for the key's own bytes, which the base64 is decoded into. Until 6.3.6
// the line being read grew a character at a time and freed the smaller buffers
// it outgrew as they stood - with the key's line, most of the key in base64.
// MailboxCrypto::Base64 is held to the same, encoding a key as ExportText does
// the stored-secrets key and MailboxKeys::Enable the private key it makes: until
// 6.3.6 it freed the buffer it encoded into as it stood, the whole key in
// base64. And so is StoredSecrets reading the key this node holds from its line
// in the configuration file, for the key in hex, which the line holds, and for
// its bytes: until 6.3.6 the hex was copied out of the line through a temporary
// freed as it stood. That check, the last, writes a configuration and the
// machine store's key file of its own, in a directory of its own under the
// system's temp directory, and removes them.
//
// Every block is handed out zeroed: a block can be memory freed before the
// watch began - this program's own copies of the key file - and a buffer
// reserved and written only in part, as ImportText does the line it reads,
// would carry those bytes back to the heap past the part written and wiped.
// musl's allocator did that on Alpine: the block ImportText reserved for its
// line had last held this program's key file short of its last line end, a
// copy outgrown when the line end was appended and freed before ImportText
// ran. glibc's allocator happened not to.
//
// One line a check, and a non-zero exit on the first failure.

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <new>
#include <stdexcept>
#include <string>

#include <boost/filesystem.hpp>

#include "../Common/Application/IniFileSettings.h"
#include "../Common/Util/Crypt.h"
#include "../Common/Util/MailboxCrypto.h"
#include "../Common/Util/StoredSecrets.h"

namespace
{
   // Room in front of every block for its size, kept to the alignment operator
   // new promises, so that what follows it is aligned as malloc's own answer is.
   const std::size_t BlockHeader = alignof(std::max_align_t) > sizeof(std::size_t) ? alignof(std::max_align_t) : sizeof(std::size_t);

   // While watching is set, each block freed is searched for the markers: a
   // piece of the key as text - its base64, or its hex - and a piece of its
   // bytes.
   std::atomic<bool> g_watching(false);
   const std::string *g_markers[2] = {nullptr, nullptr};
   std::atomic<int> g_freedWithMarker(0);

   bool HoldsAMarker(const char *begin, std::size_t size)
   {
      for (const std::string *marker : g_markers)
      {
         if (marker != nullptr && !marker->empty() &&
             std::search(begin, begin + size, marker->begin(), marker->end()) != begin + size)
            return true;
      }
      return false;
   }

   // The markers watched while the call runs, and the count of blocks freed
   // with one of them in it. The first is the key as text: its base64, or
   // its hex where that is what the code under watch reads.
   template <typename Call>
   int FreedWithMarker(const std::string &base64Piece, const std::string &keyPiece, Call call)
   {
      g_markers[0] = &base64Piece;
      g_markers[1] = &keyPiece;
      g_freedWithMarker = 0;
      g_watching = true;
      call();
      g_watching = false;
      g_markers[0] = nullptr;
      g_markers[1] = nullptr;
      return g_freedWithMarker.load();
   }

   // The key the checks below use, its bytes chosen to be found nowhere else.
   std::string TestKey()
   {
      std::string key(HM::MailboxCrypto::KeyBytes, '\0');
      for (std::size_t i = 0; i < key.size(); i++)
         key[i] = static_cast<char>(0xA5 ^ (i * 29 + 7));
      return key;
   }
}

void *
operator new(std::size_t size)
{
   void *block = std::malloc(size + BlockHeader);
   if (block == nullptr)
      throw std::bad_alloc();
   *static_cast<std::size_t *>(block) = size;
   // Zeroed, so that what a freed block is found holding is what was written
   // into it since, not what the memory held before (above).
   std::memset(static_cast<char *>(block) + BlockHeader, 0, size);
   return static_cast<char *>(block) + BlockHeader;
}

void
operator delete(void *pointer) noexcept
{
   if (pointer == nullptr)
      return;

   char *block = static_cast<char *>(pointer) - BlockHeader;
   if (g_watching.load())
   {
      const std::size_t size = *reinterpret_cast<const std::size_t *>(block);
      if (HoldsAMarker(static_cast<const char *>(pointer), size))
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
   // MailboxCrypto::Base64 of a key gives no buffer back to the heap holding the
   // base64 it made: only the string it returns holds it, which the caller
   // forgets - ExportText the stored-secrets key's, MailboxKeys::Enable the
   // private key's it makes.
   std::string CheckBase64FreesNoKey()
   {
      std::string key = TestKey();
      std::string encoded = HM::MailboxCrypto::Base64(key);
      if (encoded.size() < 40)
         return "the key's base64 could not be made";

      const std::string base64Piece = encoded.substr(0, 16);
      const std::string keyPiece = key.substr(8, 16);
      std::string again;
      const int freed = FreedWithMarker(base64Piece, keyPiece, [&]() { again = HM::MailboxCrypto::Base64(key); });
      const bool same = again == encoded;
      HM::MailboxCrypto::Forget(again);
      HM::MailboxCrypto::Forget(encoded);
      HM::MailboxCrypto::Forget(key);

      if (!same)
         return "the same key encoded twice came out two ways";

      if (freed != 0)
         return std::to_string(freed) + " buffer(s) holding the key's base64 were freed as they stood";

      return "";
   }

   // A key file as --export-stored-secrets-key writes one, with LF line ends, but
   // with an id line that names another key: ImportText reads every line and
   // decodes the key, and refuses the file at the id check, before it asks for
   // a database. Nothing it freed on the way may hold the key's base64, or the
   // key's bytes the base64 was decoded into.
   std::string CheckImportTextFreesNoKey()
   {
      const std::string key = TestKey();
      const std::string encoded = HM::MailboxCrypto::Base64(key);
      if (encoded.size() < 40)
         return "the key's base64 could not be made";

      // The first sixteen characters of the base64: the first buffer that a line
      // grown a character at a time outgrows holds them. And sixteen of the key's
      // own bytes: a buffer the base64 was decoded into holds them.
      const std::string base64Piece = encoded.substr(0, 16);
      const std::string keyPiece = key.substr(8, 16);

      const HM::AnsiString text = HM::AnsiString("hMailServer stored secrets key\nid 0123456789abcdef\nkey ") +
                                  HM::AnsiString(encoded.c_str()) + HM::AnsiString("\n");
      HM::AnsiString keyId;
      HM::String problem;

      bool imported = false;
      const int freed = FreedWithMarker(base64Piece, keyPiece, [&]() { imported = HM::StoredSecrets::ImportText(text, keyId, problem); });

      if (imported)
         return "a key file whose id line names another key was imported";

      const std::string why(HM::AnsiString(problem).c_str());
      if (why.find("is not the key its id line names") == std::string::npos)
         return "the file was refused before its key was read: " + why;

      if (freed != 0)
         return std::to_string(freed) + " buffer(s) holding the key's base64 or its bytes were freed as they stood";

      return "";
   }

   // The key this node holds, read from its line in the configuration file -
   // "HMK1KEY " and the key in hex, under this machine's store, as
   // --create-stored-secrets-key and --import-stored-secrets-key write it - gives
   // no buffer that held the hex, or the key's bytes, back to the heap unwritten.
   // Until 6.3.6 the hex was copied out of the opened line with Mid, which makes
   // its copy from a temporary that was freed as it stood: the whole key in hex.
   // The configuration and the machine store's key file are this program's own,
   // in a directory of its own under the system's temp directory.
   std::string CheckHeldKeyReadFreesNoKey()
   {
      boost::system::error_code error;
      const boost::filesystem::path scratch = boost::filesystem::temp_directory_path(error) /
         ("hm-mailbox-crypto-tests-" + std::to_string(static_cast<long long>(::getpid())));
      if (!error)
         boost::filesystem::create_directories(scratch, error);
      if (error)
         return "no scratch directory could be made under the system's temp directory";

      std::string failed;
      {
         const std::string directory = scratch.string();
         const boost::filesystem::path file = scratch / "hMailServer.ini";
         {
            std::ofstream ini(file.string().c_str(), std::ios::out | std::ios::trunc);
            ini << "[Directories]\n"
                << "ProgramFolder=" << directory << "\n"
                << "DataFolder=" << directory << "\n"
                << "LogFolder=" << directory << "\n"
                << "TempFolder=" << directory << "\n"
                << "EventFolder=" << directory << "\n"
                << "DatabaseFolder=" << directory << "\n";
            if (!ini)
               failed = "the configuration file could not be written";
         }

         std::string key = TestKey();
         static const char *digits = "0123456789abcdef";
         std::string hex;
         for (std::size_t i = 0; i < key.size(); i++)
         {
            hex += digits[(static_cast<unsigned char>(key[i]) >> 4) & 0x0F];
            hex += digits[static_cast<unsigned char>(key[i]) & 0x0F];
         }

         if (failed.empty())
         {
            HM::IniFileSettings::Instance()->SetInitializationFile(HM::String(file.string().c_str()));
            HM::IniFileSettings::Instance()->LoadSettings();

            // The line as StoredSecrets writes it, through the machine's store,
            // whose key file is made in the data directory, the scratch one.
            const HM::String inner = HM::Crypt::Instance()->EnCrypt(HM::String(HM::AnsiString(("HMK1KEY " + hex).c_str())), HM::Crypt::ETDPAPI);
            HM::String problem;
            if (inner.IsEmpty())
               failed = "this machine's store did not protect the key line";
            else if (!HM::IniFileSettings::Instance()->ReplaceStoredSecretsKeyProtected(HM::String(), _T("LINUX1:") + inner, problem))
               failed = "the key line could not be written: " + std::string(HM::AnsiString(problem).c_str());
         }

         if (failed.empty())
         {
            // Sixteen characters of the hex, and sixteen of the key's own bytes,
            // which the hex is read into.
            const std::string hexPiece = hex.substr(16, 16);
            const std::string keyPiece = key.substr(8, 16);

            HM::StoredSecrets::KeyState state = HM::StoredSecrets::KeyNone;
            const int freed = FreedWithMarker(hexPiece, keyPiece, [&]() { state = HM::StoredSecrets::State(); });

            if (state != HM::StoredSecrets::KeyHeld)
               failed = "the key line written was not read as a key this node holds";
            else if (freed != 0)
               failed = std::to_string(freed) + " buffer(s) holding the key in hex or its bytes were freed as they stood";
         }

         HM::MailboxCrypto::Forget(hex);
         HM::MailboxCrypto::Forget(key);
      }

      boost::filesystem::remove_all(scratch, error);
      return failed;
   }

   // One check, one line; false when it failed.
   bool Run(const char *what, std::string (*check)(), const char *held)
   {
      std::string failed;
      try
      {
         failed = check();
      }
      catch (const std::exception &failure)
      {
         failed = failure.what();
      }

      if (!failed.empty())
      {
         printf("FAIL  %s: %s\n", what, failed.c_str());
         return false;
      }

      printf("ok    %s: %s\n", what, held);
      return true;
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

   if (!Run("a key encoded in base64", CheckBase64FreesNoKey, "no buffer that held its base64 freed as it stood"))
      return 1;

   if (!Run("a stored-secrets key file read", CheckImportTextFreesNoKey, "no buffer that held a piece of the key freed as it stood"))
      return 1;

   if (!Run("the stored-secrets key read from its line", CheckHeldKeyReadFreesNoKey, "no buffer that held the key in hex or its bytes freed as it stood"))
      return 1;

   return 0;
}
