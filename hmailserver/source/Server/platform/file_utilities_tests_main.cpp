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
// And three more writers, under the quota and on the full disk alike, each with
// its control where the disk takes it: the member list a backup restore hands
// 7-Zip (Compression::WriteMemberList), the file HttpsClient::Download streams a
// body into, from a loopback server of this program's own, and
// FileUtilities::MoveOver, which puts a SpamAssassin result that is on another
// file system in a message's place - from /dev/shm under the quota, from the
// temporary directory onto the full tmpfs - where a move between volumes used
// to be a copy onto the message. The download needs a loopback socket, which a
// source package's build does not give: it runs this under
// build/ci/packages/offline.c, where making any IPv4 socket fails with EACCES.
// There the download says it did not run, and the rest still does; a socket
// refused for any other reason fails the case.
//
// And a file's name: one with characters outside ASCII is one file to File::Open,
// FileUtilities, Boost.Filesystem, stat() and a directory's listing, all in UTF-8
// whatever LANG says (FileUtilitiesTester::TestFileNameEncoding). Up to 6.3.7
// File::Open named it in Latin-1, and Boost.Filesystem in the locale's encoding,
// which threw for it with LANG unset and on musl. And a locale the machine does
// not have, named by LC_ALL, changes nothing: this program run again under one
// reaches main (MissingLocaleCase).
//
// It prints a line for each and exits non-zero if one failed.

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>

#include <boost/filesystem.hpp>

