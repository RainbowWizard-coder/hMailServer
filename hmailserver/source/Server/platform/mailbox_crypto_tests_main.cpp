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
// freed as it stood. That check writes a configuration and the machine store's
// key file of its own, in a directory of its own under the system's temp
// directory, and removes them; it then exports that key with
// StoredSecrets::ExportFile, as --export-stored-secrets-key does, and reads the
// file back with StoredSecrets::ImportFile, as --import-stored-secrets-key does,
// watched the same way. ImportFile is watched on its own too, over key files
// saved with LF, with CRLF and a UTF-8 byte order mark, and in UTF-16LE: until
// 6.3.7 the verbs read the file with ReadCompleteTextFile, whose ByteBuffer,
// narrow copy and returned String - the whole file each - were freed as they
// stood. Every piece of text is watched for as a String holds it and in
// UTF-16LE as well as in narrow characters - which found, in a GCC build, the
// String Crypt::DeCrypt copied the opened key line into on its way out until
// 6.3.7, its source, the key in hex, freed as it stood. Then
// StoredSecrets::Test, which the Windows server's self-test runs, runs here too.
// And last, the key this node then holds is watched for to the end of the
// process: StoredSecrets keeps a copy of it for as long as the process runs,
// and until 6.3.7 that copy went back to the heap as it stood when the process
// ended - the whole key, at every stop of the service and as each key verb
// returned. The watch goes on through main's return and the static destructors
// after it, and its line is the program's last, written by a destructor
// function, which runs after them.
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
#include <iterator>
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
   // piece of the key as text - its base64, or its hex - in narrow characters,
   // in a String's wide ones and in UTF-16LE, and a piece of its bytes.
   std::atomic<bool> g_watching(false);
   const std::string *g_markers[4] = {nullptr, nullptr, nullptr, nullptr};
   std::atomic<int> g_freedWithMarker(0);

   // The same from the last check to the end of the process (StartWatchingTheEnd),
   // set once and cleared only by the verdict (JudgeTheEnd).
   std::atomic<bool> g_watchingTheEnd(false);

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
   // its hex where that is what the code under watch reads. It is watched in
   // the two other forms text takes here as well: a String's, a wchar_t a
   // character, which a narrow piece is not found in - the String
   // ReadCompleteTextFile returned, which the import verbs read the key file
   // into until 6.3.7, was one, and so was the opened key line Crypt::DeCrypt
   // returned by a copy - and UTF-16LE, a key file's bytes as Windows
   // PowerShell 5 saves it.
   template <typename Call>
   int FreedWithMarker(const std::string &base64Piece, const std::string &keyPiece, Call call)
   {
      const std::wstring wide(base64Piece.begin(), base64Piece.end());
      const std::string widePiece(reinterpret_cast<const char *>(wide.data()), wide.size() * sizeof(wchar_t));
      std::string utf16Piece;
      for (const char c : base64Piece)
      {
         utf16Piece += c;
         utf16Piece += '\0';
      }

      g_markers[0] = &base64Piece;
      g_markers[1] = &keyPiece;
      g_markers[2] = &widePiece;
      g_markers[3] = &utf16Piece;
      g_freedWithMarker = 0;
      g_watching = true;
      call();
      g_watching = false;
      for (const std::string *&marker : g_markers)
         marker = nullptr;
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

   // The key in lower-case hex, as the key's line holds it ("HMK1KEY " and this).
   std::string HexOf(const std::string &key)
   {
      static const char *digits = "0123456789abcdef";
      std::string hex;
      for (std::size_t i = 0; i < key.size(); i++)
      {
         hex += digits[(static_cast<unsigned char>(key[i]) >> 4) & 0x0F];
         hex += digits[static_cast<unsigned char>(key[i]) & 0x0F];
      }
      return hex;
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
   if (g_watching.load() || g_watchingTheEnd.load())
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

   // The bytes written to a file of the scratch directory as they are.
   bool WriteScratchFile(const boost::filesystem::path &file, const std::string &bytes)
   {
      std::ofstream out(file.string().c_str(), std::ios::out | std::ios::trunc | std::ios::binary);
      out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
      return static_cast<bool>(out);
   }

   // --import-stored-secrets-key's read of the key file, StoredSecrets::
   // ImportFile, gives no buffer that held a piece of the key back to the heap
   // unwritten - from the file's bytes to ImportText's lines - for a file saved
   // with LF line ends, with CRLF and a UTF-8 byte order mark, and in UTF-16LE
   // with its mark. Each names another key on its id line, so that it is
   // refused at the check of the id against the key, after the key was read
   // and decoded, and before a database is asked. Until 6.3.7 the verbs read
   // it with ReadCompleteTextFile: the ByteBuffer the file was read into, the
   // narrow copy made of it and the String returned - the whole file each time
   // - were freed as they stood, and a file with a UTF-8 mark was refused as
   // not a key file.
   std::string CheckImportFileFreesNoKey()
   {
      const std::string key = TestKey();
      const std::string encoded = HM::MailboxCrypto::Base64(key);
      if (encoded.size() < 40)
         return "the key's base64 could not be made";

      const std::string base64Piece = encoded.substr(0, 16);
      const std::string keyPiece = key.substr(8, 16);

      boost::system::error_code error;
      const boost::filesystem::path scratch = boost::filesystem::temp_directory_path(error) /
         ("hm-mailbox-crypto-tests-files-" + std::to_string(static_cast<long long>(::getpid())));
      if (!error)
         boost::filesystem::create_directories(scratch, error);
      if (error)
         return "no scratch directory could be made under the system's temp directory";

      const std::string crlf = "hMailServer stored secrets key\r\nid 0123456789abcdef\r\nkey " + encoded + "\r\n";
      std::string utf16("\xFF\xFE", 2);
      for (const char c : crlf)
      {
         utf16 += c;
         utf16 += '\0';
      }

      struct Saved
      {
         const char *name;
         std::string bytes;
      };
      const Saved files[] =
      {
         {"lf.key", "hMailServer stored secrets key\nid 0123456789abcdef\nkey " + encoded + "\n"},
         {"crlf-utf8-mark.key", std::string("\xEF\xBB\xBF") + crlf},
         {"crlf-utf16le-mark.key", utf16},
      };

      std::string failed;
      for (const Saved &file : files)
      {
         const boost::filesystem::path path = scratch / file.name;
         if (!WriteScratchFile(path, file.bytes))
         {
            failed = std::string(file.name) + " could not be written";
            break;
         }

         const HM::String name(path.string().c_str());
         HM::AnsiString keyId;
         HM::String problem;
         bool imported = false;
         const int freed = FreedWithMarker(base64Piece, keyPiece, [&]() { imported = HM::StoredSecrets::ImportFile(name, keyId, problem); });

         const std::string why(HM::AnsiString(problem).c_str());
         if (imported)
            failed = std::string(file.name) + ": a key file whose id line names another key was imported";
         else if (why.find("is not the key its id line names") == std::string::npos)
            failed = std::string(file.name) + ": the file was refused before its key was read: " + why;
         else if (freed != 0)
            failed = std::string(file.name) + ": " + std::to_string(freed) + " buffer(s) holding the key's base64 or its bytes were freed as they stood";

         if (!failed.empty())
            break;
      }

      boost::filesystem::remove_all(scratch, error);
      return failed;
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
         std::string hex = HexOf(key);

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

         // And --export-stored-secrets-key's write of the key this node holds,
         // StoredSecrets::ExportFile, gives nothing that held the key's base64 or
         // its bytes back to the heap unwritten either; the file it writes holds
         // the key; and --import-stored-secrets-key's read of that file goes
         // through to the database's record - which this program has none of,
         // so that the import stops there - again freeing nothing unwritten.
         std::string encoded = HM::MailboxCrypto::Base64(key);
         if (failed.empty())
         {
            const std::string base64Piece = encoded.substr(0, 16);
            const std::string keyPiece = key.substr(8, 16);
            const boost::filesystem::path exported = scratch / "exported.key";
            const HM::String exportedName(exported.string().c_str());

            HM::AnsiString keyId;
            HM::String problem;
            HM::String writeError;
            bool written = false;
            int freed = FreedWithMarker(base64Piece, keyPiece, [&]() { written = HM::StoredSecrets::ExportFile(exportedName, keyId, problem, writeError); });

            std::string bytes;
            {
               std::ifstream in(exported.string().c_str(), std::ios::in | std::ios::binary);
               bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            }

            if (!written)
               failed = "the key this node holds was not exported: " + std::string(HM::AnsiString(problem + writeError).c_str());
            else if (freed != 0)
               failed = std::to_string(freed) + " buffer(s) holding the key's base64 or its bytes were freed as they stood by the export";
            else if (bytes.find("\r\nkey " + encoded + "\r\n") == std::string::npos || bytes.find("\r\nid " + std::string(keyId.c_str()) + "\r\n") == std::string::npos)
               failed = "the exported file does not hold the key under its id";

            if (failed.empty())
            {
               bool imported = false;
               freed = FreedWithMarker(base64Piece, keyPiece, [&]() { imported = HM::StoredSecrets::ImportFile(exportedName, keyId, problem); });
               const std::string why(HM::AnsiString(problem).c_str());

               if (imported)
                  failed = "the exported file was imported with no database to check it against";
               else if (why.find("the database is not open") == std::string::npos)
                  failed = "the exported file was refused before its key was checked against the database: " + why;
               else if (freed != 0)
                  failed = std::to_string(freed) + " buffer(s) holding the key's base64 or its bytes were freed as they stood by the import of the exported file";
            }

            HM::MailboxCrypto::Forget(bytes);
         }

         HM::MailboxCrypto::Forget(encoded);
         HM::MailboxCrypto::Forget(hex);
         HM::MailboxCrypto::Forget(key);
      }

      boost::filesystem::remove_all(scratch, error);
      return failed;
   }

   // The seal the stored-secrets key makes, and ImportFile's reading of a key
   // file whatever it was saved with, as the Windows server's self-test runs
   // them (ClassTester, StoredSecrets::Test).
   std::string CheckStoredSecretsSelfTest()
   {
      try
      {
         HM::StoredSecrets::Test();
      }
      catch (const std::exception &failure)
      {
         return failure.what();
      }
      catch (...)
      {
         return "StoredSecrets::Test failed";
      }
      return "";
   }

   // The markers of the watch over the end of the process: a piece of the key
   // this node holds in hex, as its line holds it - narrow, as a String and in
   // UTF-16LE - and a piece of its bytes, as StoredSecrets holds it. Made with
   // new and never freed, since a marker's own block given back while the
   // watch runs would be found holding it; what they are made from is given
   // back before the watch begins.
   void MakeMarkersOfTheEnd()
   {
      std::string key = TestKey();
      std::string hex = HexOf(key);
      const std::string hexPiece = hex.substr(16, 16);
      const std::wstring wide(hexPiece.begin(), hexPiece.end());
      std::string utf16Piece;
      for (const char c : hexPiece)
      {
         utf16Piece += c;
         utf16Piece += '\0';
      }

      g_markers[0] = new std::string(hexPiece);
      g_markers[1] = new std::string(key.substr(8, 16));
      g_markers[2] = new std::string(reinterpret_cast<const char *>(wide.data()), wide.size() * sizeof(wchar_t));
      g_markers[3] = new std::string(utf16Piece);

      HM::MailboxCrypto::Forget(hex);
      HM::MailboxCrypto::Forget(key);
   }

   // The key this node holds - the one the check reading the key from its line
   // left held - watched for from here to the end of the process, in every
   // block given back to the heap: main's last, then the static destructors',
   // StoredSecrets' own copy of the key among them, which went back as it
   // stood until 6.3.7. A destructor function gives the verdict (JudgeTheEnd).
   // Refused when this node holds no key: the watch would find nothing to find.
   std::string StartWatchingTheEnd()
   {
      if (HM::StoredSecrets::State() != HM::StoredSecrets::KeyHeld)
         return "this node holds no key to watch for, where the check that read the key from its line left one held";

      MakeMarkersOfTheEnd();
      g_freedWithMarker = 0;
      (void) std::fflush(stdout);
      g_watchingTheEnd = true;
      return "";
   }

   // The verdict on the end of the process. A destructor function runs after
   // every static destructor - glibc runs the static destructors from the
   // atexit list and this from _dl_fini, which that list runs last, and musl
   // runs it from __libc_exit_fini, after the list - and before the standard
   // streams are flushed. A block found ends the program with exit 1, whatever
   // main returned.
   __attribute__((destructor)) void JudgeTheEnd()
   {
      if (!g_watchingTheEnd.load())
         return;

      g_watchingTheEnd = false;
      const int freed = g_freedWithMarker.load();
      if (freed == 0)
      {
         printf("ok    the key this node holds, as the process ends: no buffer that held it in hex or its bytes freed as it stood, main's return and the static destructors included\n");
         return;
      }

      printf("FAIL  the key this node holds, as the process ends: %d buffer(s) holding the key in hex or its bytes were freed as they stood\n", freed);
      (void) std::fflush(stdout);
      ::_exit(1);
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

   if (!Run("a stored-secrets key file read from disk", CheckImportFileFreesNoKey, "LF, CRLF with a UTF-8 mark and UTF-16LE read through to the key, and no buffer that held a piece of it freed as it stood"))
      return 1;

   if (!Run("the stored-secrets key read from its line, exported and imported again", CheckHeldKeyReadFreesNoKey,
            "no buffer that held the key in hex, in base64 or its bytes freed as it stood"))
      return 1;

   if (!Run("the stored-secrets seal and key files", CheckStoredSecretsSelfTest, "StoredSecrets::Test, as the Windows server's self-test runs it"))
      return 1;

   // Last, and judged after everything else has ended (JudgeTheEnd).
   const std::string notWatched = StartWatchingTheEnd();
   if (!notWatched.empty())
   {
      printf("FAIL  the key this node holds, as the process ends: %s\n", notWatched.c_str());
      return 1;
   }

   return 0;
}
