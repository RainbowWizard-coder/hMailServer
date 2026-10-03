// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// How a command that works in the service's folders runs when root runs it
// (platform/root_command.h, RootCommand::Decide), held to the layouts it is for,
// as a program of its own for the POSIX build - there is no Windows
// counterpart: the Windows commands hold the data folder from its drive's root
// down instead.
//
// Each case lays out a data directory, a log directory, a temporary folder and
// a configuration in a scratch folder of its own, under a folder only root may
// change (the first of /var/lib, /run and /root that is), gives folders, links,
// files and access control lists to other accounts (uid 65534, and 65533 as a
// third), and asks Decide what --apply-environment run as root would do:
//
//   * all of it root's - a link of root's to another folder of root's, a link
//     that leads back up, a file of another account's, a group-writable folder
//     of root's group, a temporary folder not made yet, a configuration named
//     through a link of root's, a default access control list whose mask holds
//     another account's write back - runs as root;
//   * a folder in it that belongs to another account, one a group other than
//     root's or every account may write, one whose group's write an access
//     control list stands behind, one whose default access control list gives
//     another account (or, set-group-ID, another group) a write to what is
//     made in it, a link that belongs to another account, a link of root's
//     whose way or whose folder is not root's alone, a log directory of another
//     account's, a data directory below a folder of another account's, a
//     missing folder whose folder would be another's or is not there, and a
//     configuration another account owns, may write, or holds the folder of:
//     each refused, naming it;
//   * a data directory another account owns becomes that account where the way
//     to it is root's and that account's alone (whatever the log directory
//     is), and is refused where a third account owns a folder on the way; a
//     data directory that is not there is refused.
//
// Giving a folder or a link to another account needs root, as the command does:
// run without it, the program checks what it can - a tree of its own user's is
// refused, and so is a way through a folder every account may write - and says
// what it skipped. linux-build.sh runs it as root in the CI image's container.
// KeepWhatItMakesRoots is held to its umask either way. One line per case,
// and a non-zero exit on a failure. No database or network; the scratch
// folder is removed again.

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>
#include <ftw.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <unistd.h>

#include "root_command.h"

namespace
{
   int failures = 0;
   int passes = 0;

   void Report(bool ok, const std::string &what, const std::string &detail)
   {
      if (ok)
      {
         ++passes;
         (void) std::printf("ok    %s\n", what.c_str());
      }
      else
      {
         ++failures;
         (void) std::printf("FAIL  %s: %s\n", what.c_str(), detail.c_str());
      }
   }

   std::string ErrorText(int code)
   {
      return std::generic_category().message(code);
   }

   bool Contains(const std::string &text, const std::string &part)
   {
      return text.find(part) != std::string::npos;
   }

   const char *OutcomeName(HM::RootCommand::Outcome outcome)
   {
      switch (outcome)
      {
      case HM::RootCommand::AsOwner:
         return "AsOwner";
      case HM::RootCommand::AsRoot:
         return "AsRoot";
      default:
         return "Refused";
      }
   }

   // The decision for --apply-environment over data, with logs and temp as
   // the log directory and the temporary folder, and ini as the configuration.
   HM::RootCommand::Decision Decide(const std::string &data, const std::string &logs, const std::string &temp, const std::string &ini)
   {
      const std::vector<HM::RootCommand::Folder> others = {
         { logs, "the log directory ([Directories] LogFolder)" },
         { temp, "the temporary folder ([Directories] TempFolder)" },
         { "", "the database folder ([Directories] DatabaseFolder)" } };
      return HM::RootCommand::Decide("--apply-environment", data, others, ini);
   }

   // The outcome expected, and every part expected in the message.
   void Expect(const std::string &what, const HM::RootCommand::Decision &decision, HM::RootCommand::Outcome outcome, const std::vector<std::string> &parts)
   {
      std::string detail;
      if (decision.outcome != outcome)
         detail = std::string("expected ") + OutcomeName(outcome) + ", decided " + OutcomeName(decision.outcome);
      for (const std::string &part : parts)
      {
         if (detail.empty() && !Contains(decision.message, part))
            detail = "the message does not name \"" + part + "\"";
      }

      if (!detail.empty())
         detail += ". The message: " + decision.message;
      Report(detail.empty(), what, detail);
   }

