// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#include "StdAfx.h"

#include "Compression.h"
#include "ProcessLauncher.h"
#include "FileUtilities.h"
#include "Unicode.h"

#include <boost/filesystem.hpp>

#include <fstream>
#include <set>
#include <sstream>
#include <string>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{

   Compression::Compression() :
      maximum_run_time_ms_(0),
      last_run_timed_out_(false),
      last_run_output_failed_(false)
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

      if (!WriteMemberList(listFile, members))
         return false;

      String commandLine = Formatter::Format("\"{0}\" x \"{1}\" \"@{2}\" -o\"{3}\" -y -scsUTF-8",
         GetExecutableFullPath_(), zipFile, listFile, targetDirectory);

      const bool result = LaunchCommand_(commandLine, passphrase);

      FileUtilities::DeleteFile(listFile);

      return result;
   }

   bool
   Compression::WriteMemberList(const String &listFile, const std::vector<String> &members)
   {
      {
         // A narrow path on POSIX: libstdc++'s streams take no wide one.
#ifdef HM_PLATFORM_POSIX
         const AnsiString narrowListFile(listFile);
         std::ofstream out(narrowListFile.c_str(), std::ios::binary | std::ios::trunc);
#else
         std::ofstream out(listFile.c_str(), std::ios::binary | std::ios::trunc);
#endif

         if (!out)
            return false;

         out.write("\xEF\xBB\xBF", 3);

         for (size_t i = 0; i < members.size() && out; i++)
         {
            AnsiString utf8;
            Unicode::WideToMultiByte(members[i], utf8);
            out.write(utf8.c_str(), utf8.GetLength());
            out.write("\r\n", 2);
         }

         // The close writes what is still buffered, and its answer is the only one
         // there is for the list's last lines: a stream's destructor closes it and
         // says nothing.
         out.close();
         if (!out.fail())
            return true;
      }

      FileUtilities::DeleteFile(listFile);
      return false;
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
   // Checked again against 26.03 on 25 September 2026: the same 62 bytes and id.
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

   namespace
   {
      // A POSIX mode as 7-Zip prints it, "lrwxrwxrwx", of a symbolic link: the
      // type letter first (kPosixTypes, where l is a link and no other type is),
      // then the nine permission letters.
      bool IsLinkMode_(const std::string &token)
      {
         if (token.size() != 10 || token[0] != 'l')
            return false;

         const std::string permissions = "-rwxsStT";

         for (size_t i = 1; i < token.size(); i++)
         {
            if (permissions.find(token[i]) == std::string::npos)
               return false;
         }

         return true;
      }

      // The value of an "Attributes = " line: 7-Zip's Windows attribute letters,
      // where L - and only L, of every letter it prints - is a reparse point, a
      // symbolic link or a junction among them ("DL" for a directory); then, for
      // an entry that carries one, a space and its POSIX mode, which p7zip 16.02
      // prints too ("A_ lrwxrwxrwx", "_ lrwxrwxrwx" for a zip's, measured on
      // 25 September 2026). The POSIX mode has no L, only the l that starts a
      // link's.
      bool HasReparseLetter_(const std::string &value)
      {
         const std::string letters = value.substr(0, value.find(' '));
         return letters.find('L') != std::string::npos;
      }

      // A POSIX link mode anywhere in the value of an "Attributes = " line, or of
      // a "Mode = " line (a tar entry's mode alone).
      bool HasPosixLinkMode_(const std::string &value)
      {
         std::istringstream tokens(value);
         std::string token;

         while (tokens >> token)
         {
            if (IsLinkMode_(token))
               return true;
         }

         return false;
      }

      bool ListingProperty_(const std::string &line, const char *name, std::string &value)
      {
         const std::string prefix = std::string(name) + " = ";

         if (line.compare(0, prefix.size(), prefix) != 0)
            return false;

         value = line.substr(prefix.size());
         return true;
      }

      // A "Size = " value, when it is a number.
      bool ListingSize_(const std::string &value, unsigned long long &size)
      {
         if (value.empty() || value.size() > 19)
            return false;

         size = 0;

         for (size_t i = 0; i < value.size(); i++)
         {
            if (value[i] < '0' || value[i] > '9')
               return false;

            size = size * 10 + (unsigned long long) (value[i] - '0');
         }

         return true;
      }
   }

   bool
   Compression::IsUnsafeEntryPath(const AnsiString &path)
   {
      const std::string text(path.c_str());

      if (text.empty())
         return false;

      // From the root: a leading separator, which is also how a UNC path
      // (\\server\share) and a Win32 device path (\\?\C:\...) begin.
      if (text[0] == '/' || text[0] == '\\')
         return true;

      // A drive, with a separator after it or not: "C:x" is relative to that
      // drive's current directory, which is no nearer the target.
      const char drive = text[0];
      if (text.size() >= 2 && text[1] == ':' && ((drive >= 'A' && drive <= 'Z') || (drive >= 'a' && drive <= 'z')))
         return true;

      // Each segment, between either separator: 7-Zip prints \ on Windows and
      // p7zip / on Linux. ".." climbs out, and so may any segment of nothing but
      // dots and spaces with two dots or more, because Windows drops the trailing
      // dots and spaces of a name. No backup this server writes has either.
      size_t start = 0;

      while (start <= text.size())
      {
         size_t end = text.find_first_of("/\\", start);
         if (end == std::string::npos)
            end = text.size();

         size_t dots = 0;
         bool onlyDotsAndSpaces = end > start;

         for (size_t i = start; i < end; i++)
         {
            if (text[i] == '.')
               dots++;
            else if (text[i] != ' ')
            {
               onlyDotsAndSpaces = false;
               break;
            }
         }

         if (onlyDotsAndSpaces && dots >= 2)
            return true;

         start = end + 1;
      }

      return false;
   }

   bool
   Compression::FindUnsafeEntry(std::istream &listing, AnsiString &unsafeEntry)
   {
      unsafeEntry = "";

      bool inEntries = false;
      bool haveEntry = false;
      std::string line;

      // What is known of the entry being read: its lines come one property each,
      // from its "Path = " line to the next entry's.
      std::string path;
      std::string reparseAttribute;
      std::string posixLink;
      bool haveSize = false;
      unsigned long long size = 0;
      std::set<std::string> properties;

      // A link is refused when it has a target to link to, and a link's target
      // is its data. A POSIX link (a mode starting l) is refused whenever it has
      // any: p7zip writes the entry out as a file and then makes a link of its
      // first line (convert_to_symlink), so "/etc" padded to any length with
      // NULs is a link to /etc - measured with p7zip 16.02 on 25 September 2026.
      // A Windows reparse point (L) is refused when its data is under 16 KiB,
      // the most a reparse buffer can be (MAXIMUM_REPARSE_DATA_BUFFER_SIZE);
      // 7-Zip makes one of nothing longer. With no data there is nothing to link
      // to: 7za 19.00, archiving a store that holds a junction, followed it and
      // kept the reparse bit on the directory it stored, which lists as "DL"
      // with "Size = 0" - an archive to restore, not to refuse. At 16 KiB or more
      // a reparse bit is on a file's content (a deduplicated or tiered file keeps
      // it). With no size in the listing at all, either is refused.
      auto linkWithTarget = [&]() -> std::string
      {
         if (!posixLink.empty() && (!haveSize || size > 0))
            return posixLink;

         if (!reparseAttribute.empty() && (!haveSize || (size > 0 && size < 16384)))
            return reparseAttribute;

         return std::string();
      };

      while (std::getline(listing, line))
      {
         if (!line.empty() && line[line.size() - 1] == '\r')
            line.erase(line.size() - 1);

         if (!inEntries)
         {
            // The archive's own block comes first - its path, which is wherever
            // the administrator keeps it and so usually absolute, its type and
            // its size - and ends at this line. Every line after it is an entry's.
            if (line == "----------")
               inEntries = true;

            continue;
         }

         std::string value;
         std::string problem;

         // What only an archive can have written into the listing. p7zip 16.02
         // prints a name or a comment as it is, newlines and all (measured: a zip
         // entry's comment "x\nSize = 65536" printed a second Size line), so a
         // property that comes twice in one entry, or a value on several lines -
         // a line starting "{" or a value that is "{" - is a listing that cannot
         // be read one way only. No backup of this server's has either.
         const size_t equals = line.find(" = ");
         const std::string name = equals == std::string::npos ? std::string() : line.substr(0, equals);

         if (!line.empty() && line[0] == '{')
            problem = "a listing with a value on several lines, which only the archive can have put there";
         else if (!name.empty() && line.compare(equals + 3, std::string::npos, "{") == 0)
            problem = "a listing whose " + name + " is on several lines, which only the archive can have put there";
         else if (haveEntry && !name.empty() && name != "Path" && !properties.insert(name).second)
            problem = "a listing with two " + name + " lines for one entry, which only the archive can have put there";
         else if (ListingProperty_(line, "Path", value))
         {
            // The entry before this one is complete.
            const std::string link = haveEntry ? linkWithTarget() : std::string();

            if (!link.empty())
            {
               unsafeEntry = AnsiString((path + " - a link (" + link + ")").c_str());
               return true;
            }

            haveEntry = true;
            path = value;
            reparseAttribute.clear();
            posixLink.clear();
            haveSize = false;
            size = 0;
            properties.clear();

            if (IsUnsafeEntryPath(AnsiString(value.c_str())))
               problem = "a path that leads out of the directory it is extracted to";
         }
         else if (ListingProperty_(line, "Size", value))
         {
            haveSize = ListingSize_(value, size);
         }
         else if (ListingProperty_(line, "Attributes", value))
         {
            if (HasReparseLetter_(value))
               reparseAttribute = "Attributes = " + value;

            if (HasPosixLinkMode_(value))
               posixLink = "Attributes = " + value;
         }
         else if (ListingProperty_(line, "Mode", value) || ListingProperty_(line, "Posix Attributes", value))
         {
            if (HasPosixLinkMode_(value))
               posixLink = "mode " + value;
         }
         else if (ListingProperty_(line, "Symbolic Link", value) || ListingProperty_(line, "Hard Link", value) ||
                  ListingProperty_(line, "Copy Link", value) || ListingProperty_(line, "Link", value))
         {
            // Named outright (a tar's link, whose target is in its header rather
            // than its data), so refused whatever its size.
            if (!value.empty())
               problem = "a link to " + value;
         }

         if (!problem.empty())
         {
            // One is enough: the archive is refused whole at the first, and the
            // administrator is told which.
            unsafeEntry = AnsiString((path + " - " + problem).c_str());
            return true;
         }
      }

      const std::string link = haveEntry ? linkWithTarget() : std::string();

      if (!link.empty())
      {
         unsafeEntry = AnsiString((path + " - a link (" + link + ")").c_str());
         return true;
      }

      return false;
   }

   Compression::EntryCheck
   Compression::CheckEntries(const String &zipFile, const String &scratchDirectory, const String &passphrase, String &unsafeEntry)
   {
      unsafeEntry.Empty();

      const String listingFile = FileUtilities::Combine(scratchDirectory, _T("hMailServer-entries.txt"));

      // l -slt: every property of every entry, one "Name = value" line each,
      // which is what FindUnsafeEntry reads; the plain listing's attribute column
      // has no room for a link. -sccUTF-8 on Windows, where 7-Zip otherwise writes
      // a name in the console's code page: the rule needs only the ASCII in it,
      // but the entry named in a refusal should read as it was written. Not on
      // POSIX, where the output is the locale's (UTF-8 on any server) and p7zip's
      // reading of -scc is not one to depend on. Checked against 7za 26.03.
#ifdef HM_PLATFORM_POSIX
      String commandLine = Formatter::Format("\"{0}\" l \"{1}\" -slt",
         GetExecutableFullPath_(), zipFile);
#else
      String commandLine = Formatter::Format("\"{0}\" l \"{1}\" -slt -sccUTF-8",
         GetExecutableFullPath_(), zipFile);
#endif

      const bool listed = LaunchCommand_(commandLine, passphrase, listingFile);

      bool read = false;
      bool unsafe = false;
      AnsiString found;

      if (listed)
      {
         // In a scope of its own, so that the file is closed before it is deleted:
         // on Windows an open file cannot be, and DeleteFile would retry for five
         // seconds and then report HM5047.
         try
         {
            const boost::filesystem::path path(listingFile.c_str());
            std::ifstream listing(path.c_str(), std::ios::in | std::ios::binary);

            if (listing.is_open())
            {
               read = true;
               unsafe = FindUnsafeEntry(listing, found);
            }
         }
         catch (boost::thread_interrupted const &)
         {
            // The service is shutting down; Task::Run handles this one.
            throw;
         }
         catch (...)
         {
            read = false;
         }
      }

      FileUtilities::DeleteFile(listingFile);

      // The listing had nowhere to go: a fault of the scratch directory's, not
      // the archive's, and said as such (HM7541 has said why).
      if (!listed && last_run_output_failed_)
         return EntriesListingNotWritten;

      if (!listed || !read)
         return EntriesUnlisted;

      if (unsafe)
      {
         Unicode::MultiByteToWide(found, unsafeEntry);
         return EntriesUnsafe;
      }

      return EntriesSound;
   }

   bool
   Compression::LaunchCommand_(const String &commandLine, const String &passphrase, const String &outputFile)
   {
      unsigned int exitCode = 0;
      last_run_timed_out_ = false;
      last_run_output_failed_ = false;

      ProcessLauncher processLauncher(commandLine);

      // Always a standard input, even an empty one. See PassphraseInput_.
      processLauncher.SetStandardInput(PassphraseInput_(passphrase));

      // What the archiver prints, when the caller reads it (CheckEntries).
      if (!outputFile.IsEmpty())
         processLauncher.SetStandardOutputFile(outputFile);

      // Only when a caller asked for one. Writing a backup does not, and must not:
      // see SetMaximumRunTime.
      if (maximum_run_time_ms_ > 0)
         processLauncher.SetMaximumRunTime(maximum_run_time_ms_);

      ProcessLauncher::FailureReason failureReason = ProcessLauncher::FailureReason::None;

      if (!processLauncher.Launch(exitCode, failureReason))
      {
         last_run_timed_out_ = failureReason == ProcessLauncher::FailureReason::TimedOut;
         last_run_output_failed_ = failureReason == ProcessLauncher::FailureReason::OutputFileFailed;
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

   namespace
   {
      // A listing as 7za 26.03 writes one with -slt: the archive's own block,
      // whose path is absolute and is not an entry, then the entries given.
      bool ListingRefused_(const std::string &entries, AnsiString &named)
      {
         // REUSE-IgnoreStart: 7-Zip's banner, as 7za prints it, not this file's notice.
         const std::string listing =
            "\r\n7-Zip (a) 26.03 (x64) : Copyright (c) 1999-2026 Igor Pavlov : 2026-09-03\r\n\r\n"
            "Scanning the drive for archives:\r\n1 file, 240 bytes (1 KiB)\r\n\r\n"
            "Listing archive: C:\\Backups\\HMBackup 2026-09-25 101500.7z\r\n\r\n--\r\n"
            "Path = C:\\Backups\\HMBackup 2026-09-25 101500.7z\r\nType = 7z\r\nPhysical Size = 240\r\n"
            "Headers Size = 216\r\nMethod = LZMA2:12\r\nSolid = -\r\nBlocks = 2\r\n\r\n"
            "----------\r\n" + entries;
         // REUSE-IgnoreEnd

         std::istringstream stream(listing);
         return Compression::FindUnsafeEntry(stream, named);
      }

      // One entry's lines, as a 7z archive's are listed.
      std::string EntryLines_(const std::string &path, const std::string &size, const std::string &attributes)
      {
         return "Path = " + path + "\r\nSize = " + size + "\r\nPacked Size = 0\r\nModified = 2026-09-25 14:20:14.5191295\r\n"
            "Attributes = " + attributes + "\r\nCRC = \r\nEncrypted = -\r\nMethod = LZMA2:12\r\nBlock = 0\r\n\r\n";
      }

      void ExpectListing_(bool refused, const std::string &entries, const std::string &what)
      {
         AnsiString named;

         if (ListingRefused_(entries, named) == refused)
            return;

         throw std::logic_error("Compression::FindUnsafeEntry " + std::string(refused ? "let through " : "refused ") + what +
            (named.IsEmpty() ? std::string() : " (it named " + std::string(named.c_str()) + ")"));
      }
   }

   void
   CompressionTester::Test()
   {
      // What a backup holds: the index, the message store's directories and
      // files. Nothing refused - and not the archive's own absolute path above
      // the entries either.
      const std::string backup =
         EntryLines_("DataBackup", "0", "D") +
         EntryLines_("DataBackup\\example.test", "0", "D") +
         EntryLines_("DataBackup\\example.test\\user\\{3A1B}\\{3A1B2C3D-0000-4000-8000-000000000001}.eml", "6", "A") +
         EntryLines_("hMailServerBackup.xml", "10", "A");
      ExpectListing_(false, backup, "an ordinary backup");

      AnsiString named;
      if (!ListingRefused_(backup + EntryLines_("..\\escaped.eml", "6", "A"), named) || named.Find("..\\escaped.eml") < 0)
         throw std::logic_error("Compression::FindUnsafeEntry did not name the entry it refused: " + std::string(named.c_str()));

      // Out of the target: what 7za rn and -spf make.
      ExpectListing_(true, EntryLines_("..\\escaped.eml", "6", "A"), "an entry named ..\\escaped.eml");
      ExpectListing_(true, EntryLines_("DataBackup\\..\\..\\escaped.eml", "6", "A"), "a .. segment inside a path");
      ExpectListing_(true, EntryLines_("DataBackup/../../escaped.eml", "6", "A"), "a .. segment with forward slashes (p7zip's listing)");
      ExpectListing_(true, EntryLines_("C:\\Windows\\System32\\evil.dll", "6", "A"), "an absolute path with a drive");
      ExpectListing_(true, EntryLines_("c:evil.dll", "6", "A"), "a path relative to a drive");
      ExpectListing_(true, EntryLines_("\\rooted.xml", "6", "A"), "a path from the root");
      ExpectListing_(true, EntryLines_("/etc/cron.d/evil", "6", "A"), "a POSIX absolute path");
      ExpectListing_(true, EntryLines_("\\\\server\\share\\evil", "6", "A"), "a UNC path");
      ExpectListing_(true, EntryLines_("DataBackup\\.. \\escaped.eml", "6", "A"), "a segment Windows trims to ..");
      ExpectListing_(true, EntryLines_("..", "0", "D"), "a directory named ..");

      // Names that only look like it.
      ExpectListing_(false, EntryLines_("DataBackup\\a..b\\..eml", "6", "A"), "dots inside names");
      ExpectListing_(false, EntryLines_("DataBackup\\.\\.hidden", "6", "A"), "a single-dot segment");

      // Links: a symbolic link or a junction stored with its target (7z, from
      // Windows or from Linux), and the ones a tar names outright.
      ExpectListing_(true, EntryLines_("link.txt", "72", "AL"), "a reparse point with data");
      ExpectListing_(true, EntryLines_("DataBackup\\example.test", "40", "DL"), "a junction with its target");
      ExpectListing_(true, EntryLines_("link", "11", "A lrwxrwxrwx"), "a POSIX symbolic link");
      ExpectListing_(true, "Path = link\r\nMode = lrwxrwxrwx\r\nAttributes = \r\n\r\n", "a link with no size in the listing");
      ExpectListing_(true, "Path = evil\r\nFolder = -\r\nSize = 10\r\nMode = lrw-r--r--\r\nSymbolic Link = C:/Windows\r\nHard Link = \r\n\r\n",
         "a tar's symbolic link");
      ExpectListing_(true, "Path = hard\r\nFolder = -\r\nSize = 0\r\nMode = -rw-r--r--\r\nSymbolic Link = \r\nHard Link = /etc/shadow\r\n\r\n",
         "a tar's hard link");

      // Not links: the bit with nothing to link to (7za 19.00 kept it on a
      // directory it reached through a junction and stored as a directory), a
      // file of 16 KiB or more that keeps it (deduplicated or tiered), and the
      // empty link properties every tar entry lists.
      ExpectListing_(false, backup + EntryLines_("DataBackup\\moved.test", "0", "DL"), "a directory stored through a junction");
      ExpectListing_(false, EntryLines_("DataBackup\\big.eml", "65536", "AL"), "a large file that keeps the reparse bit");
      ExpectListing_(false, EntryLines_("file", "6", "A -rw-r--r--"), "a POSIX regular file");
      ExpectListing_(false, "Path = file\r\nFolder = -\r\nSize = 9\r\nMode = -rw-r--r--\r\nSymbolic Link = \r\nHard Link = \r\n\r\n",
         "a tar's ordinary file");

      // The last entry is judged at the end of the listing, and LF alone is read
      // as a line end as well as CRLF.
      ExpectListing_(true, "Path = last\nSize = 20\nAttributes = AL\n", "a link as the last entry, with LF line ends");

      // p7zip 16.02's forms (measured): the mode after "A_" or "_", and a POSIX
      // link refused at any size, since p7zip links to the data's first line -
      // "/etc" padded with NULs to 20000 bytes made a link to /etc.
      ExpectListing_(true, EntryLines_("s/link", "4", "A_ lrwxrwxrwx"), "p7zip's link in a 7z");
      ExpectListing_(true, EntryLines_("padded-link", "20000", "_ lrwxrwxrwx"), "a POSIX link padded past 16 KiB");
      ExpectListing_(false, EntryLines_("s/file.txt", "6", "A_ -rw-r--r--"), "p7zip's ordinary file");

      // What a raw name or comment writes into p7zip 16.02's listing: a second
      // Size after a reparse point's (a zip comment "x\nSize = 65536"), a Size
      // before the real one (a name "named\nSize = 0"), a value on several lines.
      ExpectListing_(true, "Path = x\r\nSize = 100\r\nAttributes = AL\r\nComment = y\r\nSize = 65536\r\n\r\n",
         "a Size written into the listing by a comment");
      ExpectListing_(true, "Path = named\r\nSize = 0\r\nSize = 4\r\nAttributes = _ lrwxrwxrwx\r\n\r\n",
         "a Size written into the listing by a name");
      ExpectListing_(true, EntryLines_("a", "1", "A") + "Path = b\r\nComment = {\r\nx\r\n}\r\n", "a value on several lines");
      ExpectListing_(true, EntryLines_("a", "1", "A") + "{\r\n", "a line starting {");

      // Before the separator nothing is an entry: a listing with none.
      std::istringstream noEntries("Path = /etc/passwd\r\nType = 7z\r\n");
      AnsiString none;
      if (Compression::FindUnsafeEntry(noEntries, none))
         throw std::logic_error("Compression::FindUnsafeEntry read the archive's own block as an entry.");

      if (Compression::IsUnsafeEntryPath("") || Compression::IsUnsafeEntryPath("hMailServerBackup.xml") ||
          !Compression::IsUnsafeEntryPath("a\\..") || !Compression::IsUnsafeEntryPath("a/../b"))
         throw std::logic_error("Compression::IsUnsafeEntryPath misjudged a bare path.");
   }
}