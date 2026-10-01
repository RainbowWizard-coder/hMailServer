// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Who owns hMailServer.INI after the server rewrites it (Common/Util/IniFile.cpp,
// WriteFileAtomically), as a program of its own for the POSIX build - the
// counterpart of spf-tests and backup-tests. Nothing on Windows runs that code
// (there the profile API is kernel32's), so this is the only test of it.
//
// A rewrite goes to a temporary file beside the configuration, which is given
// the owner and the mode of the file it replaces and renamed over it. That
// matters run as root, as `sudo hmailserver --set-admin-password` is: the
// packaged file is root:hmailserver 0640, and a root:root replacement locks the
// service out of its own configuration. The file is read first and its owner
// taken a moment later, and in between it can go or be replaced. Up to 6.3.5 a
// file gone by then left the replacement with an owner taken from a stat that
// had not answered; from 6.3.6 it is made as a new file is, the writer's own,
// 0600. Four cases, in a scratch directory under /dev/shm (or /tmp) that is removed
// again:
//
//   1. root rewrites a file another user owns: its owner, group and mode stay;
//   2. the file is read and gone before it is written: the replacement is the
//      writer's own, 0600. The server reads a FIFO whose writer removes it
//      before closing it, so the moment between the read and the write is not
//      left to chance. Run by any user;
//   3. the file is read, and before it is written another user has made a file
//      of their own there: the replacement takes that file's owner and mode;
//   4. a race: another user's process removes the file and makes it again as
//      fast as it can while root rewrites it two thousand times; after every
//      rewrite the file is root's own 0600, the other user's 0644, or absent,
//      and never anybody else's.
//
// 1, 3 and 4 need root - to give a file to another user, and to be one - and say
// that they are skipped otherwise; linux-build.sh runs this in the CI image, as
// root. No database, configuration or network. One line per case, and a
// non-zero exit on a failure.

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>

#include <boost/filesystem.hpp>

#include <fcntl.h>
#include <grp.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../Common/Util/IniFile.h"

namespace
{
   // Another user and group: numbers, which need no entry in /etc/passwd.
   const uid_t OtherUser = 4242;
   const gid_t OtherGroup = 4343;

   const int RaceRewrites = 2000;

   int failures = 0;

   void Report(bool passed, const char *what, const std::string &detail)
   {
      if (passed)
      {
         printf("ok    %s\n", what);
      }
      else
      {
         printf("FAIL  %s: %s\n", what, detail.c_str());
         failures++;
      }
   }

   std::wstring Wide(const std::string &text)
   {
      return std::wstring(text.begin(), text.end());
   }

   bool Write(const std::string &path, const wchar_t *key, const std::wstring &value)
   {
      return WritePrivateProfileString(L"Settings", key, value.c_str(), Wide(path).c_str()) != FALSE;
   }

   std::wstring Read(const std::string &path, const wchar_t *key)
   {
      wchar_t buffer[256] = {};
      GetPrivateProfileString(L"Settings", key, L"", buffer, 256, Wide(path).c_str());
      return buffer;
   }

   // "file uid:gid mode", the mode in octal.
   std::string Expected(uid_t uid, gid_t gid, mode_t mode, const char *kind = "file")
   {
      std::ostringstream text;
      text << kind << ' ' << (unsigned long) uid << ':' << (unsigned long) gid << ' '
           << std::setfill('0') << std::setw(4) << std::oct << (unsigned long) (mode & 07777);
      return text.str();
   }

   // Expected's form for what is at path, "absent", or "not a file ...".
   std::string Describe(const std::string &path)
   {
      struct stat state = {};
      if (::lstat(path.c_str(), &state) != 0)
         return "absent";
      return Expected(state.st_uid, state.st_gid, state.st_mode, S_ISREG(state.st_mode) ? "file" : "not a file");
   }

   bool WriteAll(int descriptor, const std::string &bytes)
   {
      size_t offset = 0;
      while (offset < bytes.size())
      {
         const ssize_t wrote = ::write(descriptor, bytes.data() + offset, bytes.size() - offset);
         if (wrote < 0 && errno == EINTR)
            continue;
         if (wrote <= 0)
            return false;
         offset += (size_t) wrote;
      }
      return true;
   }