   bool Make(const std::string &path, mode_t mode, uid_t uid = 0, gid_t gid = 0)
   {
      if (::mkdir(path.c_str(), mode) != 0 || ::chmod(path.c_str(), mode) != 0 || ::chown(path.c_str(), uid, gid) != 0)
      {
         (void) std::printf("FAIL  the layout: %s could not be made: %s\n", path.c_str(), ErrorText(errno).c_str());
         ++failures;
         return false;
      }
      return true;
   }

   bool Link(const std::string &to, const std::string &at, uid_t uid = 0, gid_t gid = 0)
   {
      if (::symlink(to.c_str(), at.c_str()) != 0 || ::lchown(at.c_str(), uid, gid) != 0)
      {
         (void) std::printf("FAIL  the layout: the link %s could not be made: %s\n", at.c_str(), ErrorText(errno).c_str());
         ++failures;
         return false;
      }
      return true;
   }

   bool File(const std::string &path, uid_t uid = 0, gid_t gid = 0)
   {
      FILE *file = std::fopen(path.c_str(), "w");
      if (file == nullptr)
      {
         (void) std::printf("FAIL  the layout: %s could not be made: %s\n", path.c_str(), ErrorText(errno).c_str());
         ++failures;
         return false;
      }
      (void) std::fputs("a message\n", file);
      (void) std::fclose(file);
      return ::chown(path.c_str(), uid, gid) == 0;
   }

   // An access control list in the kernel's form (posix_acl_xattr: a version
   // of 2, then a tag, the permissions and an id per entry, little-endian) put
   // on path as name (system.posix_acl_access or system.posix_acl_default),
   // its entries in the order the kernel asks: the user owner (1), users (2),
   // the group owner (4), groups (8), the mask (16), others (32). False, errno
   // set, where the file system keeps no lists.
   struct Entry
   {
      unsigned int tag;
      unsigned int permissions;
      unsigned long id;
   };

   const unsigned long Unset = 0xffffffffUL;

   bool PutList(const std::string &path, const char *name, const std::vector<Entry> &entries)
   {
      std::vector<unsigned char> bytes = { 2, 0, 0, 0 };
      for (const Entry &entry : entries)
      {
         const unsigned char more[] = { (unsigned char) (entry.tag & 0xffU), (unsigned char) (entry.tag >> 8U),
            (unsigned char) (entry.permissions & 0xffU), (unsigned char) (entry.permissions >> 8U),
            (unsigned char) (entry.id & 0xffUL), (unsigned char) ((entry.id >> 8U) & 0xffUL), (unsigned char) ((entry.id >> 16U) & 0xffUL),
            (unsigned char) ((entry.id >> 24U) & 0xffUL) };
         bytes.insert(bytes.end(), std::begin(more), std::end(more));
      }
      return ::lsetxattr(path.c_str(), name, bytes.data(), bytes.size(), 0) == 0;
   }

   int RemoveOne(const char *path, const struct stat * /*held*/, int kind, struct FTW * /*where*/)
   {
      return kind == FTW_DP ? ::rmdir(path) : ::unlink(path);
   }

   void RemoveAll(const std::string &path)
   {
      // NOLINT below: the program has one thread, and without FTW_CHDIR nftw
      // changes no working directory.
      (void) ::nftw(path.c_str(), RemoveOne, 32, FTW_DEPTH | FTW_PHYS); // NOLINT(concurrency-mt-unsafe)
   }

   // A folder only root may change, to make the scratch folder in: the way to
   // it root's alone, and it root's with no write for anybody else.
   std::string RootOnlyFolder()
   {
      const std::vector<std::string> candidates = { "/var/lib", "/run", "/root" };

      for (const std::string &candidate : candidates)
      {
         struct stat held {};
         std::string why;
         if (::lstat(candidate.c_str(), &held) == 0 && S_ISDIR(held.st_mode) && held.st_uid == 0 &&
             (held.st_mode & S_IWOTH) == 0 && ((held.st_mode & S_IWGRP) == 0 || held.st_gid == 0) &&
             HM::RootCommand::WayIsRootsOr(candidate, 0, 0, why))
            return candidate;
      }
      return std::string();
   }

