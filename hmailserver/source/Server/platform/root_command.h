// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// How a command that works in the service's folders runs when root runs it
// (platform/main.cpp, RunAsServiceAccount; docs/ServiceAccount.md, "On Linux, a
// command run as root"): the store walks, the copies to and from an object
// store, --sample-domain, --apply-profile, --revert-profile and
// --apply-environment.
//
// Root going by a path follows every symbolic link on it, so the one rule is that
// root never follows a link an account other than root could have put where the
// command goes. Two layouts keep it:
//
//   * the data directory belongs to another account - the service's, as the
//     package makes /var/lib/hmailserver for the unit's User= - and the way to it
//     is root's and that account's alone: the command becomes that account, and
//     reaches only what the service reaches;
//   * the data directory belongs to root - a service run as root, as a source
//     build or a container without User= runs it, and as the Linux regression
//     suite does - and it, everything under it and the way to it are root's
//     alone, and so are the other folders the command uses (the log, temporary,
//     event and database folders) and the configuration that names them all,
//     which the command reads again as it starts and writes its settings back
//     to: the command crosses no boundary, and runs on as root, making what it
//     makes for root alone (KeepWhatItMakesRoots).
//
// Anything else is refused, exit 2, naming the first folder or link that fails.
// Its own unit, so that root-command-tests (platform/root_command_tests_main.cpp)
// holds the decision to the layouts above without a server.
//
// Not built on Windows, where the store commands hold the data folder from its
// drive's root down instead (MessageStoreMaintenance, OnlyAdministratorsControl).
#pragma once

#include <string>
#include <vector>
#include <sys/types.h>

namespace HM
{
   namespace RootCommand
   {
      // Whether the way to path is root's or owner's alone: every folder whose
      // names decide where it leads - each folder above it, and above where any
      // symbolic link on the way points - belongs to root or to owner, and
      // nobody else may change its names (no write for others; for its group
      // only when that is root's group, or owner's own group on owner's folder;
      // none for a group whose bits are an access control list's mask). The
      // folder path names itself is not judged: TreeIsRootsAlone judges that.
      // why: the folder that is not, or what could not be told.
      bool WayIsRootsOr(const std::string &path, uid_t owner, gid_t ownerGroup, std::string &why);

      // Whether the folder path names - and every folder and symbolic link
      // under it, and every folder a symbolic link under it leads to - is
      // root's alone: each folder belongs to root and may be written by no
      // other account (no write for others; for its group only when that is
      // root's group and no access control list stands behind the group's
      // bits), carries no default access control list that would give another
      // account a write to what is made in it, and each symbolic link belongs
      // to root and leads where WayIsRootsOr(link, 0, 0) allows. Walked with
      // lstat, never following a link but the ones it has judged, each folder
      // once however many links lead to it. Files are not judged: in a folder
      // only root may change, no other account can put one there or replace it
      // with a link. why: the first folder or link that is not, and why.
      bool TreeIsRootsAlone(const std::string &path, std::string &why);

      // Whether the configuration file path names is root's alone: the way to
      // it as WayIsRootsOr(path, 0, 0) allows, and the file it leads to
      // root's, a file, and written by no other account (as a folder above).
      // It names every folder the command goes to and is read again when the
      // command starts, so an account that could change it could choose them.
      // why: what is not, and why.
      bool ConfigurationIsRootsAlone(const std::string &path, std::string &why);

      // For a command that runs on as root (AsRoot): what it makes from here on
      // - a folder for a new account, a temporary file, a copy of a message -
      // is root's alone too, whatever umask it was started under (a docker
      // exec or a CI job may give 0000): the umask takes away at least every
      // other account's write (022). A default access control list, which the
      // umask does not reach, is refused by the walk instead.
      void KeepWhatItMakesRoots();

      enum Outcome
      {
         // Become the data directory's owner (uid, gid, name) before anything
         // is opened.
         AsOwner,
         // Run on as root: the data directory and everything it uses are
         // root's alone.
         AsRoot,
         // Neither: exit 2 with message.
         Refused
      };

      // A folder the command uses besides the data directory, and what it is
      // ("the log directory ([Directories] LogFolder)").
      struct Folder
      {
         std::string path;
         std::string what;
      };

      struct Decision
      {
         Outcome outcome = Refused;
         uid_t uid = 0;
         gid_t gid = 0;
         // The owner's account name, empty when the password database has none.
         std::string name;
         // What to say on standard error, without the line end: the refusal, or
         // for AsRoot the line saying so (AsOwner's is said once it is that
         // account).
         std::string message;
      };

      // How verb, run as root, runs over dataDirectory; others are the other
      // folders it uses and configuration the file that names them all, both
      // judged only when the data directory is root's (an empty folder path is
      // not set, and is skipped). A folder that does not exist yet is judged by
      // the folder that would hold it.
      Decision Decide(const std::string &verb, const std::string &dataDirectory, const std::vector<Folder> &others, const std::string &configuration);
   }
}
