// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The scratch directory a tester program makes for itself under the system's
// temp directory, where it writes its hMailServer.ini and points the Logger and
// the ini's folders. Made by mkdtemp: a name of the prefix and six random
// characters nobody has made, made 0700 and the tester's own, and never a
// directory, a file or a link that is there already - which another account on
// the machine could have prepared, at a name it could foresee, for the tester to
// write through. Until 31 October 2026 the testers made
// <temp>/hm-<name>-tests-<pid> with boost::filesystem::create_directories,
// which takes such a directory, or a link to one, as made; a hMailServer.ini
// linked from it to a file the tester's account could write was written over.
// POSIX only: the tester programs are Linux builds.

#pragma once

#include <cerrno>
#include <cstdlib>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <boost/filesystem.hpp>
#include <boost/system/error_code.hpp>

namespace HMTesterScratch
{
   // A new directory <base>/<prefix>-XXXXXX, by mkdtemp. Empty, with error set,
   // when none could be made.
   inline boost::filesystem::path MakeIn(const boost::filesystem::path &base, const std::string &prefix, boost::system::error_code &error)
   {
      error.clear();

      const std::string pattern = (base / (prefix + "-XXXXXX")).string();
      std::vector<char> buffer(pattern.begin(), pattern.end());
      buffer.push_back('\0');

      if (::mkdtemp(buffer.data()) == nullptr)
      {
         error = boost::system::error_code(errno, boost::system::system_category());
         return boost::filesystem::path();
      }

      return boost::filesystem::path(buffer.data());
   }

   // The same under the system's temp directory (TMPDIR, or else /tmp).
   inline boost::filesystem::path Make(const std::string &prefix, boost::system::error_code &error)
   {
      error.clear();

      const boost::filesystem::path base = boost::filesystem::temp_directory_path(error);
      if (error)
         return boost::filesystem::path();

      return MakeIn(base, prefix, error);
   }

   // Makes path, which must not be there, 0700: refused (EEXIST) when anything
   // is - a directory, a file, or a link, even to a directory - as mkdtemp
   // refuses a name it finds taken.
   inline bool MakeNew(const boost::filesystem::path &path, boost::system::error_code &error)
   {
      error.clear();

      if (::mkdir(path.string().c_str(), 0700) != 0)
      {
         error = boost::system::error_code(errno, boost::system::system_category());
         return false;
      }

      return true;
   }

   // The rule held, on a directory of its own: a directory, and a link to one,
   // prepared where a scratch directory is to be made are refused; a name that
   // is free is made, 0700 and the caller's own; and two directories Make makes
   // for one prefix are two. Empty when it holds, and otherwise what did not.
   inline std::string Check()
   {
      boost::system::error_code error;
      const boost::filesystem::path root = Make("hm-tester-scratch-check", error);
      if (root.empty())
         return "no directory could be made to check the scratch directory's rule in";

      std::string failed;
      const boost::filesystem::path prepared = root / "prepared";
      const boost::filesystem::path link = root / "link";
      const boost::filesystem::path fresh = root / "fresh";

      if (::mkdir(prepared.string().c_str(), 0777) != 0 || ::symlink(prepared.string().c_str(), link.string().c_str()) != 0)
         failed = "a prepared directory and a link to it could not be made to check the rule with";
      else if (MakeNew(prepared, error) || error.value() != EEXIST)
         failed = "a scratch directory was taken where a directory had been prepared";
      else if (MakeNew(link, error) || error.value() != EEXIST)
         failed = "a scratch directory was taken where a link to a directory had been prepared";
      else if (!MakeNew(fresh, error))
         failed = "a scratch directory could not be made where nothing was";
      else
      {
         struct stat made {};
         if (::lstat(fresh.string().c_str(), &made) != 0 || !S_ISDIR(made.st_mode) || (made.st_mode & 07777) != 0700 ||
             made.st_uid != ::geteuid())
            failed = "a scratch directory made is not a directory of mode 0700 of the tester's own";
      }

      if (failed.empty())
      {
         const boost::filesystem::path other = MakeIn(root, "twice", error);
         const boost::filesystem::path again = MakeIn(root, "twice", error);
         struct stat made {};
         if (other.empty() || again.empty() || other == again)
            failed = "two scratch directories made for one prefix are not two";
         else if (::lstat(other.string().c_str(), &made) != 0 || !S_ISDIR(made.st_mode) || (made.st_mode & 07777) != 0700 ||
                  made.st_uid != ::geteuid())
            failed = "a scratch directory mkdtemp made is not a directory of mode 0700 of the tester's own";
      }

      boost::filesystem::remove_all(root, error);
      return failed;
   }
}