   // One case's layout, root's alone: <case>/Data (with x/y, a file and the
   // temporary folder Temp in it), <case>/Logs, <case>/elsewhere/inner and the
   // configuration <case>/program/hMailServer.ini.
   struct Layout
   {
      std::string root;
      std::string data;
      std::string logs;
      std::string temp;
      std::string elsewhere;
      std::string ini;
      bool made = false;
   };

   Layout Lay(const std::string &scratch, const std::string &name)
   {
      Layout layout;
      layout.root = scratch + "/" + name;
      layout.data = layout.root + "/Data";
      layout.logs = layout.root + "/Logs";
      layout.temp = layout.data + "/Temp";
      layout.elsewhere = layout.root + "/elsewhere";
      layout.ini = layout.root + "/program/hMailServer.ini";
      layout.made = Make(layout.root, 0755) && Make(layout.data, 0755) && Make(layout.data + "/x", 0755) &&
         Make(layout.data + "/x/y", 0750) && File(layout.data + "/x/y/message.eml") && Make(layout.temp, 0755) &&
         Make(layout.logs, 0755) && Make(layout.elsewhere, 0755) && Make(layout.elsewhere + "/inner", 0755) &&
         Make(layout.root + "/program", 0755) && File(layout.ini) && ::chmod(layout.ini.c_str(), 0644) == 0;
      return layout;
   }

