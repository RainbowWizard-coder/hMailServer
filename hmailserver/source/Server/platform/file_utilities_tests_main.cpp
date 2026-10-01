// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// A written file's close, checked: File::Close and the writers of stored mail
// that take its answer, as a program of their own for the POSIX build - the
// counterpart of spf-tests and backup-tests. linux-build.sh runs it with every
// other *-tests program.
//
// fwrite answers for the bytes it takes into the C runtime's buffer, and fclose
// writes what is still there before it closes: for a file that was written, the
// close's answer is the only report of whether its last bytes - up to a buffer,
// the file system's block size with glibc, 4 KB on most - reached the file.
// File::Close used to drop it, so a full disk or a quota left a message file
// short while every writer took it for whole. Each case here gives a writer a
// file that cannot take its last bytes and asks it to say so:
//
//   * FileUtilitiesTester::TestFailedClose, on /dev/full, which refuses every
//     write with ENOSPC: File::Close and FileUtilities::WriteToFile. And the
//     spool a received message is written to - TransparentTransmissionBuffer,
//     which SMTP's DATA and BDAT and both fetchers write through - on /dev/full
//     and, as the control, on a file that takes it whole.
//   * A quota, the process's own RLIMIT_FSIZE, with SIGXFSZ ignored so that a
//     write past it fails with EFBIG instead of ending the process: File::Close,
//     WriteToFile, TraceHeaderWriter::Write - which rewrites a stored message
//     through a copy and moves the copy over it - the message store's
//     WriteAtomically, the spool, and FileUtilities::CopyOver, which puts a
//     SpamAssassin result in a message's place. The header writer, the store and
//     CopyOver must leave the message as it was and nothing beside it, where they
//     used to put a short copy in its place; the spool must count the message
//     as not written, which is what refuses it.
//   * A full disk: a tmpfs of 64 KB filled to its last page, in a mount
//     namespace of a child process's own, and the same writers on it. It
//     needs the right to mount, which root has on a workstation and a container
//     is usually not given; without it the case says it did not run, and the
//     other two, which need nothing, still do.
//
// It prints a line for each and exits non-zero if one failed.

#include <cerrno>
#include <cstdio>
#include <exception>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <boost/filesystem.hpp>

#include "../Common/Application/IniFileSettings.h"
#include "../Common/Util/File.h"
#include "../Common/Util/FileUtilities.h"
#include "../Common/Util/TraceHeaderWriter.h"
#include "../Common/Util/TransparentTransmissionBuffer.h"
#include "../Common/Persistence/MessageStore.h"

namespace
{
   using HM::AnsiString;
   using HM::String;

   // More than the quota below and less than the smallest stdio buffer this runs
   // with (musl's is 1 KB), so every write of it is buffered and only the close
   // meets the limit.
   const size_t PayloadBytes = 900;
   const rlim_t QuotaBytes = 512;

   // What the exit status of the full-disk child means.
   const int ChildPassed = 0;
   const int ChildFailed = 1;
   const int ChildCannotMount = 77;

   void Check(bool condition, const std::string &what)
   {
      if (!condition)
         throw std::logic_error(what);
   }

   // Where ErrorManager's reports go while this runs: a log directory of the
   // program's own (main), named by a configuration file of its own. Without one
   // the log directory is empty and the error log is written at the root of the
   // file system.
   boost::filesystem::path LogDirectory;

   // How many times the error log says the code given (HM5862, say).
   int ReportsOf(const std::string &code)
   {
      int count = 0;
      boost::system::error_code error;
      for (boost::filesystem::directory_iterator entry(LogDirectory, error);
           !error && entry != boost::filesystem::directory_iterator(); entry.increment(error))
      {
         std::ifstream in(entry->path().string().c_str(), std::ios::binary);
         const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
         for (size_t at = text.find("Code: " + code); at != std::string::npos; at = text.find("Code: " + code, at + 1))
            count++;
      }
      return count;
   }

   AnsiString Payload()
   {
      std::string text;
      for (size_t i = 0; text.size() < PayloadBytes; i++)
         text += "line " + std::to_string(i) + " of a message that is either whole or refused\r\n";
      text.resize(PayloadBytes);
      return AnsiString(text.c_str(), text.size());
   }