   // In a forked child: becomes the other user. Async-signal-safe calls only, so
   // that a child forked from a process with threads may make them.
   bool BecomeOther()
   {
      return ::setgroups(0, nullptr) == 0 && ::setgid(OtherGroup) == 0 && ::setuid(OtherUser) == 0;
   }

   // Makes path, 0644, as the other user, in a process of its own; true when it
   // was made.
   bool MakeAsOther(const std::string &path, const std::string &content)
   {
      const pid_t child = ::fork();
      if (child < 0)
         return false;
      if (child == 0)
      {
         if (!BecomeOther())
            _exit(2);
         ::umask(022);
         const int descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
         if (descriptor < 0)
            _exit(3);
         const bool written = WriteAll(descriptor, content);
         _exit(::close(descriptor) == 0 && written ? 0 : 4);
      }
      int status = 0;
      if (::waitpid(child, &status, 0) != child)
         return false;
      return WIFEXITED(status) && WEXITSTATUS(status) == 0;
   }

   // The writer's end of a FIFO at path, for cases 2 and 3. It gives the reader
   // content, removes the name, in case 3 lets another user make a file there,
   // and only then closes - so that by the time the reader has read to the end,
   // the file it read is gone and what is at the name is what this left there.
   // Opened without waiting, and tried again for ten seconds, so that a reader
   // that never comes leaves no thread waiting for ever.
   void ServeAndReplace(const std::string &path, const std::string &content, bool otherMakesOne, bool &served)
   {
      served = false;
      int descriptor = -1;
      const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
      while (descriptor < 0 && std::chrono::steady_clock::now() < deadline)
      {
         descriptor = ::open(path.c_str(), O_WRONLY | O_NONBLOCK);
         if (descriptor < 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      if (descriptor < 0)
         return;
      const int flags = ::fcntl(descriptor, F_GETFL);
      bool done = flags >= 0 && ::fcntl(descriptor, F_SETFL, flags & ~O_NONBLOCK) == 0;
      done = done && WriteAll(descriptor, content);
      done = done && ::unlink(path.c_str()) == 0;
      if (otherMakesOne)
         done = done && MakeAsOther(path, "[Other]\r\nMine=1\r\n");
      ::close(descriptor);
      served = done;
   }

   // A configuration for the FIFO to serve: a section and a comment long enough
   // that the reader's buffer is full of it.
   std::string ServedContent()
   {
      return "[Settings]\r\nServed=1\r\n;" + std::string(6000, 'A') + "\r\n";
   }

   void RemoveTree(const std::string &directory)
   {
      boost::system::error_code error;
      boost::filesystem::remove_all(directory, error);
   }

   // 1. root rewrites a file another user owns.
   void OwnerAndModeAreKept(const std::string &directory)
   {
      const char *what = "a file another user owns, rewritten by root, keeps its owner, group and mode";
      const std::string path = directory + "/kept.ini";
      if (!MakeAsOther(path, "[Settings]\r\nKept=1\r\n") || ::chmod(path.c_str(), 0640) != 0)
      {
         Report(false, what, "the file could not be made as another user");
         return;
      }
      const bool written = Write(path, L"Round", L"1");
      const std::string found = Describe(path);
      const std::string wanted = Expected(OtherUser, OtherGroup, 0640);
      Report(written && found == wanted && Read(path, L"Round") == L"1" && Read(path, L"Kept") == L"1", what,
         std::string(written ? "written" : "the write was refused") + "; " + found + ", wanted " + wanted);
   }

   // 2 and 3. The file is read, and gone - or another user's - before it is written.
   void ReadThenReplaced(const std::string &directory, bool otherMakesOne)
   {
      const char *what = otherMakesOne
         ? "a file read and, before it is written, made again by another user gives the replacement that user's owner and mode"
         : "a file read and gone before it is written is made as a new one: the writer's own, 0600";
      const std::string path = directory + (otherMakesOne ? "/replaced.ini" : "/gone.ini");
      if (::mkfifo(path.c_str(), 0600) != 0)
      {
         Report(false, what, "mkfifo failed, errno " + std::to_string(errno));
         return;
      }
      bool served = false;
      std::thread server(ServeAndReplace, path, ServedContent(), otherMakesOne, std::ref(served));
      const bool written = Write(path, L"Round", otherMakesOne ? L"3" : L"2");
      server.join();
      const std::string found = Describe(path);
      const std::string wanted = otherMakesOne ? Expected(OtherUser, OtherGroup, 0644) : Expected(::geteuid(), ::getegid(), 0600);
      const bool content = Read(path, L"Round") == (otherMakesOne ? L"3" : L"2") && Read(path, L"Served") == L"1";
      Report(served && written && found == wanted && content, what,
         std::string(served ? "" : "the FIFO's writer did not finish; ") + (written ? "written" : "the write was refused") +
         "; " + found + ", wanted " + wanted + (content ? "" : "; the content is not what was written"));
   }

   // 4. Another user's process removes the file and makes it again, as fast as it
   // can, while root rewrites it.
   void RaceWithAnotherUser(const std::string &directory)
   {
      const char *what = "two thousand rewrites by root while another user removes and remakes the file: each leaves it root's own 0600, the other user's 0644, or absent";
      const std::string path = directory + "/race.ini";
      const std::string content = "[Settings]\r\nOther=1\r\n";
      const pid_t other = ::fork();
      if (other < 0)
      {
         Report(false, what, "fork failed");
         return;
      }
      if (other == 0)
      {
         if (!BecomeOther())
            _exit(2);
         ::umask(022);
         for (;;)
         {
            ::unlink(path.c_str());
            const int descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
            if (descriptor >= 0)
            {
               WriteAll(descriptor, content);
               ::close(descriptor);
            }
         }
      }

      const std::string own = Expected(::geteuid(), ::getegid(), 0600);
      const std::string others = Expected(OtherUser, OtherGroup, 0644);
      int ownCount = 0;
      int otherCount = 0;
      int absentCount = 0;
      int refused = 0;
      std::string wrong;
      for (int round = 0; round < RaceRewrites; round++)
      {
         if (!Write(path, L"Round", std::to_wstring(round)))
            refused++;
         const std::string found = Describe(path);
         if (found == own)
            ownCount++;
         else if (found == others)
            otherCount++;
         else if (found == "absent")
            absentCount++;
         else if (wrong.empty())
            wrong = found;
      }
      ::kill(other, SIGKILL);
      int status = 0;
      ::waitpid(other, &status, 0);
      const bool otherRan = WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL;

      const std::string counts = std::to_string(refused) + " rewrites refused; afterwards root's own " +
         std::to_string(ownCount) + " times, the other user's " + std::to_string(otherCount) + ", absent " +
         std::to_string(absentCount);
      Report(otherRan && refused == 0 && wrong.empty(), what,
         std::string(otherRan ? "" : "the other user's process did not run; ") + counts +
         (wrong.empty() ? std::string() : "; once " + wrong));
      if (otherRan && refused == 0 && wrong.empty())
         printf("      %s\n", counts.c_str());
   }
}

int
main(int /*argc*/, char * /*argv*/[])
{
   const char *shm = "/dev/shm";
   struct stat shmState = {};
   const std::string base = ::stat(shm, &shmState) == 0 && S_ISDIR(shmState.st_mode) && ::access(shm, W_OK) == 0
      ? std::string(shm) : std::string("/tmp");
   std::string pattern = base + "/hm-ini-file-tests-XXXXXX";
   if (!::mkdtemp(&pattern[0]))
   {
      printf("FAIL  the configuration file's owner: no scratch directory could be made under %s\n", base.c_str());
      return 1;
   }
   const std::string directory = pattern;
   // Writable by the other user, and not sticky, so that it may remove and make
   // files there as the cases need.
   ::chmod(directory.c_str(), 0777);

   const bool root = ::geteuid() == 0;
   if (root)
      OwnerAndModeAreKept(directory);
   else
      printf("skip  a file another user owns keeps its owner: not root\n");

   ReadThenReplaced(directory, false);

   if (root)
   {
      ReadThenReplaced(directory, true);
      RaceWithAnotherUser(directory);
   }
   else
   {
      printf("skip  a file made again by another user between the read and the write: not root\n");
      printf("skip  the race with another user: not root\n");
   }

   RemoveTree(directory);
   return failures == 0 ? 0 : 1;
}