   void AsRoot(const std::string &scratch)
   {
      using HM::RootCommand::AsOwner;
      using HM::RootCommand::Refused;
      const HM::RootCommand::Outcome asRoot = HM::RootCommand::AsRoot;
      const uid_t them = 65534;
      const uid_t third = 65533;
      gid_t theirGroup = 65534;
      {
         std::vector<char> buffer(16384);
         struct passwd entry {};
         struct passwd *found = nullptr;
         if (::getpwuid_r(them, &entry, buffer.data(), buffer.size(), &found) == 0 && found != nullptr)
            theirGroup = found->pw_gid;
      }

      // ------------------------------------------------ root's alone: runs as root
      {
         Layout l = Lay(scratch, "all-roots");
         if (l.made && Link(l.elsewhere, l.data + "/x/kept-elsewhere") && Link("..", l.data + "/x/y/up") &&
             File(l.data + "/x/theirs.eml", them, theirGroup) && Make(l.data + "/x/shared", 0775, 0, 0))
         {
            Expect("a data directory root's alone, with a link of root's to a folder of root's, a link back up, a file of another account's and a folder its group (root's) may write, runs as root",
               Decide(l.data, l.logs, l.temp, l.ini), asRoot, { "Run as root: --apply-environment runs as root", "the data directory " + l.data + " and everything under it being root's alone" });
         }
      }

      {
         Layout l = Lay(scratch, "temp-not-yet");
         if (l.made)
            Expect("a temporary folder not made yet, in a folder root's alone, runs as root",
               Decide(l.data, l.logs, l.root + "/Temp-to-come", l.ini), asRoot, { "Run as root:" });
      }

      {
         Layout l = Lay(scratch, "data-is-a-link");
         if (l.made && Link(l.data, l.root + "/Data-link"))
            Expect("a data directory named through a link of root's, in a folder root's alone, runs as root",
               Decide(l.root + "/Data-link", l.logs, l.temp, l.ini), asRoot, { "Run as root:" });
      }

      // ------------------------------------------------ not root's alone: refused
      {
         Layout l = Lay(scratch, "their-subfolder");
         if (l.made && ::chown((l.data + "/x/y").c_str(), them, theirGroup) == 0)
            Expect("a folder in the data directory that belongs to another account is refused, naming it",
               Decide(l.data, l.logs, l.temp, l.ini), Refused, { l.data + "/x/y belongs to ", "uid 65534", "Nothing was changed." });
      }

      {
         Layout l = Lay(scratch, "their-group");
         if (l.made && ::chown((l.data + "/x").c_str(), 0, 65534) == 0 && ::chmod((l.data + "/x").c_str(), 0775) == 0)
            Expect("a folder in the data directory that a group other than root's may write is refused, naming it",
               Decide(l.data, l.logs, l.temp, l.ini), Refused, { l.data + "/x may be written by the group ", "gid 65534" });
      }

      {
         Layout l = Lay(scratch, "everybody-writes");
         if (l.made && ::chmod((l.data + "/x/y").c_str(), 01777) == 0)
            Expect("a folder in the data directory that every account may write, sticky or not, is refused, naming it",
               Decide(l.data, l.logs, l.temp, l.ini), Refused, { l.data + "/x/y may be written by every account" });
      }

      {
         Layout l = Lay(scratch, "their-link");
         if (l.made && Link(l.elsewhere, l.data + "/x/planted", them, theirGroup))
            Expect("a symbolic link in the data directory that belongs to another account is refused, naming it",
               Decide(l.data, l.logs, l.temp, l.ini), Refused, { l.data + "/x/planted is a symbolic link that belongs to ", "uid 65534" });
      }

      {
         Layout l = Lay(scratch, "link-their-way");
         if (l.made && Make(l.root + "/theirs", 0755, them, theirGroup) && Make(l.root + "/theirs/store", 0755) &&
             Link(l.root + "/theirs/store", l.data + "/x/away"))
            Expect("a link of root's whose way leads through a folder of another account's is refused, naming the link and the folder",
               Decide(l.data, l.logs, l.temp, l.ini), Refused, { l.data + "/x/away is a symbolic link of root's, and the way it leads is not root's alone", l.root + "/theirs may be changed" });
      }

      {
         Layout l = Lay(scratch, "link-their-tree");
         if (l.made && ::chown((l.elsewhere + "/inner").c_str(), them, theirGroup) == 0 && Link(l.elsewhere, l.data + "/x/away"))
            Expect("a link of root's to a folder of root's with another account's folder in it is refused, naming that folder",
               Decide(l.data, l.logs, l.temp, l.ini), Refused, { l.elsewhere + "/inner belongs to " });
      }

      {
         Layout l = Lay(scratch, "link-their-folder");
         if (l.made && ::chown(l.elsewhere.c_str(), them, theirGroup) == 0 && ::chown((l.elsewhere + "/inner").c_str(), 0, 0) == 0 &&
             Link(l.elsewhere + "/inner", l.data + "/x/away"))
            Expect("a link of root's into a folder of another account's is refused, naming the folder above where it leads",
               Decide(l.data, l.logs, l.temp, l.ini), Refused, { l.data + "/x/away is a symbolic link of root's", l.elsewhere + " may be changed" });
      }

      {
         Layout l = Lay(scratch, "their-logs");
         if (l.made && ::chown(l.logs.c_str(), them, theirGroup) == 0)
            Expect("a log directory that belongs to another account, beside a data directory root's alone, is refused, naming it",
               Decide(l.data, l.logs, l.temp, l.ini), Refused, { "the log directory ([Directories] LogFolder) " + l.logs + " is not root's alone", l.logs + " belongs to " });
      }

      {
         Layout l = Lay(scratch, "their-way");
         if (l.made && Make(l.root + "/theirs", 0755, them, theirGroup) && Make(l.root + "/theirs/Data", 0755))
            Expect("a data directory of root's below a folder of another account's is refused, naming that folder",
               Decide(l.root + "/theirs/Data", l.logs, l.temp, l.ini), Refused, { "the way to the data directory " + l.root + "/theirs/Data is not root's alone", l.root + "/theirs may be changed" });
      }

      {
         Layout l = Lay(scratch, "temp-their-folder");
         if (l.made && Make(l.root + "/theirs", 0755, them, theirGroup))
            Expect("a temporary folder not made yet, in a folder of another account's, is refused, naming that folder",
               Decide(l.data, l.logs, l.root + "/theirs/Temp", l.ini), Refused, { l.root + "/theirs/Temp is not there yet", l.root + "/theirs belongs to " });
      }

      {
         Layout l = Lay(scratch, "temp-no-holder");
         if (l.made)
            Expect("a temporary folder whose folder is not there either is refused, saying so",
               Decide(l.data, l.logs, l.root + "/gone/Temp", l.ini), Refused, { l.root + "/gone/Temp is not there, and neither is the folder that would hold it, " + l.root + "/gone" });
      }

      {
         Layout l = Lay(scratch, "link-to-their-folder");
         if (l.made && Make(l.root + "/theirs", 0755, them, theirGroup) && Link(l.root + "/theirs", l.data + "/x/away"))
            Expect("a link of root's straight to a folder of another account's is refused, naming the folder it leads to",
               Decide(l.data, l.logs, l.temp, l.ini), Refused, { "the folder the symbolic link " + l.data + "/x/away leads to, " + l.root + "/theirs, belongs to " });
      }

      // ------------------------------------------------ the configuration
      {
         Layout l = Lay(scratch, "ini-through-link");
         if (l.made && Link(l.ini, l.root + "/hMailServer.ini"))
            Expect("a configuration named through a link of root's to a file of root's, beside a tree root's alone, runs as root",
               Decide(l.data, l.logs, l.temp, l.root + "/hMailServer.ini"), asRoot, { "Run as root:", "the configuration " + l.root + "/hMailServer.ini" });
      }

      {
         Layout l = Lay(scratch, "ini-theirs");
         if (l.made && ::chown(l.ini.c_str(), them, theirGroup) == 0)
            Expect("a configuration another account owns, beside a tree root's alone, is refused, naming it",
               Decide(l.data, l.logs, l.temp, l.ini), Refused, { "the configuration " + l.ini + ", which names them, is not root's alone", l.ini + " belongs to ", "uid 65534" });
      }

      {
         Layout l = Lay(scratch, "ini-their-group");
         if (l.made && ::chown(l.ini.c_str(), 0, 65534) == 0 && ::chmod(l.ini.c_str(), 0664) == 0)
            Expect("a configuration a group other than root's may write is refused, naming it",
               Decide(l.data, l.logs, l.temp, l.ini), Refused, { l.ini + " may be written by the group ", "gid 65534" });
      }

      {
         Layout l = Lay(scratch, "ini-their-folder");
         if (l.made && ::chown((l.root + "/program").c_str(), them, theirGroup) == 0)
            Expect("a configuration of root's in a folder of another account's is refused, naming the folder",
               Decide(l.data, l.logs, l.temp, l.ini), Refused, { "the way to the configuration " + l.ini + ", which names them, is not root's alone: ", l.root + "/program may be changed" });
      }

      {
         Layout l = Lay(scratch, "ini-link-their-file");
         if (l.made && Make(l.root + "/theirs", 0755, them, theirGroup) && File(l.root + "/theirs/hMailServer.ini") &&
             Link(l.root + "/theirs/hMailServer.ini", l.root + "/hMailServer.ini"))
            Expect("a configuration named through a link of root's into a folder of another account's is refused, naming the folder",
               Decide(l.data, l.logs, l.temp, l.root + "/hMailServer.ini"), Refused, { "the way to the configuration " + l.root + "/hMailServer.ini, which names them, is not root's alone: ", l.root + "/theirs may be changed" });
      }

      // ------------------------------------------------ access control lists
      {
         Layout l = Lay(scratch, "access-list");
         const std::string folder = l.data + "/x";
         if (l.made && ::chmod(folder.c_str(), 0775) == 0 &&
             PutList(folder, "system.posix_acl_access", { { 1, 7, Unset }, { 2, 7, them }, { 4, 5, Unset }, { 16, 7, Unset }, { 32, 5, Unset } }))
            Expect("a folder of root's group whose group's write is an access control list's mask, giving another account a write, is refused, naming it",
               Decide(l.data, l.logs, l.temp, l.ini), Refused, { folder + " may be written by its group, and has an access control list" });
         else if (l.made)
            (void) std::printf("SKIP  an access control list on a folder: this file system keeps none (%s)\n", ErrorText(errno).c_str());
      }

      {
         Layout l = Lay(scratch, "default-list");
         const std::string folder = l.data + "/x";
         if (l.made && PutList(folder, "system.posix_acl_default", { { 1, 7, Unset }, { 2, 7, them }, { 4, 5, Unset }, { 16, 7, Unset }, { 32, 5, Unset } }))
            Expect("a folder of root's whose default access control list gives another account a write to what is made in it is refused, naming it and the account",
               Decide(l.data, l.logs, l.temp, l.ini), Refused, { folder + " has a default access control list that gives ", "uid 65534", " a write to what is made in it" });
         else if (l.made)
            (void) std::printf("SKIP  a default access control list: this file system keeps none (%s)\n", ErrorText(errno).c_str());
      }

      {
         Layout l = Lay(scratch, "default-list-others");
         const std::string folder = l.data + "/x/y";
         if (l.made && PutList(folder, "system.posix_acl_default", { { 1, 7, Unset }, { 4, 5, Unset }, { 32, 7, Unset } }))
            Expect("a folder of root's whose default access control list gives every account a write to what is made in it is refused, naming it",
               Decide(l.data, l.logs, l.temp, l.ini), Refused, { folder + " has a default access control list that gives every account a write" });
      }

      {
         Layout l = Lay(scratch, "default-list-masked");
         const std::string folder = l.data + "/x";
         if (l.made && PutList(folder, "system.posix_acl_default", { { 1, 7, Unset }, { 2, 7, them }, { 4, 5, Unset }, { 16, 5, Unset }, { 32, 5, Unset } }))
            Expect("a default access control list whose mask holds another account's write back runs as root",
               Decide(l.data, l.logs, l.temp, l.ini), asRoot, { "Run as root:" });
      }

      {
         Layout l = Lay(scratch, "default-list-set-group");
         const std::string folder = l.data + "/x";
         if (l.made && ::chown(folder.c_str(), 0, 65534) == 0 && ::chmod(folder.c_str(), 02755) == 0 &&
             PutList(folder, "system.posix_acl_default", { { 1, 7, Unset }, { 4, 7, Unset }, { 32, 5, Unset } }))
            Expect("a set-group-ID folder of another group whose default access control list gives the group a write to what is made in it is refused, naming the group",
               Decide(l.data, l.logs, l.temp, l.ini), Refused, { folder + " has a default access control list that gives the group ", "gid 65534" });
      }

      // ------------------------------------------------ the owner's: as before
      {
         Layout l = Lay(scratch, "owners");
         if (l.made && ::chown(l.data.c_str(), them, theirGroup) == 0 && ::chown(l.logs.c_str(), them, theirGroup) == 0)
         {
            const HM::RootCommand::Decision decision = Decide(l.data, l.logs, l.temp, l.ini);
            Expect("a data directory another account owns, the way to it root's, becomes that account (whatever the log directory is)",
               decision, AsOwner, {});
            Report(decision.outcome != AsOwner || (decision.uid == them && decision.gid == theirGroup), "... as uid 65534 and its group",
               "decided uid " + std::to_string((unsigned long) decision.uid) + " gid " + std::to_string((unsigned long) decision.gid));
         }
      }

      {
         Layout l = Lay(scratch, "owners-configuration");
         if (l.made && ::chown(l.data.c_str(), them, theirGroup) == 0 && ::chown(l.ini.c_str(), them, theirGroup) == 0)
            Expect("a data directory another account owns becomes that account whoever owns the configuration, as before",
               Decide(l.data, l.logs, l.temp, l.ini), AsOwner, {});
      }

      {
         Layout l = Lay(scratch, "owners-own-way");
         if (l.made && Make(l.root + "/theirs", 0775, them, theirGroup) && Make(l.root + "/theirs/Data", 0755, them, theirGroup))
            Expect("a data directory another account owns, below a folder of that account's its own group may write, becomes that account",
               Decide(l.root + "/theirs/Data", l.logs, l.temp, l.ini), AsOwner, {});
      }

      {
         Layout l = Lay(scratch, "owners-third-way");
         if (l.made && Make(l.root + "/third", 0755, third, third) && Make(l.root + "/third/Data", 0755, them, theirGroup))
            Expect("a data directory another account owns, below a folder a third account owns, is refused, naming that folder",
               Decide(l.root + "/third/Data", l.logs, l.temp, l.ini), Refused, { "the way to it is not root's and that owner's alone", l.root + "/third may be changed by an account other than root and the data directory's owner" });
      }

      {
         Layout l = Lay(scratch, "no-data");
         if (l.made)
            Expect("a data directory that is not there is refused",
               Decide(l.root + "/Missing", l.logs, l.temp, l.ini), Refused, { "the data directory " + l.root + "/Missing could not be examined" });
      }
   }