   // A SpamAssassin result: a message with spamd's headers in front, larger than
   // two pages. Copied onto a message of one page on the full disk, it gets back
   // the page the copy frees by emptying the message, and needs three.
   const size_t ResultBytes = 9000;

   AnsiString Result()
   {
      std::string text = "X-Spam-Checker-Version: SpamAssassin\r\nX-Spam-Status: No\r\n";
      for (size_t i = 0; text.size() < ResultBytes; i++)
         text += "line " + std::to_string(i) + " of a result that replaces a message only whole\r\n";
      text.resize(ResultBytes);
      return AnsiString(text.c_str(), text.size());
   }

   String Wide(const boost::filesystem::path &path)
   {
      return String(path.wstring());
   }

   unsigned long long SizeOf(const boost::filesystem::path &path)
   {
      boost::system::error_code error;
      const boost::uintmax_t size = boost::filesystem::file_size(path, error);
      return error ? (unsigned long long) -1 : (unsigned long long) size;
   }

   // Empty when the file cannot be read, which no stored message here is.
   std::string ReadAll(const boost::filesystem::path &path)
   {
      std::ifstream in(path.string().c_str(), std::ios::binary);
      return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
   }

   // Whether a directory holds anything but the names given.
   bool HoldsOnly(const boost::filesystem::path &directory, const std::vector<std::string> &names)
   {
      boost::system::error_code error;
      for (boost::filesystem::directory_iterator entry(directory, error);
           !error && entry != boost::filesystem::directory_iterator(); entry.increment(error))
      {
         const std::string name = entry->path().filename().string();
         bool expected = false;
         for (const std::string &allowed : names)
            expected = expected || name == allowed;
         if (!expected)
            return false;
      }
      return !error;
   }

   void WriteWhole(const boost::filesystem::path &path, const AnsiString &bytes)
   {
      FILE *file = fopen(path.string().c_str(), "wb");
      if (file == nullptr)
         throw std::logic_error("the stored message could not be made");
      const bool written = fwrite(bytes.c_str(), 1, (size_t) bytes.GetLength(), file) == (size_t) bytes.GetLength();
      const bool closed = fclose(file) == 0;
      Check(written && closed, "the stored message could not be made");
   }

   // A received message through the spool it is written to: SMTP's DATA and BDAT
   // and both fetchers write through TransparentTransmissionBuffer, and what it
   // answers to GetWriteFailed is what refuses the message - SMTP's 451, a fetch
   // that leaves the message on the remote server. In the byte-transparent mode
   // BDAT and the IMAP fetcher use: the other mode reads the server's SMTP
   // settings, which this program has none of, and ends in the same close.
   // Answers whether the message was counted as not written.
   bool SpoolWriteFailed(const String &path, const AnsiString &bytes)
   {
      // A configured fsync fails first on a file that refuses its bytes, and would
      // refuse the message before the close is reached.
      Check(!HM::IniFileSettings::Instance()->GetMessageStoreFsync(), "MessageStoreFsync is on, so the spool's flush and not its close would be what refuses");

      HM::TransparentTransmissionBuffer spool(false);
      spool.SetBinaryMode(true);
      Check(spool.Initialize(path), "the spool file could not be opened");
      spool.Append(reinterpret_cast<const BYTE *>(bytes.c_str()), (size_t) bytes.GetLength());
      spool.MarkTransmissionEnded();
      spool.Flush(true);
      return spool.GetWriteFailed();
   }