#include "../Common/Application/IniFileSettings.h"
#include "../Common/Util/Compression.h"
#include "../Common/Util/File.h"
#include "../Common/Util/HttpsClient.h"
#include "../Common/Util/FileUtilities.h"
#include "../Common/Util/TraceHeaderWriter.h"
#include "../Common/Util/TransparentTransmissionBuffer.h"
#include "../Common/Persistence/MessageStore.h"
#include "../Common/BO/MessageData.h"
#include "../Common/Util/Crypt.h"
#include "../Common/Util/Unicode.h"
#include "../Common/Util/UpdateInstaller.h"

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

   // Whether this process is refused every IPv4 socket, as under offline.c, which
   // fails socket(2) with EACCES (main asks once, before the cases). Then the
   // download, which needs a loopback server, is not run.
   bool SocketsRefused = false;

   // ", a download" where the download runs, nothing where it does not: for the
   // cases' names, which list what they ran.
   std::string ADownload()
   {
      return SocketsRefused ? std::string() : std::string(", a download");
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

   // The members a backup restore asks 7-Zip for: more than the quota and less than
   // a stream's buffer, so every line lands in the buffer and only the close meets
   // the limit.
   std::vector<String> Members()
   {
      std::vector<String> members;
      for (int i = 0; i < 24; i++)
         members.push_back(String(L"DataDirectory/example.test/account/message-") + String(std::to_wstring(i).c_str()) + String(L".eml"));
      return members;
   }

   // What WriteMemberList writes for them: the byte-order mark, then a line each.
   std::string MemberListText()
   {
      std::string text = "\xEF\xBB\xBF";
      for (const String &member : Members())
         text += std::string(AnsiString(member).c_str()) + "\r\n";
      return text;
   }

   // One HTTP/1.0 response, a 200 with the body given, to one request on a loopback
   // port of the system's choosing, from a thread of its own. Gone, it stops
   // listening whether or not it was asked.
   class OneResponse
   {
   public:
      explicit OneResponse(const std::string &body) :
         body_(body)
      {
         listener_ = socket(AF_INET, SOCK_STREAM, 0);
         Check(listener_ >= 0, "the loopback server has no socket");
         sockaddr_in address = {};
         address.sin_family = AF_INET;
         address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
         address.sin_port = 0;
         socklen_t length = sizeof(address);
         Check(bind(listener_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0 && listen(listener_, 1) == 0 &&
               getsockname(listener_, reinterpret_cast<sockaddr *>(&address), &length) == 0, "the loopback server could not listen");
         port_ = ntohs(address.sin_port);
         thread_ = std::thread([this] { Serve(); });
      }

      ~OneResponse()
      {
         shutdown(listener_, SHUT_RDWR);
         if (thread_.joinable())
            thread_.join();
         close(listener_);
      }

      OneResponse(const OneResponse &) = delete;
      OneResponse &operator=(const OneResponse &) = delete;

      AnsiString Url() const
      {
         return AnsiString(("http://127.0.0.1:" + std::to_string(port_) + "/body").c_str());
      }

   private:
      void Serve()
      {
         const int peer = accept(listener_, nullptr, nullptr);
         if (peer < 0)
            return;

         std::string request;
         char buffer[1024];
         while (request.find("\r\n\r\n") == std::string::npos)
         {
            const ssize_t got = recv(peer, buffer, sizeof(buffer), 0);
            if (got <= 0)
               break;
            request.append(buffer, (size_t) got);
         }

         const std::string response = "HTTP/1.0 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: " +
                                      std::to_string(body_.size()) + "\r\n\r\n" + body_;
         size_t sent = 0;
         while (sent < response.size())
         {
            const ssize_t put = send(peer, response.data() + sent, response.size() - sent, MSG_NOSIGNAL);
            if (put <= 0)
               break;
            sent += (size_t) put;
         }
         close(peer);
      }

      std::string body_;
      int listener_ = -1;
      int port_ = 0;
      std::thread thread_;
   };

   // The body, through HttpsClient::Download, to the path given. Answers Download's
   // answer; the status and the error are the caller's.
   bool Download(const boost::filesystem::path &target, int &status, String &error)
   {
      const AnsiString payload = Payload();
      OneResponse server(std::string(payload.c_str(), (size_t) payload.GetLength()));
      const size_t limit = size_t{1024} * 1024;
      return HM::HttpsClient::Download(server.Url(), Wide(target), limit, status, error, 10);
   }

   // Whether two paths are on one file system.
   bool SameDevice(const boost::filesystem::path &one, const boost::filesystem::path &two)
   {
      struct stat first = {};
      struct stat second = {};
      return ::stat(one.string().c_str(), &first) == 0 && ::stat(two.string().c_str(), &second) == 0 && first.st_dev == second.st_dev;
   }

   // The writers against a directory whose file system will not take the last
   // bytes of what they write. `message` was stored there before the file system
   // was made to refuse, and must come out as it went in; `result`, a SpamAssassin
   // result put there before too, is what CopyOver is asked to put in its place.
   //
   // `elsewhere`, when it is not empty, is a copy of the result on another file
   // system, which MoveOver is asked to put in the message's place: across file
   // systems the move is a copy, and the copy is what the disk refuses.
   void WritersRefuse(const boost::filesystem::path &directory, const boost::filesystem::path &message, const std::string &original,
                      const boost::filesystem::path &result, const char *where, const boost::filesystem::path &elsewhere)
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

      // The member list a backup restore hands 7-Zip: a list cut short would have
      // it extract fewer members than were asked for.
      const boost::filesystem::path list = directory / "members.txt";
      Check(!HM::Compression::WriteMemberList(Wide(list), Members()), "Compression::WriteMemberList answered true for a list that did not take its last lines" + place);
      Check(!boost::filesystem::exists(list), "Compression::WriteMemberList left a short list behind" + place);

      // A download: a 200 read in full, into a file that did not take it in full.
      if (!SocketsRefused)
      {
         const boost::filesystem::path downloaded = directory / "download.bin";
         int status = 0;
         String error;
         Check(!Download(downloaded, status, error), "HttpsClient::Download answered true for a body its file did not take in full" + place);
         Check(status == 200, "the loopback server's 200 was not read, so the file is not what refused (" + std::to_string(status) + ")" + place);
         Check(!boost::filesystem::exists(downloaded), "HttpsClient::Download left a short file behind" + place);
      }

      // MoveOver from another file system: the move is a copy, and a copy the disk
      // refuses must leave the message as it was and the result where it was.
      if (!elsewhere.empty())
      {
         Check(!SameDevice(elsewhere, message), "the result to move is on the message's own file system, so the move would not copy" + place);
         Check(!HM::FileUtilities::MoveOver(Wide(elsewhere), Wide(message)), "FileUtilities::MoveOver answered true for a move whose copy was refused" + place);
         Check(ReadAll(message) == original, "FileUtilities::MoveOver changed the stored message (" + std::to_string(SizeOf(message)) + " bytes now) although its copy was refused" + place);
         Check(boost::filesystem::exists(elsewhere), "FileUtilities::MoveOver removed the result although it was not moved" + place);
      }

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

      // The result on another file system for MoveOver, where there is one: /dev/shm
      // is a tmpfs on Linux, and the temporary directory usually is not.
      boost::filesystem::path elsewhere;
      if (boost::filesystem::is_directory("/dev/shm") && !SameDevice("/dev/shm", directory))
      {
         elsewhere = boost::filesystem::path("/dev/shm") / boost::filesystem::unique_path("hm-close-result-%%%%-%%%%.eml");
         WriteWhole(elsewhere, Result());
      }
      else
      {
         std::printf("note  the quota case does not move a result between file systems: /dev/shm is missing or on the temporary directory's\n");
      }

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
         WritersRefuse(directory, message, original, result, "a quota", elsewhere);
      }
      catch (const std::exception &e)
      {
         failure = e.what();
      }

      const bool restored = setrlimit(RLIMIT_FSIZE, &before) == 0 && sigaction(SIGXFSZ, &previous, nullptr) == 0;

      boost::system::error_code ignored;
      boost::filesystem::remove_all(directory, ignored);
      if (!elsewhere.empty())
         boost::filesystem::remove(elsewhere, ignored);

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

         // The result on the file system the temporary directory is on, which the
         // tmpfs is not.
         const boost::filesystem::path elsewhere = boost::filesystem::temp_directory_path() /
            boost::filesystem::unique_path("hm-close-result-%%%%-%%%%.eml");
         WriteWhole(elsewhere, Result());
         try
         {
            WritersRefuse(directory, message, original, result, "a full tmpfs", elsewhere);
         }
         catch (...)
         {
            boost::system::error_code ignored;
            boost::filesystem::remove(elsewhere, ignored);
            throw;
         }
         boost::system::error_code ignored;
         boost::filesystem::remove(elsewhere, ignored);
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

   // The argument this program is run again with, by MissingLocaleCase, under a
   // locale no machine has.
   const char MissingLocaleArgument[] = "--under-a-missing-locale";

   // How this program was run (main's argv[0]), for the run again where
   // /proc/self/exe cannot be executed.
   const char *ProgramName = "file-utilities-tests";

   // The run under a missing locale: that it got here at all means the static
   // initialisers, FileUtilities' UseUtf8FileNames among them, did not end the
   // process, and a wide name still converts to UTF-8.
   int MissingLocaleChild()
   {
      const wchar_t wide[] = { L'c', 0x00E0, L'-', 0x20AC, 0 };
      return boost::filesystem::path(wide).native() == std::string("c\xC3\xA0-\xE2\x82\xAC") ? 0 : 3;
   }

   // The environment this process has, with each variable named in taken and each
   // one the NAME=value assignments in given name taken out, and given put in.
   std::vector<std::string> EnvironmentWith(const std::vector<std::string> &taken, const std::vector<std::string> &given)
   {
      std::vector<std::string> names(taken);
      for (const std::string &assignment : given)
         names.push_back(assignment.substr(0, assignment.find('=')));

      std::vector<std::string> environment;
      for (char **entry = environ; entry != nullptr && *entry != nullptr; entry++)
      {
         const std::string variable(*entry);
         bool replaced = false;
         for (const std::string &name : names)
            replaced = replaced || variable.compare(0, name.size() + 1, name + "=") == 0;
         if (!replaced)
            environment.push_back(variable);
      }
      environment.insert(environment.end(), given.begin(), given.end());
      return environment;
   }

   // This program run again with one argument and the environment given - by
   // /proc/self/exe, or by the path main's argv[0] gave where /proc is not
   // mounted - and its wait status. The child is handed its environment by
   // execve; this process's own is not changed.
   int RunAgain(const char *argument, const std::vector<std::string> &environment)
   {
      std::vector<char *> variables;
      for (const std::string &variable : environment)
         variables.push_back(const_cast<char *>(variable.c_str()));
      variables.push_back(nullptr);
      char *const arguments[] = { const_cast<char *>(ProgramName), const_cast<char *>(argument), nullptr };

      // Flushed before the fork, or the child would print the lines above again.
      if (std::fflush(stdout) != 0)
         throw std::runtime_error(std::string("the output could not be flushed before this program was run again with ") + argument);

      const pid_t child = fork();
      if (child == 0)
      {
         execve("/proc/self/exe", arguments, variables.data());
         execve(ProgramName, arguments, variables.data());
         _exit(127);
      }

      int status = 0;
      Check(child > 0 && waitpid(child, &status, 0) == child, std::string("this program could not be run again with ") + argument);
      return status;
   }

   // LANG, LC_ALL or LC_CTYPE naming a locale the machine does not have - an SSH
   // session from a Mac sets LC_CTYPE=UTF-8, a container is given a LANG it has no
   // locale for - changes nothing: this program run again with LC_ALL naming one
   // reaches main and converts a name as UTF-8. Boost 1.90's path::imbue answers
   // with std::locale(""), which glibc's libstdc++ refuses for such a name, and
   // when nothing caught that, every program linked with the server's code -
   // this one, the server and every hmailserver command - ended before main.
   void MissingLocaleCase()
   {
      const int status = RunAgain(MissingLocaleArgument, EnvironmentWith({}, { "LC_ALL=xx_YY.UTF-8", "LANG=xx_YY.UTF-8", "LC_CTYPE=UTF-8" }));
      if (WIFSIGNALED(status))
         throw std::runtime_error("run again with LC_ALL=xx_YY.UTF-8, a locale no machine has, it ended by signal " +
            std::to_string(WTERMSIG(status)) + " before it reached main (an exception from a static initialiser)");
      Check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "run again with LC_ALL=xx_YY.UTF-8 it exited " +
         std::to_string(WIFEXITED(status) ? WEXITSTATUS(status) : -1) + " (3: a wide name did not convert to UTF-8; 127: it could not be run)");
   }

