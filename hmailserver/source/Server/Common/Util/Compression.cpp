// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "Compression.h"
#include "ProcessLauncher.h"
#include "FileUtilities.h"
#include "Unicode.h"

#include <boost/filesystem.hpp>

#include <fstream>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{

   Compression::Compression() :
      maximum_run_time_ms_(0),
      last_run_timed_out_(false)
   {
      
   }

   Compression::~Compression(void)
   {

   }

   namespace
   {
      // The switches that make 7-Zip encrypt what it adds. -p with NO value is
      // 7-Zip's "ask for the password", which it asks for on its standard input -
      // so the passphrase is never a value on the command line, where every user
      // of the machine can read it in the process list. -mhe=on encrypts the
      // headers as well as the data, so the file names cannot be listed either.
      String EncryptionSwitches_(const String &passphrase)
      {
         return passphrase.IsEmpty() ? String() : String(_T(" -p -mhe=on"));
      }

      // What 7-Zip reads when it asks: the passphrase on a line of its own, three
      // times, because how often it asks depends on the archiver and on what it is
      // doing. Measured with 7za 19.00: creating an archive asks once, adding to
      // an archive whose headers are already encrypted asks twice (once to open
      // it, once for what is being added), and opening one asks once; p7zip asks a
      // creating run to type it a second time to confirm. A line nobody asks for
      // is simply never read.
      //
      // Every passphrase is printable ASCII (Configuration::CheckBackupEncryption-
      // Passphrase), so these bytes are those characters whatever code page or
      // locale the archiver decodes its input in - and the key derived from them
      // is the key any other 7-Zip derives from the same passphrase typed by hand.
      //
      // Empty when there is no passphrase, which is still a standard input and is
      // the point: an archive that asks for a passphrase nobody gave now reads the
      // end of the input and fails (7-Zip exits 255) where it used to wait for
      // ever at a console prompt nobody could see.
      AnsiString PassphraseInput_(const String &passphrase)
      {
         if (passphrase.IsEmpty())
            return AnsiString();

         AnsiString line = AnsiString(passphrase) + "\n";
         return line + line + line;
      }
   }

   bool
   Compression::AddDirectory(const String &zipFile, const String &directoryToAdd, const String &passphrase)
   {
      // -r = recurse -t = type 7z -mmt = multithread off -mx1 = lowest compression (safer, faster & less cpu+ram)
      String commandLine = Formatter::Format("\"{0}\" a \"{1}\" \"{2}\" -r -t7z -mmt -mx1  -w\"{3}\"",
         GetExecutableFullPath_(), zipFile, directoryToAdd, IniFileSettings::Instance()->GetTempDirectory());

      return LaunchCommand_(commandLine + EncryptionSwitches_(passphrase), passphrase);
   }

   bool
   Compression::AddFile(const String &zipFile, const String &fileToAdd, const String &passphrase)
   {
      // -t = type 7z -mmt = multithread off -mx1 = lowest compression (safer, faster & less cpu+ram)
      String commandLine = Formatter::Format("\"{0}\" a \"{1}\" \"{2}\" -t7z -mmt -mx1 -w\"{3}\"",
         GetExecutableFullPath_(), zipFile, fileToAdd, IniFileSettings::Instance()->GetTempDirectory());

      return LaunchCommand_(commandLine + EncryptionSwitches_(passphrase), passphrase);
   }

   void
   Compression::SetMaximumRunTime(unsigned int milliseconds)
   {
      maximum_run_time_ms_ = milliseconds;
   }

   bool
   Compression::GetLastRunTimedOut() const
   {
      return last_run_timed_out_;
   }

   bool
   Compression::UncompressAll(const String &zipFile, const String &targetDirectory, const String &passphrase)
   {
      return UncompressMember(zipFile, targetDirectory, "*", passphrase);
   }

   bool
   Compression::UncompressMember(const String &zipFile, const String &targetDirectory, const String &wildCard, const String &passphrase)
   {
      // No -p here, deliberately. To 7-Zip OPENING an archive, -p with no value
      // means "the password is empty" rather than "ask" - measured with 7za 19.00,
      // which then fails every encrypted archive as a wrong password. Left off, it
      // asks when and only when the archive needs one, and reads the answer from
      // its standard input.
      String commandLine = Formatter::Format("\"{0}\" x \"{1}\" \"{2}\" -o\"{3}\" -y",
         GetExecutableFullPath_(), zipFile, wildCard, targetDirectory);

      return LaunchCommand_(commandLine, passphrase);
   }

   bool
   Compression::UncompressMembers(const String &zipFile, const String &targetDirectory, const std::vector<String> &members, const String &passphrase)
   {
      if (members.empty())
         return true;

      // The list file sits in the target directory, which is the caller's own
      // scratch directory, and is removed once the archiver has read it. Bytes
      // written directly: UTF-8 with the byte-order mark 7-Zip recognises, one
      // member per line, no quoting - a list file takes a name with spaces as
      // it is.
      String listFile = FileUtilities::Combine(targetDirectory, _T("hMailServer-members.txt"));

      {
         std::ofstream out(listFile.c_str(), std::ios::binary | std::ios::trunc);

         if (!out)
            return false;

         out.write("\xEF\xBB\xBF", 3);

         for (size_t i = 0; i < members.size(); i++)
         {
            AnsiString utf8;
            Unicode::WideToMultiByte(members[i], utf8);
            out.write(utf8.c_str(), utf8.GetLength());
            out.write("\r\n", 2);
         }

         if (!out)
            return false;
      }

      String commandLine = Formatter::Format("\"{0}\" x \"{1}\" \"@{2}\" -o\"{3}\" -y -scsUTF-8",
         GetExecutableFullPath_(), zipFile, listFile, targetDirectory);

      const bool result = LaunchCommand_(commandLine, passphrase);

      FileUtilities::DeleteFile(listFile);

      return result;
   }

   Compression::ArchiveEncryption
   Compression::GetArchiveEncryption(const String &zipFile)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Whether an archive is encrypted, read from the archive itself.
   //
   // WHY THIS IS NOT ASKED OF 7-ZIP. An encrypted archive opened without its
   // passphrase and a damaged archive fail alike, with one exit code between
   // them, and the difference is the whole of what the administrator needs to be
   // told: one means "find the passphrase", the other means "find another
   // archive". Reading the header answers it without running anything.
   //
   // THE FORMAT (7-Zip's DOC/7zFormat.txt). Thirty-two bytes at the start: the
   // six-byte signature, the version, a CRC, then the offset from byte 32 of the
   // "next header" and its size. The next header is at the END of the file, which
   // is why this seeks with a 64-bit offset - an archive of a whole message store
   // is routinely past what a 32-bit position could reach - and is either the
   // header itself (property id 0x01, kHeader), whose file names are then in the
   // clear, or an encoded header (0x17, kEncodedHeader): a short description of
   // the coders the real header was packed with. 7-Zip packs the header even with
   // no encryption, so 0x17 alone proves nothing; what proves it is the AES coder
   // among those coders, method id 06 F1 07 01.
   //
   // Checked against 7za 19.00: an archive written with -p -mhe=on has a 62-byte
   // encoded header carrying that id, and one written without has a plain header.
   // Anything that cannot be read as an archive at all - no file, no signature, a
   // next header past the end of a truncated file - is EncryptionUnknown, and the
   // caller then lets 7-Zip say what is wrong with it, as before.
   //---------------------------------------------------------------------------()
   {
      try
      {
         const boost::filesystem::path path(zipFile.c_str());
         std::ifstream archive(path.c_str(), std::ios::in | std::ios::binary);
         if (!archive.is_open())
            return EncryptionUnknown;

         unsigned char start[32];
         archive.read((char *) start, sizeof(start));
         if (archive.gcount() != (std::streamsize) sizeof(start))
            return EncryptionUnknown;

         const unsigned char signature[6] = { 0x37, 0x7A, 0xBC, 0xAF, 0x27, 0x1C };
         if (memcmp(start, signature, sizeof(signature)) != 0)
            return EncryptionUnknown;

         unsigned __int64 nextHeaderOffset = 0;
         unsigned __int64 nextHeaderSize = 0;

         for (int i = 7; i >= 0; i--)
         {
            nextHeaderOffset = (nextHeaderOffset << 8) | start[12 + i];
            nextHeaderSize = (nextHeaderSize << 8) | start[20 + i];
         }

         // An archive with nothing in it has no header to read.
         if (nextHeaderSize == 0)
            return EncryptionNone;

         // A position no file could hold is a broken or hostile header, not a
         // place to seek to.
         if (nextHeaderOffset > (unsigned __int64) 0x7FFFFFFFFFFF0000ull)
            return EncryptionUnknown;

         archive.seekg((std::streamoff) (32 + nextHeaderOffset), std::ios::beg);
         if (!archive)
            return EncryptionUnknown;

         const size_t readLimit = 64 * 1024;
         const size_t wanted = nextHeaderSize < (unsigned __int64) readLimit ? (size_t) nextHeaderSize : readLimit;

         std::vector<unsigned char> header(wanted);
         archive.read((char *) &header[0], (std::streamsize) wanted);
         if (archive.gcount() != (std::streamsize) wanted)
            return EncryptionUnknown;

         const unsigned char propertyHeader = 0x01;
         const unsigned char propertyEncodedHeader = 0x17;

         if (header[0] == propertyHeader)
            return EncryptionNone;

         if (header[0] != propertyEncodedHeader)
            return EncryptionUnknown;

         const unsigned char aesMethod[4] = { 0x06, 0xF1, 0x07, 0x01 };
         for (size_t i = 0; i + sizeof(aesMethod) <= header.size(); i++)
         {
            if (memcmp(&header[i], aesMethod, sizeof(aesMethod)) == 0)
               return EncryptionHeaders;
         }

         return EncryptionNone;
      }
      catch (boost::thread_interrupted const &)
      {
         // The service is shutting down; Task::Run handles this one.
         throw;
      }
      catch (...)
      {
         return EncryptionUnknown;
      }
   }

   bool
   Compression::LaunchCommand_(const String &commandLine, const String &passphrase)
   {
      unsigned int exitCode = 0;
      last_run_timed_out_ = false;

      ProcessLauncher processLauncher(commandLine);

      // Always a standard input, even an empty one. See PassphraseInput_.
      processLauncher.SetStandardInput(PassphraseInput_(passphrase));

      // Only when a caller asked for one. Writing a backup does not, and must not:
      // see SetMaximumRunTime.
      if (maximum_run_time_ms_ > 0)
         processLauncher.SetMaximumRunTime(maximum_run_time_ms_);

      ProcessLauncher::FailureReason failureReason = ProcessLauncher::FailureReason::None;

      if (!processLauncher.Launch(exitCode, failureReason))
      {
         last_run_timed_out_ = failureReason == ProcessLauncher::FailureReason::TimedOut;
         return false;
      }

      if (exitCode != 0 && exitCode != 1)
         return false;

      return true;
   }

   String
   Compression::GetExecutableFullPath_()
   {
#ifdef HM_PLATFORM_POSIX
      // No 7za.exe ships in the Linux packages; the archiver is the system's,
      // under whichever name its package gives it: 7zz (7-Zip's own Linux
      // build), 7z or 7za (p7zip), 7zr (p7zip's reduced build). The first found
      // on PATH is used by its full path, so the log names the binary that ran.
      // With none found, the bare 7z goes to the launcher, whose failure then
      // names what is missing (HM5401) - the package recommends 7zip for this.
      const char *candidates[] = { "7zz", "7z", "7za", "7zr" };
      const char *environment = ::getenv("PATH");
      AnsiString directories = environment && *environment ? environment : "/usr/local/bin:/usr/bin:/bin";

      for (const char *candidate : candidates)
      {
         int start = 0;
         while (start <= directories.GetLength())
         {
            int end = directories.Find(":", start);
            if (end < 0)
               end = directories.GetLength();

            AnsiString directory = directories.Mid(start, end - start);
            start = end + 1;

            if (directory.IsEmpty())
               continue;

            AnsiString full = directory + "/" + candidate;
            if (::access(full.c_str(), X_OK) == 0)
               return String(full);
         }
      }

      return "7z";
#else
      const String ZipExecutable = "7za.exe";
      String binDir = IniFileSettings::Instance()->GetBinDirectory();
      return FileUtilities::Combine(binDir, ZipExecutable);
#endif
   }
}