   void NotRoot()
   {
      // A scratch folder of this user's, in the temporary folder.
      std::string pattern = "/tmp/root-command-tests.XXXXXX";
      std::vector<char> buffer(pattern.begin(), pattern.end());
      buffer.push_back('\0');
      if (::mkdtemp(buffer.data()) == nullptr)
      {
         Report(false, "the scratch folder", ErrorText(errno).c_str());
         return;
      }

      const std::string scratch(buffer.data());
      const std::string data = scratch + "/Data";
      if (::mkdir(data.c_str(), 0755) == 0)
      {
         std::string why;
         const bool tree = HM::RootCommand::TreeIsRootsAlone(data, why);
         Report(!tree && Contains(why, data + " belongs to "), "a tree of this user's is not root's alone", why.empty() ? "it was taken for root's" : why);

         struct stat held {};
         if (::lstat("/tmp", &held) == 0 && (held.st_mode & S_IWOTH) != 0)
         {
            std::string way;
            const bool roots = HM::RootCommand::WayIsRootsOr("/tmp/hmailserver-root-command-tests", 0, 0, way);
            Report(!roots && Contains(way, "/tmp may be changed by an account other than root"), "a way through a folder every account may write (/tmp) is not root's alone",
               way.empty() ? "it was taken for root's" : way);

            const std::string ini = scratch + "/hMailServer.ini";
            FILE *file = std::fopen(ini.c_str(), "w");
            if (file != nullptr)
            {
               (void) std::fclose(file);
               std::string unsettled;
               const bool configuration = HM::RootCommand::ConfigurationIsRootsAlone(ini, unsettled);
               Report(!configuration && Contains(unsettled, "the way to it is not root's alone: /tmp may be changed"), "a configuration of this user's in /tmp is not root's alone",
                  unsettled.empty() ? "it was taken for root's" : unsettled);
            }
         }
      }

      RemoveAll(scratch);
      (void) std::printf("SKIP  the cases that give folders and links to other accounts: they need root, which this run does not have (uid %u)\n",
         (unsigned int) ::geteuid());
   }
}