#if HM_LIVE_UPDATE
   // The arguments this program is run again with by AppImageHelperCase, and the
   // two mount points: one named in UTF-8, one in bytes that are not.
   const char AppDirUtf8Argument[] = "--appdir-in-utf8";
   const char AppDirNotUtf8Argument[] = "--appdir-not-utf8";
   const char AppDirInUtf8[] = "/tmp/caf\xC3\xA9-\xE6\x97\xA5/.mount_hMailS";
   const char AppDirNotInUtf8[] = "/tmp/caf\xE9/.mount_hMailS";

   // The AppImage's update helper, which the apply copies out of APPDIR - the
   // image's mount point, under TMPDIR, whatever that is named: read as UTF-8, as
   // every name is here, so that a mount point outside ASCII names the helper that
   // is there, and one whose bytes are not UTF-8 is refused with the reason. Up to
   // 6.3.7 its bytes were widened one to a character, and the helper's name was
   // the mount point's UTF-8 encoded a second time: the copy found nothing
   // (HM6891), and the image could not update itself.
   //
   // Each check runs in this program run again (RunAgain), handed an environment
   // that names the image (APPIMAGE) and its mount point (APPDIR) and holds
   // neither HM_INSTALL_KIND nor SNAP, which would name another kind of
   // installation; this process's own environment is not changed. The run's exit
   // status is the check's: 0 when it holds, 1 with the reason printed when not.
   int AppDirChild(bool utf8)
   {
      try
      {
         String why;
         const String helper = HM::UpdateInstaller::HelperSourcePath(why);
         if (utf8)
         {
            const wchar_t wide[] = { L'/', L't', L'm', L'p', L'/', L'c', L'a', L'f', 0x00E9, L'-', 0x65E5, 0 };
            const String expected = String(wide) + _T("/.mount_hMailS/usr/lib/hmailserver/hmailserver-update");
            Check(why.IsEmpty() && helper == expected, "an APPDIR in UTF-8 named the helper in " + std::to_string(helper.GetLength()) +
               " characters, not the " + std::to_string(expected.GetLength()) + " of " + std::string(HM::Unicode::ToUtf8(expected).c_str()) +
               (why.IsEmpty() ? std::string() : ": " + std::string(HM::Unicode::ToUtf8(why).c_str())));
         }
         else
         {
            Check(helper.IsEmpty() && why.Find(_T("APPDIR")) >= 0 && why.Find(_T("not UTF-8")) >= 0,
               "an APPDIR whose bytes are not UTF-8 was not refused with the reason: [" + std::string(HM::Unicode::ToUtf8(helper).c_str()) + "] [" +
               std::string(HM::Unicode::ToUtf8(why).c_str()) + "]");
         }
         return 0;
      }
      catch (const std::exception &e)
      {
         std::printf("      %s\n", e.what());
         return 1;
      }
   }

   void AppImageHelperCase()
   {
      const std::vector<std::string> taken = { "HM_INSTALL_KIND", "SNAP" };
      const std::string image = "APPIMAGE=/opt/hMailServer-x86_64.AppImage";

      const int inUtf8 = RunAgain(AppDirUtf8Argument, EnvironmentWith(taken, { image, std::string("APPDIR=") + AppDirInUtf8 }));
      Check(WIFEXITED(inUtf8) && WEXITSTATUS(inUtf8) == 0, "an APPDIR in UTF-8: its run exited " +
         std::to_string(WIFEXITED(inUtf8) ? WEXITSTATUS(inUtf8) : -1) + " (the reason is on the line above)");

      const int notUtf8 = RunAgain(AppDirNotUtf8Argument, EnvironmentWith(taken, { image, std::string("APPDIR=") + AppDirNotInUtf8 }));
      Check(WIFEXITED(notUtf8) && WEXITSTATUS(notUtf8) == 0, "an APPDIR whose bytes are not UTF-8: its run exited " +
         std::to_string(WIFEXITED(notUtf8) ? WEXITSTATUS(notUtf8) : -1) + " (the reason is on the line above)");
   }