   // The writers against a directory whose file system will not take the last
   // bytes of what they write. `message` was stored there before the file system
   // was made to refuse, and must come out as it went in; `result`, a SpamAssassin
   // result put there before too, is what CopyOver is asked to put in its place.
   void WritersRefuse(const boost::filesystem::path &directory, const boost::filesystem::path &message, const std::string &original,
                      const boost::filesystem::path &result, const char *where)
   {
      const AnsiString payload = Payload();
      const std::string place = std::string(" (") + where + ")";

      // File itself: the write is buffered and answers true, the close does not.
      const boost::filesystem::path plain = directory / "plain.eml";
      {
         HM::File file;
         Check(file.Open(Wide(plain), HM::File::OTCreate), "File::Open failed" + place);
         Check(file.Write(payload), "File::Write answered false, so the close is not what is tested" + place);
         Check(!file.Close(), "File::Close answered true for a file that did not take its last bytes" + place);
      }
      Check(SizeOf(plain) < PayloadBytes, "the file took every byte, so nothing was refused" + place);
      boost::filesystem::remove(plain);

      // WriteToFile, which answers with the close.
      const boost::filesystem::path written = directory / "written.txt";
      Check(!HM::FileUtilities::WriteToFile(Wide(written), payload), "FileUtilities::WriteToFile answered true for a file that did not take its last bytes" + place);
      boost::filesystem::remove(written);

      // TraceHeaderWriter: a header in front of a stored message, through a copy
      // moved over it. A copy whose close failed is short, and must not be moved.
      {
         std::vector<std::pair<AnsiString, AnsiString> > fields;
         fields.push_back(std::make_pair(AnsiString("Return-Path"), AnsiString("<sender@example.test>")));

         HM::TraceHeaderWriter writer;
         Check(!writer.Write(Wide(message), std::shared_ptr<HM::Message>(), fields), "TraceHeaderWriter::Write answered true for a copy that did not take its last bytes" + place);
      }
      Check(ReadAll(message) == original, "TraceHeaderWriter::Write changed the stored message although its copy was refused" + place);

      // The message store's WriteAtomically: a temporary beside the name, renamed
      // over it. A temporary whose close failed must not take the name.
      const boost::filesystem::path stored = directory / "stored.eml";
      Check(!HM::MessageStore::LocalBackend().WriteAtomically(Wide(stored), payload), "MessageStore's WriteAtomically answered true for a file that did not take its last bytes" + place);
      Check(!boost::filesystem::exists(stored), "MessageStore's WriteAtomically left a short file under the name" + place);

      // The spool of a received message: every write lands in the buffer, the
      // close does not, and the message must be counted as not written.
      const boost::filesystem::path spool = directory / "spool.eml";
      Check(SpoolWriteFailed(Wide(spool), payload), "TransparentTransmissionBuffer counted a spool file that did not take its last bytes as written" + place);
      boost::filesystem::remove(spool);

      // CopyOver: a SpamAssassin result put in the message's place. A copy made
      // onto the message emptied it first, and one the disk refused left it short.
      Check(!HM::FileUtilities::CopyOver(Wide(result), Wide(message)), "FileUtilities::CopyOver answered true for a copy that was refused" + place);
      Check(ReadAll(message) == original, "FileUtilities::CopyOver changed the stored message (" + std::to_string(SizeOf(message)) + " bytes now) although its copy was refused" + place);

      // Nothing left behind: no copy, no temporary.
      Check(HoldsOnly(directory, { message.filename().string(), result.filename().string(), "filler" }), "a writer left a file behind" + place);
   }

   // A quota: this process may not make a file longer than QuotaBytes. The
   // write past it fails with EFBIG once SIGXFSZ, which would end the process,
   // is ignored. Both are put back before this returns.
   void QuotaCase()
   {
      const boost::filesystem::path directory = boost::filesystem::temp_directory_path() /
         boost::filesystem::unique_path("hm-close-quota-%%%%-%%%%");
      boost::filesystem::create_directories(directory);

      const boost::filesystem::path message = directory / "message.eml";
      const std::string original(Payload().c_str(), PayloadBytes);
      WriteWhole(message, Payload());
      const boost::filesystem::path result = directory / "result.eml";
      WriteWhole(result, Result());

      struct rlimit before = {};
      Check(getrlimit(RLIMIT_FSIZE, &before) == 0, "getrlimit(RLIMIT_FSIZE) failed");
      Check(before.rlim_max == RLIM_INFINITY || before.rlim_max > QuotaBytes, "the hard file size limit is already below the quota");

      struct sigaction ignore = {};
      struct sigaction previous = {};
      ignore.sa_handler = SIG_IGN;
      sigemptyset(&ignore.sa_mask);
      Check(sigaction(SIGXFSZ, &ignore, &previous) == 0, "SIGXFSZ could not be ignored");

      struct rlimit quota = before;
      quota.rlim_cur = QuotaBytes;
      Check(setrlimit(RLIMIT_FSIZE, &quota) == 0, "setrlimit(RLIMIT_FSIZE) failed");

      std::string failure;
      try
      {
         WritersRefuse(directory, message, original, result, "a quota");
      }
      catch (const std::exception &e)
      {
         failure = e.what();
      }

      const bool restored = setrlimit(RLIMIT_FSIZE, &before) == 0 && sigaction(SIGXFSZ, &previous, nullptr) == 0;

      boost::system::error_code ignored;
      boost::filesystem::remove_all(directory, ignored);

      Check(restored, "the file size limit or SIGXFSZ could not be put back");
      Check(failure.empty(), failure);
   }