namespace
{
   // KeepWhatItMakesRoots: every other account's write taken away, and
   // nothing else the umask takes away given back.
   std::string Octal(mode_t mode)
   {
      char text[16] = {};
      (void) std::snprintf(text, sizeof(text), "%04o", (unsigned int) mode);
      return text;
   }

   void Umask()
   {
      const mode_t cases[][2] = { { 0, 022 }, { 002, 022 }, { 027, 027 }, { 077, 077 } };
      for (const auto &held : cases)
      {
         (void) ::umask(held[0]);
         HM::RootCommand::KeepWhatItMakesRoots();
         const mode_t now = ::umask(022);
         Report(now == held[1], "KeepWhatItMakesRoots under umask " + Octal(held[0]) + " leaves " + Octal(held[1]), "it left " + Octal(now));
      }
   }
}

static int
Run()
{
   Umask();
   (void) ::umask(022);

   if (::geteuid() != 0)
   {
      NotRoot();
   }
   else
   {
      const std::string folder = RootOnlyFolder();
      if (folder.empty())
      {
         (void) std::printf("FAIL  no folder only root may change to lay the cases out in: /var/lib, /run and /root are not\n");
         return 1;
      }

      std::string pattern = folder + "/hmailserver-root-command-tests.XXXXXX";
      std::vector<char> buffer(pattern.begin(), pattern.end());
      buffer.push_back('\0');
      if (::mkdtemp(buffer.data()) == nullptr || ::chmod(buffer.data(), 0755) != 0)
      {
         (void) std::printf("FAIL  the scratch folder could not be made in %s: %s\n", folder.c_str(), ErrorText(errno).c_str());
         return 1;
      }

      const std::string scratch(buffer.data());
      (void) std::printf("the cases are laid out in %s\n", scratch.c_str());

      // Root in a user namespace that maps no uid 65534 cannot give it a folder:
      // the cases are then not this program's to run.
      const std::string probe = scratch + "/probe";
      if (::mkdir(probe.c_str(), 0755) != 0 || ::chown(probe.c_str(), 65534, 65534) != 0)
      {
         (void) std::printf("SKIP  the cases that give folders and links to other accounts: root here cannot give a folder to uid 65534 (%s)\n", ErrorText(errno).c_str());
         RemoveAll(scratch);
         (void) std::printf("%d passed, %d failed\n", passes, failures);
         return 0;
      }

      AsRoot(scratch);
      RemoveAll(scratch);
   }

   (void) std::printf("%d passed, %d failed\n", passes, failures);
   return failures == 0 ? 0 : 1;
}

int
main(int /*argc*/, char * /*argv*/[])
{
   try
   {
      return Run();
   }
   catch (const std::exception &failure)
   {
      (void) std::printf("FAIL  %s\n", failure.what());
      return 1;
   }
}