#endif

   int Run(const std::string &name, void (*test)())
   {
      try
      {
         test();
         std::printf("ok    %s\n", name.c_str());
         return 0;
      }
      catch (const std::exception &e)
      {
         std::printf("FAIL  %s: %s\n", name.c_str(), e.what());
         return 1;
      }
   }

   void DevFullCase()
   {
      HM::FileUtilitiesTester tester;
      tester.TestFailedClose();
   }

   // FileUtilitiesTester::TestFileNameEncoding: up to 6.3.7 File::Open named a
   // file in Latin-1 and Boost.Filesystem in the locale's encoding - UTF-8 under a
   // UTF-8 LANG on glibc, an exception with LANG unset and on musl.
   void FileNameEncodingCase()
   {
      HM::FileUtilitiesTester tester;
      tester.TestFileNameEncoding();
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

   // FileUtilities::WriteToFileAtomically with the body copied from a file, as a
   // message read header only is written: the head and the range, whole, onto
   // the file the range came from and onto another; refused, the file as it was
   // and nothing beside it, for a range past the source's end.
   void BodyCopiedFromFileCase()
   {
      const boost::filesystem::path directory = boost::filesystem::temp_directory_path() /
         boost::filesystem::unique_path("hm-close-range-%%%%-%%%%");
      boost::filesystem::create_directories(directory);

      std::string failure;
      try
      {
         const boost::filesystem::path message = directory / "message.eml";
         const boost::filesystem::path other = directory / "other.eml";
         const std::string old = "Subject: old\r\n\r\n" + std::string(3 * 1024 * 1024 + 17, 'b') + "end\r\n";
         WriteWhole(message, AnsiString(old.data(), old.size()));
         const size_t offset = 16;

         Check(HM::FileUtilities::WriteToFileAtomically(Wide(other), AnsiString("Subject: other\r\n\r\n"), Wide(message), offset, old.size()),
               "WriteToFileAtomically answered false for a body copied from another file");
         Check(ReadAll(other) == "Subject: other\r\n\r\n" + old.substr(offset), "WriteToFileAtomically did not write the head and the whole range to another file");

         Check(HM::FileUtilities::WriteToFileAtomically(Wide(message), AnsiString("Subject: new, and longer\r\n\r\n"), Wide(message), offset, old.size()),
               "WriteToFileAtomically answered false for a body copied from the file it replaces");
         Check(ReadAll(message) == "Subject: new, and longer\r\n\r\n" + old.substr(offset), "WriteToFileAtomically did not put the head and the whole range in the file it was copied from");

         const std::string before = ReadAll(message);
         Check(!HM::FileUtilities::WriteToFileAtomically(Wide(message), AnsiString("Subject: x\r\n\r\n"), Wide(message), offset, before.size() + 1),
               "WriteToFileAtomically answered true for a range past the end of its source");
         Check(ReadAll(message) == before, "a refused WriteToFileAtomically changed the file");
         Check(HoldsOnly(directory, { message.filename().string(), other.filename().string() }), "a refused WriteToFileAtomically left a file beside the message");
      }
      catch (const std::exception &e)
      {
         failure = e.what();
      }

      boost::system::error_code ignored;
      boost::filesystem::remove_all(directory, ignored);
      Check(failure.empty(), failure);
   }

   // A message over MessageData::MaxParseBytes read header only (LargeMessageTester,
   // Common/BO/MessageData.cpp): the header read, changed and written back with the
   // body byte for byte - and so a header longer than the first step it is read in,
   // one whose blank line falls across a step's edge, and a message held in memory;
   // and what is refused, each reported as many times as it is refused.
   void LargeMessageCase()
   {
      const boost::filesystem::path directory = boost::filesystem::temp_directory_path() /
         boost::filesystem::unique_path("hm-large-message-%%%%-%%%%");
      boost::filesystem::create_directories(directory);

      const int changedBody = ReportsOf("HM7840");
      const int notRead = ReportsOf("HM7841");
      const int cutShort = ReportsOf("HM7842");
      const int notWritten = ReportsOf("HM6011");
      const int emptyRefused = ReportsOf("HM6010");
      const int bodyNotRead = ReportsOf("HM6000");

      std::string failure;
      try
      {
         HM::LargeMessageTester tester;
         tester.Test(Wide(directory));
         Check(ReportsOf("HM7840") == changedBody && ReportsOf("HM7841") == notRead && ReportsOf("HM7842") == cutShort &&
               ReportsOf("HM6011") == notWritten && ReportsOf("HM6000") == bodyNotRead,
               "reading, changing and writing the header of a large message, of headers read in steps and of a message held in memory reported an error");

         tester.TestRefusals(Wide(directory));
         Check(ReportsOf("HM7840") == changedBody + 2, "a changed body of a header-only message, from a file and held in memory, was not reported twice as HM7840");
         Check(ReportsOf("HM7842") == cutShort + 1, "a header-only message whose source was cut short was not reported once as HM7842");
         Check(ReportsOf("HM7841") == notRead + 1, "a header with no end in the bytes read was not reported once as HM7841");
         Check(ReportsOf("HM6011") == notWritten + 2, "the two refused writes were not each reported once as HM6011");
         Check(ReportsOf("HM6010") == emptyRefused + 1, "the write of a message whose header could not be read was not refused once as HM6010");
      }
      catch (const std::exception &e)
      {
         failure = e.what();
      }

      boost::system::error_code ignored;
      boost::filesystem::remove_all(directory, ignored);
      Check(failure.empty(), failure);
   }

   // A message under folders named outside ASCII, read by MimeBody, MessageStore::
   // LoadMime and MessageData and written back by SaveAllToFile (MessageFileName-
   // Tester, Common/BO/MessageData.cpp), with nothing reported. Up to 6.3.7 the
   // MIME paths took the name as AnsiString(String) narrows it, and a name above
   // U+00FF was another name to them than to the writer.
   void MessageFileNamesCase()
   {
      const boost::filesystem::path directory = boost::filesystem::temp_directory_path() /
         boost::filesystem::unique_path("hm-message-names-%%%%-%%%%");
      boost::filesystem::create_directories(directory);

      const int notOpened = ReportsOf("HM4228") + ReportsOf("HM4218") + ReportsOf("HM6000");

      std::string failure;
      try
      {
         HM::MessageFileNameTester tester;
         tester.Test(Wide(directory));
         Check(ReportsOf("HM4228") + ReportsOf("HM4218") + ReportsOf("HM6000") == notOpened, "reading and writing the messages reported an error");
      }
      catch (const std::exception &e)
      {
         failure = e.what();
      }

      boost::system::error_code ignored;
      boost::filesystem::remove_all(directory, ignored);
      Check(failure.empty(), failure);
   }

   // The log's text, every file of it.
   std::string LogText()
   {
      std::string all;
      boost::system::error_code error;
      for (boost::filesystem::directory_iterator entry(LogDirectory, error);
           !error && entry != boost::filesystem::directory_iterator(); entry.increment(error))
      {
         std::ifstream in(entry->path().string().c_str(), std::ios::binary);
         all += std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      }
      return all;
   }

   // A secret a backup carries that the legacy Blowfish scheme refuses: left out,
   // with one HM7280 naming whose it was and which limit refused it - a character
   // above U+00FF, or more characters than the scheme holds, as an OAuth refresh
   // token often has - and no other report (HM7277, HM7278). One it takes is
   // written, with no report.
   void BackupSecretCase()
   {
      const int refusedBefore = ReportsOf("HM7280");
      const int otherBefore = ReportsOf("HM7277") + ReportsOf("HM7278");

      const HM::String wide(L"p\x00e0ss\x20ac");
      const HM::String token(std::wstring(249, L't').c_str());
      const HM::String wideWhose(_T("The password of the external account Wide (server imap.example, user w)"));
      const HM::String tokenWhose(_T("The OAuth token of the external account Long (server imap.example, user t)"));

      const HM::String wideWritten = HM::Crypt::Instance()->EnCryptForXml(wide, wideWhose);
      const HM::String tokenWritten = HM::Crypt::Instance()->EnCryptForXml(token, tokenWhose);
      const HM::String takenWritten = HM::Crypt::Instance()->EnCryptForXml(HM::String(L"caf\x00e9"), _T("The password of the external account Taken"));

      Check(wideWritten.IsEmpty() && tokenWritten.IsEmpty(), "a secret the legacy Blowfish scheme refuses was written to the backup");
      Check(takenWritten == HM::String(_T("df3dc19376c45323")), "a secret the scheme takes was not written as the scheme writes it");
      Check(ReportsOf("HM7280") == refusedBefore + 2, "the two refused secrets were not each reported once as HM7280");
      Check(ReportsOf("HM7277") + ReportsOf("HM7278") == otherBefore, "a refused secret was reported a second time, by the scheme's own HM7277 or HM7278");

      const std::string log = LogText();
      Check(log.find(std::string(HM::Unicode::ToUtf8(wideWhose).c_str()) + " was written to the backup without it, since the legacy Blowfish scheme a backup carries secrets in refused it: it holds a character above U+00FF") != std::string::npos,
         "HM7280 for the password above U+00FF does not name whose it was and why");
      Check(log.find(std::string(HM::Unicode::ToUtf8(tokenWhose).c_str()) + " was written to the backup without it, since the legacy Blowfish scheme a backup carries secrets in refused it: it has 249 characters, and the scheme holds at most 248") != std::string::npos,
         "HM7280 for the 249-character token does not name whose it was and its length");
   }
}