   // A full disk, in a child process: its own mount namespace, a tmpfs of 64 KB
   // there, the stored message put on it, and the rest filled to its last byte.
   // Answers the child's exit status.
   int FullDiskChild()
   {
      if (unshare(CLONE_NEWNS) != 0)
         return ChildCannotMount;

      // Nothing mounted here is seen outside this process.
      if (mount("none", "/", nullptr, MS_REC | MS_PRIVATE, nullptr) != 0)
         return ChildCannotMount;

      char pattern[] = "/tmp/hm-close-disk-XXXXXX";
      if (mkdtemp(pattern) == nullptr)
         return ChildCannotMount;

      const boost::filesystem::path directory(pattern);
      if (mount("tmpfs", pattern, "tmpfs", MS_NOSUID | MS_NODEV, "size=64k,mode=0700") != 0)
      {
         rmdir(pattern);
         return ChildCannotMount;
      }

      int status = ChildPassed;
      try
      {
         const boost::filesystem::path message = directory / "message.eml";
         const std::string original(Payload().c_str(), PayloadBytes);
         WriteWhole(message, Payload());
         const boost::filesystem::path result = directory / "result.eml";
         WriteWhole(result, Result());

         // Filled a page at a time, then a byte at a time into the last page, until
         // the file system refuses: after that no write that needs a byte of space
         // succeeds.
         const std::string filler = (directory / "filler").string();
         const int fd = open(filler.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
         Check(fd >= 0, "the filler could not be made");
         std::vector<char> page(4096, 'f');
         while (write(fd, page.data(), page.size()) == (ssize_t) page.size())
         {
         }
         while (write(fd, page.data(), 1) == 1)
         {
         }
         const int filled = errno;
         close(fd);
         Check(filled == ENOSPC, "the tmpfs was not filled: the last write failed with errno " + std::to_string(filled) + ", not ENOSPC");

         WritersRefuse(directory, message, original, result, "a full tmpfs");
      }
      catch (const std::exception &e)
      {
         std::printf("FAIL  a full disk: %s\n", e.what());
         status = ChildFailed;
      }

      // The mount goes with this process's namespace whatever these answer; they
      // only tidy the directory under /tmp. What was printed is flushed before the
      // child ends with _exit, which flushes nothing.
      if (umount(pattern) != 0 || rmdir(pattern) != 0)
         std::printf("note  the scratch mount point %s could not be removed\n", pattern);
      if (std::fflush(stdout) != 0)
         return ChildFailed;
      return status;
   }

   int Run(const char *name, void (*test)())
   {
      try
      {
         test();
         std::printf("ok    %s\n", name);
         return 0;
      }
      catch (const std::exception &e)
      {
         std::printf("FAIL  %s: %s\n", name, e.what());
         return 1;
      }
   }

   void DevFullCase()
   {
      HM::FileUtilitiesTester tester;
      tester.TestFailedClose();
   }

   // The spool on a file that takes every byte, which is the control for the rest:
   // counted as written, and holding the message. Then on /dev/full, where every
   // write lands in the buffer and the close that writes it is refused.
   void SpoolCase()
   {
      const boost::filesystem::path directory = boost::filesystem::temp_directory_path() /
         boost::filesystem::unique_path("hm-close-spool-%%%%-%%%%");
      boost::filesystem::create_directories(directory);

      const boost::filesystem::path spool = directory / "spool.eml";
      const bool failed = SpoolWriteFailed(Wide(spool), Payload());
      const std::string held = ReadAll(spool);

      boost::system::error_code ignored;
      boost::filesystem::remove_all(directory, ignored);

      Check(!failed, "a spool file that took every byte was counted as not written");
      Check(held == std::string(Payload().c_str(), PayloadBytes), "the spool file does not hold the message written to it");
      Check(ReportsOf("HM5862") == 0, "a spool file that took every byte was reported as not written (HM5862)");

      Check(SpoolWriteFailed(_T("/dev/full"), Payload()), "the spool on /dev/full, whose close is refused, was counted as written");

      // Said, and once for the message (HM5862), as a write that failed is.
      const int reports = ReportsOf("HM5862");
      Check(reports == 1, "the spool on /dev/full was reported " + std::to_string(reports) + " time(s) as HM5862, not once");
   }

   // CopyOver where the disk takes the copy: the message is replaced by the whole
   // result, and nothing is left beside it.
   void CopyOverCase()
   {
      const boost::filesystem::path directory = boost::filesystem::temp_directory_path() /
         boost::filesystem::unique_path("hm-close-copy-%%%%-%%%%");
      boost::filesystem::create_directories(directory);

      const boost::filesystem::path message = directory / "message.eml";
      const boost::filesystem::path result = directory / "result.eml";
      WriteWhole(message, Payload());
      WriteWhole(result, Result());

      const bool copied = HM::FileUtilities::CopyOver(Wide(result), Wide(message));
      const std::string held = ReadAll(message);
      const bool alone = HoldsOnly(directory, { message.filename().string(), result.filename().string() });

      boost::system::error_code ignored;
      boost::filesystem::remove_all(directory, ignored);

      Check(copied, "FileUtilities::CopyOver answered false where the disk took the copy");
      Check(held == std::string(Result().c_str(), ResultBytes), "FileUtilities::CopyOver did not put the whole result in the message's place");
      Check(alone, "FileUtilities::CopyOver left a file beside the message");
   }
}

int main()
{
   int failed = 0;

   // A configuration of this program's own, naming a log directory under the temp
   // directory (LogDirectory, above), removed at the end. It names nothing else, so
   // every setting the writers read is its default.
   const boost::filesystem::path scratch = boost::filesystem::temp_directory_path() /
      boost::filesystem::unique_path("hm-close-config-%%%%-%%%%");
   LogDirectory = scratch / "logs";
   boost::system::error_code made;
   boost::filesystem::create_directories(LogDirectory, made);
   const boost::filesystem::path configuration = scratch / "hMailServer.ini";
   {
      std::ofstream ini(configuration.string().c_str(), std::ios::binary);
      ini << "[Directories]\nLogFolder=" << LogDirectory.string() << "\n";
      ini.close();
      if (made || !ini)
      {
         std::printf("FAIL  the program's own configuration could not be written in %s\n", scratch.string().c_str());
         return 1;
      }
   }
   HM::IniFileSettings::Instance()->SetInitializationFile(Wide(configuration));

   failed += Run("a written file's close: a whole file closes true, and /dev/full's close answers false through File::Close and FileUtilities::WriteToFile", DevFullCase);
   failed += Run("the spool of a received message: a file that takes every byte is written whole, and on /dev/full the message is counted as not written", SpoolCase);
   failed += Run("FileUtilities::CopyOver puts the whole of a file in another's place and leaves nothing beside it", CopyOverCase);
   failed += Run("a quota (RLIMIT_FSIZE): File::Close, WriteToFile, TraceHeaderWriter::Write, the message store's WriteAtomically, the spool and CopyOver refuse a file short of its last bytes, and leave the stored message as it was", QuotaCase);

   // Flushed before the fork, or the child would print the lines above again.
   if (std::fflush(stdout) != 0)
      return 1;
   const pid_t child = fork();
   if (child == 0)
      _exit(FullDiskChild());

   int status = 0;
   if (child < 0 || waitpid(child, &status, 0) != child || !WIFEXITED(status))
   {
      std::printf("FAIL  a full disk: the child that makes one did not finish\n");
      failed++;
   }
   else if (WEXITSTATUS(status) == ChildCannotMount)
   {
      std::printf("not run: a full disk - this process may not mount a tmpfs of its own (it needs CAP_SYS_ADMIN); the quota above is the same refusal\n");
   }
   else if (WEXITSTATUS(status) == ChildPassed)
   {
      std::printf("ok    a full disk (a 64 KB tmpfs filled to its last byte): File::Close, WriteToFile, TraceHeaderWriter::Write, the message store's WriteAtomically, the spool and CopyOver refuse, and leave the stored message as it was\n");
   }
   else
   {
      failed++;
   }

   boost::system::error_code ignored;
   boost::filesystem::remove_all(scratch, ignored);

   return failed == 0 ? 0 : 1;
}