namespace
{
   // The three where the disk takes what they write, the controls for the
   // refusals above: the list whole, the download whole, and MoveOver putting the
   // whole result in the message's place - from the message's own file system,
   // where it is a rename, and from another, where it is a copy - leaving nothing
   // beside the message and the result gone from where it was.
   void WritersWithRoomCase()
   {
      const boost::filesystem::path directory = boost::filesystem::temp_directory_path() /
         boost::filesystem::unique_path("hm-close-room-%%%%-%%%%");
      boost::filesystem::create_directories(directory);
      boost::filesystem::path elsewhere;

      std::string failure;
      try
      {
         const boost::filesystem::path list = directory / "members.txt";
         Check(HM::Compression::WriteMemberList(Wide(list), Members()), "Compression::WriteMemberList answered false where the disk took the list");
         Check(ReadAll(list) == MemberListText(), "Compression::WriteMemberList did not write the list whole");
         boost::filesystem::remove(list);

         if (!SocketsRefused)
         {
            const boost::filesystem::path downloaded = directory / "download.bin";
            int status = 0;
            String error;
            const bool fetched = Download(downloaded, status, error);
            Check(fetched && status == 200, "HttpsClient::Download answered false (" + std::string(AnsiString(error).c_str()) + ") where the disk took the body");
            Check(ReadAll(downloaded) == std::string(Payload().c_str(), PayloadBytes), "HttpsClient::Download did not write the body whole");
            boost::filesystem::remove(downloaded);
         }

         const boost::filesystem::path message = directory / "message.eml";
         const boost::filesystem::path result = directory / "result.eml";
         WriteWhole(message, Payload());
         WriteWhole(result, Result());
         Check(HM::FileUtilities::MoveOver(Wide(result), Wide(message)), "FileUtilities::MoveOver answered false for a rename on one file system");
         Check(ReadAll(message) == std::string(Result().c_str(), ResultBytes), "FileUtilities::MoveOver did not put the whole result in the message's place");
         Check(!boost::filesystem::exists(result), "FileUtilities::MoveOver left the result where it was");
         Check(HoldsOnly(directory, { message.filename().string() }), "FileUtilities::MoveOver left a file beside the message");

         if (boost::filesystem::is_directory("/dev/shm") && !SameDevice("/dev/shm", directory))
         {
            WriteWhole(message, Payload());
            elsewhere = boost::filesystem::path("/dev/shm") / boost::filesystem::unique_path("hm-close-result-%%%%-%%%%.eml");
            WriteWhole(elsewhere, Result());
            Check(HM::FileUtilities::MoveOver(Wide(elsewhere), Wide(message)), "FileUtilities::MoveOver answered false for a move between file systems that had room");
            Check(ReadAll(message) == std::string(Result().c_str(), ResultBytes), "FileUtilities::MoveOver did not put the whole result in the message's place from another file system");
            Check(!boost::filesystem::exists(elsewhere), "FileUtilities::MoveOver left the result on the other file system");
            Check(HoldsOnly(directory, { message.filename().string() }), "FileUtilities::MoveOver left a file beside the message after a move between file systems");
         }
         else
         {
            std::printf("note  MoveOver between file systems was not tried with room: /dev/shm is missing or on the temporary directory's\n");
         }
      }
      catch (const std::exception &e)
      {
         failure = e.what();
      }

      boost::system::error_code ignored;
      boost::filesystem::remove_all(directory, ignored);
      if (!elsewhere.empty())
         boost::filesystem::remove(elsewhere, ignored);
      Check(failure.empty(), failure);
   }
}

int main(int argc, char *argv[])
{
   if (argc == 2 && std::strcmp(argv[1], MissingLocaleArgument) == 0)
      return MissingLocaleChild();
#if HM_LIVE_UPDATE
   if (argc == 2 && std::strcmp(argv[1], AppDirUtf8Argument) == 0)
      return AppDirChild(true);
   if (argc == 2 && std::strcmp(argv[1], AppDirNotUtf8Argument) == 0)
      return AppDirChild(false);
#endif
   if (argc > 0 && argv[0] != nullptr && argv[0][0] != 0)
      ProgramName = argv[0];

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

   // The download's loopback server, where this process may make a socket. EACCES
   // is offline.c's refusal; any other failure is left to the cases, which fail on it.
   const int probe = socket(AF_INET, SOCK_STREAM, 0);
   if (probe >= 0)
      close(probe);
   else if (errno == EACCES)
      SocketsRefused = true;

   failed += Run("a written file's close: a whole file closes true, and /dev/full's close answers false through File::Close and FileUtilities::WriteToFile", DevFullCase);
   failed += Run("the spool of a received message: a file that takes every byte is written whole, and on /dev/full the message is counted as not written", SpoolCase);
   failed += Run("FileUtilities::CopyOver puts the whole of a file in another's place and leaves nothing beside it", CopyOverCase);
   failed += Run("a file named with characters outside ASCII is one file to File::Open, FileUtilities, Boost.Filesystem, stat() and a directory's listing, all in UTF-8 whatever LANG says", FileNameEncodingCase);
   failed += Run("a locale named that the machine does not have (LC_ALL=xx_YY.UTF-8): this program run again under it reaches main and names a file in UTF-8", MissingLocaleCase);
#if HM_LIVE_UPDATE
   failed += Run("the AppImage's update helper: APPDIR, where the image is mounted, read as UTF-8, and refused with the reason when it is not", AppImageHelperCase);
#else
   std::printf("not run: the AppImage's update helper - this build has no live update (HM_LIVE_UPDATE OFF)\n");
#endif
   failed += Run("a message under folders named outside ASCII, above U+00FF and within it, is read by MimeBody, MessageStore::LoadMime and MessageData and written back by SaveAllToFile by its name", MessageFileNamesCase);
   failed += Run("a backup's secret the legacy Blowfish scheme refuses - above U+00FF, or 249 characters long - is left out with one HM7280 naming whose it was and why, and one it takes is written", BackupSecretCase);
   failed += Run("a backup restore's member list" + ADownload() + " and MoveOver where the disk takes them: each whole, and MoveOver's result in the message's place with nothing beside it", WritersWithRoomCase);
   failed += Run("FileUtilities::WriteToFileAtomically with the body copied from a file: the head and the whole range, onto another file and onto the one it came from, and a range past the source's end refused with the file as it was", BodyCopiedFromFileCase);
   failed += Run("a message over 80 MB read header only when asked: its loop count read, raised and written back with the body byte for byte, as are a header past the first step it is read in, one ending across a step's edge and a message held in memory; and a changed body, a source cut short and a header with no end refused and reported", LargeMessageCase);
   failed += Run("a quota (RLIMIT_FSIZE): File::Close, WriteToFile, TraceHeaderWriter::Write, the message store's WriteAtomically, the spool, CopyOver, a backup restore's member list" + ADownload() + " and MoveOver from another file system refuse a file short of its last bytes, and leave the stored message as it was", QuotaCase);
   if (SocketsRefused)
      std::printf("not run: a download (HttpsClient::Download into a file that takes it whole, under the quota and on the full disk) - this process may make no IPv4 socket (EACCES), as under build/ci/packages/offline.c, and the download's server is a loopback one; every other writer runs without it\n");

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
      std::printf("ok    a full disk (a 64 KB tmpfs filled to its last byte): File::Close, WriteToFile, TraceHeaderWriter::Write, the message store's WriteAtomically, the spool, CopyOver, a backup restore's member list%s and MoveOver from another file system refuse, and leave the stored message as it was\n", ADownload().c_str());
   }
   else
   {
      failed++;
   }

   boost::system::error_code ignored;
   boost::filesystem::remove_all(scratch, ignored);

   return failed == 0 ? 0 : 1;
}
