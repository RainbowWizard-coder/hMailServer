// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// How a command that works in the service's folders runs when root runs it:
// root_command.h says what and why.

#include "root_command.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <set>
#include <system_error>
#include <utility>
#include <dirent.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

namespace HM
{
   namespace RootCommand
   {
      namespace
      {
         // "chris (uid 1000)", or "uid 1000" for an account the password
         // database does not name.
         std::string Account(uid_t uid)
         {
            std::vector<char> buffer(16384);
            struct passwd entry {};
            struct passwd *found = nullptr;
            const std::string number = "uid " + std::to_string((unsigned long) uid);
            if (::getpwuid_r(uid, &entry, buffer.data(), buffer.size(), &found) == 0 && found != nullptr && found->pw_name != nullptr)
               return std::string(found->pw_name) + " (" + number + ")";
            return number;
         }

         std::string Group(gid_t gid)
         {
            std::vector<char> buffer(16384);
            struct group entry {};
            struct group *found = nullptr;
            const std::string number = "gid " + std::to_string((unsigned long) gid);
            if (::getgrgid_r(gid, &entry, buffer.data(), buffer.size(), &found) == 0 && found != nullptr && found->gr_name != nullptr)
               return std::string(found->gr_name) + " (" + number + ")";
            return number;
         }

         // What errno code says, as strerror says it, without strerror's shared
         // buffer.
         std::string ErrorText(int code)
         {
            return std::generic_category().message(code);
         }

         // Whether the folder carries an access control list, whose mask its
         // group's bits then are: a write the bits give the group may be a
         // write the list gives some other account. Not followed through a
         // link. A file system that keeps no lists has none.
         bool HasAccessList(const std::string &path)
         {
            return ::lgetxattr(path.c_str(), "system.posix_acl_access", nullptr, 0) >= 0;
         }

         // Whether the folder's default access control list - the one what is
         // made in it takes its own from, which the umask does not reach - gives
         // an account other than root a write to what the command makes there
         // while it runs: every account (others), a named user or group not
         // root's, or the group of what is made there when that is not root's
         // (the folder's own, the folder being set-group-ID). The mask, where
         // there is one, holds back all but the owner and others, as it does on
         // the folder. A list that cannot be read whole is taken to give one.
         // Not followed through a link. what: whom it gives the write to.
         bool DefaultListGivesWrite(const std::string &path, const struct stat &held, std::string &what)
         {
            const char *const name = "system.posix_acl_default";
            const ssize_t size = ::lgetxattr(path.c_str(), name, nullptr, 0);
            if (size < 0)
               return false;

            // The kernel's form (posix_acl_xattr): a version of 2, then entries
            // of a tag, the permissions and an id, all little-endian.
            std::vector<unsigned char> list((size_t) size);
            const ssize_t got = size == 0 ? 0 : ::lgetxattr(path.c_str(), name, list.data(), list.size());
            const auto at16 = [&list](size_t at) { return (unsigned int) list[at] | ((unsigned int) list[at + 1] << 8); };
            const auto at32 = [&list](size_t at) { return (unsigned long) list[at] | ((unsigned long) list[at + 1] << 8) | ((unsigned long) list[at + 2] << 16) | ((unsigned long) list[at + 3] << 24); };
            if (got != size || got < 4 || (got - 4) % 8 != 0 || at32(0) != 2)
            {
               what = "a default access control list that could not be read, which may give another account a write to what is made in it";
               return true;
            }

            bool masked = false;
            std::string others;
            std::string beyond;
            for (size_t at = 4; at + 8 <= list.size(); at += 8)
            {
               const unsigned int tag = at16(at);
               const bool writes = (at16(at + 2) & 0x02) != 0;
               const unsigned long id = at32(at + 4);
               if (tag == 0x10)
                  masked = !writes;
               else if (!writes)
                  continue;
               else if (tag == 0x20)
                  others = "every account";
               else if (tag == 0x02 && id != 0 && beyond.empty())
                  beyond = Account((uid_t) id);
               else if (tag == 0x08 && id != 0 && beyond.empty())
                  beyond = "the group " + Group((gid_t) id);
               else if (tag == 0x04 && (held.st_mode & S_ISGID) != 0 && held.st_gid != 0 && beyond.empty())
                  beyond = "the group " + Group(held.st_gid) + ", which what is made in it takes, the folder being set-group-ID,";
            }

            const std::string who = !others.empty() ? others : (masked ? std::string() : beyond);
            if (who.empty())
               return false;

            what = "a default access control list that gives " + who + " a write to what is made in it";
            return true;
         }

         // The entry's own owner and mode, as TreeIsRootsAlone holds each folder
         // and ConfigurationIsRootsAlone the configuration: root's, and nobody but
         // root may change it - for a folder, its names, and what is made in it.
         // path is the entry itself, no link to it; label names it in why, which
         // says who else may.
         bool EntryIsRoots(const std::string &path, const std::string &label, const struct stat &held, std::string &why)
         {
            if (held.st_uid != 0)
            {
               why = label + " belongs to " + Account(held.st_uid) + ", not root";
               return false;
            }

            if ((held.st_mode & S_IWOTH) != 0)
            {
               why = label + " may be written by every account";
               return false;
            }

            if ((held.st_mode & S_IWGRP) != 0)
            {
               if (held.st_gid != 0)
               {
                  why = label + " may be written by the group " + Group(held.st_gid) + ", which is not root's";
                  return false;
               }

               if (HasAccessList(path))
               {
                  why = label + " may be written by its group, and has an access control list, which may give that write to another account";
                  return false;
               }
            }

            std::string given;
            if (S_ISDIR(held.st_mode) && DefaultListGivesWrite(path, held, given))
            {
               why = label + " has " + given;
               return false;
            }

            return true;
         }

         // The folder path names, through any links on the way, by the name
         // with none: realpath's.
         bool Resolve(const std::string &path, std::string &resolved, std::string &why)
         {
            std::vector<char> buffer(PATH_MAX + 1);
            if (::realpath(path.c_str(), buffer.data()) == nullptr)
            {
               why = path + " could not be examined: " + ErrorText(errno);
               return false;
            }

            resolved = buffer.data();
            return true;
         }

         // The folders walked so far, by device and inode: a folder two links
         // lead to, or one a link inside it leads back to, is walked once.
         typedef std::set<std::pair<dev_t, ino_t>> Walked;

         bool WalkTree(const std::string &start, Walked &walked, std::string &why)
         {
            // The folder start names, through the links on the way to it,
            // which the caller has had WayIsRootsOr judge.
            std::string first;
            if (!Resolve(start, first, why))
               return false;

            struct stat held {};
            if (::lstat(first.c_str(), &held) != 0 || !S_ISDIR(held.st_mode))
            {
               why = start + " is not a folder";
               return false;
            }

            if (!EntryIsRoots(first, start, held, why))
               return false;

            if (!walked.insert(std::make_pair(held.st_dev, held.st_ino)).second)
               return true;

            std::vector<std::string> pending(1, first);
            while (!pending.empty())
            {
               const std::string folder = pending.back();
               pending.pop_back();

               DIR *listing = ::opendir(folder.c_str());
               if (listing == nullptr)
               {
                  why = folder + " could not be read: " + ErrorText(errno);
                  return false;
               }

               std::vector<std::string> links;
               bool failed = false;
               for (;;)
               {
                  errno = 0;
                  // NOLINT below: the listing is this call's own, read by this
                  // thread alone, which is all readdir asks.
                  const struct dirent *entry = ::readdir(listing); // NOLINT(concurrency-mt-unsafe)
                  if (entry == nullptr)
                  {
                     if (errno != 0)
                     {
                        why = folder + " could not be read: " + ErrorText(errno);
                        failed = true;
                     }
                     break;
                  }

                  const std::string name(entry->d_name);
                  if (name == "." || name == "..")
                     continue;

                  // A file in a folder only root may change is root's to have
                  // put there: not judged, and not even looked at where the
                  // listing already says it is a file.
                  if (entry->d_type == DT_REG)
                     continue;

                  const std::string path = (folder == "/" ? std::string() : folder) + "/" + name;
                  struct stat named {};
                  if (::lstat(path.c_str(), &named) != 0)
                  {
                     why = path + " could not be examined: " + ErrorText(errno);
                     failed = true;
                     break;
                  }

                  if (S_ISDIR(named.st_mode))
                  {
                     if (!EntryIsRoots(path, path, named, why))
                     {
                        failed = true;
                        break;
                     }

                     if (walked.insert(std::make_pair(named.st_dev, named.st_ino)).second)
                        pending.push_back(path);
                     continue;
                  }

                  if (S_ISLNK(named.st_mode))
                  {
                     if (named.st_uid != 0)
                     {
                        why = path + " is a symbolic link that belongs to " + Account(named.st_uid) + ", not root";
                        failed = true;
                        break;
                     }

                     links.push_back(path);
                  }
               }

               (void) ::closedir(listing);
               if (failed)
                  return false;

               // Each of root's links: where it leads, as root would go there,
               // and - when that is a folder - all of it, as the command would
               // walk into it.
               for (const std::string &link : links)
               {
                  std::string way;
                  if (!WayIsRootsOr(link, 0, 0, way))
                  {
                     why = link + " is a symbolic link of root's, and the way it leads is not root's alone: " + way;
                     return false;
                  }

                  struct stat target {};
                  if (::stat(link.c_str(), &target) != 0)
                  {
                     why = link + " is a symbolic link of root's that leads nowhere that could be examined: " + ErrorText(errno);
                     return false;
                  }

                  if (!S_ISDIR(target.st_mode))
                     continue;

                  std::string resolved;
                  if (!Resolve(link, resolved, why))
                     return false;

                  if (!EntryIsRoots(resolved, "the folder the symbolic link " + link + " leads to, " + resolved + ",", target, why))
                     return false;

                  if (walked.insert(std::make_pair(target.st_dev, target.st_ino)).second)
                     pending.push_back(resolved);
               }
            }

            return true;
         }

         // A folder the command uses: the way to it and all of it root's alone
         // - or, where it is not there yet, the way to the folder that would
         // hold it and that folder itself, since the command running as root
         // could make it there. what names it in why.
         bool UsedFolderIsRoots(const std::string &path, const std::string &what, Walked &walked, std::string &why)
         {
            struct stat named {};
            if (::lstat(path.c_str(), &named) != 0 && errno == ENOENT)
            {
               const size_t slash = path.find_last_of('/');
               const std::string parent = slash == std::string::npos ? std::string(".") : (slash == 0 ? std::string("/") : path.substr(0, slash));

               // Whether the folder that would hold it is there first, so that a
               // missing one is said as that and not as a way that is not root's.
               struct stat holder {};
               if (::stat(parent.c_str(), &holder) != 0 || !S_ISDIR(holder.st_mode))
               {
                  why = what + " " + path + " is not there, and neither is the folder that would hold it, " + parent;
                  return false;
               }

               std::string way;
               if (!WayIsRootsOr(parent, 0, 0, way))
               {
                  why = what + " " + path + " is not there yet, and the way to " + parent + ", which would hold it, is not root's alone: " + way;
                  return false;
               }

               std::string resolved;
               std::string folder;
               if (!Resolve(parent, resolved, folder) || !EntryIsRoots(resolved, parent, holder, folder))
               {
                  why = what + " " + path + " is not there yet, and the folder that would hold it is not root's alone: " + folder;
                  return false;
               }

               return true;
            }

            std::string way;
            if (!WayIsRootsOr(path, 0, 0, way))
            {
               why = "the way to " + what + " " + path + " is not root's alone: " + way;
               return false;
            }

            std::string tree;
            if (!WalkTree(path, walked, tree))
            {
               why = what + " " + path + " is not root's alone: " + tree;
               return false;
            }

            return true;
         }
      }

      bool WayIsRootsOr(const std::string &path, uid_t owner, gid_t ownerGroup, std::string &why)
      {
         std::string full = path;
         if (full.empty() || full[0] != '/')
         {
            std::vector<char> here(4096);
            if (::getcwd(here.data(), here.size()) == nullptr)
            {
               why = "the folder it is relative to could not be told";
               return false;
            }
            full = std::string(here.data()) + "/" + full;
         }

         // The names still to walk, the next one last; the folders walked so far.
         std::vector<std::string> pending;
         std::vector<std::string> walked;
         const auto add = [&pending](const std::string &more)
         {
            std::vector<std::string> names;
            size_t at = 0;
            while (at <= more.size())
            {
               const size_t next = more.find('/', at);
               const size_t end = next == std::string::npos ? more.size() : next;
               if (end > at)
                  names.push_back(more.substr(at, end - at));
               at = end + 1;
            }
            pending.insert(pending.end(), names.rbegin(), names.rend());
         };
         add(full);

         int links = 0;
         while (!pending.empty())
         {
            const std::string name = pending.back();
            pending.pop_back();
            if (name == ".")
               continue;
            if (name == "..")
            {
               if (!walked.empty())
                  walked.pop_back();
               continue;
            }

            std::string folder;
            for (const std::string &part : walked)
               folder += "/" + part;
            if (folder.empty())
               folder = "/";

            struct stat held {};
            if (::lstat(folder.c_str(), &held) != 0 || !S_ISDIR(held.st_mode))
            {
               why = folder + " could not be examined";
               return false;
            }

            const bool ownersOwn = held.st_uid == owner && held.st_gid == ownerGroup;
            const bool groupWrites = (held.st_mode & S_IWGRP) != 0;
            std::string who;
            if (held.st_uid != 0 && held.st_uid != owner)
               who = "it belongs to " + Account(held.st_uid);
            else if ((held.st_mode & S_IWOTH) != 0)
               who = "every account may write it";
            else if (groupWrites && held.st_gid != 0 && !ownersOwn)
               who = "the group " + Group(held.st_gid) + " may write it";
            else if (groupWrites && HasAccessList(folder))
               who = "its group may write it, and it has an access control list, which may give that write to another account";

            if (!who.empty())
            {
               why = folder + (owner == 0 ? " may be changed by an account other than root" : " may be changed by an account other than root and the data directory's owner") +
                  " (" + who + ")";
               return false;
            }

            const std::string next = (folder == "/" ? std::string() : folder) + "/" + name;
            struct stat named {};
            if (::lstat(next.c_str(), &named) != 0)
            {
               why = next + " could not be examined";
               return false;
            }

            if (!S_ISLNK(named.st_mode))
            {
               walked.push_back(name);
               continue;
            }

            if (++links > 40)
            {
               why = next + " leads through more than forty symbolic links";
               return false;
            }

            std::vector<char> target(4096);
            const ssize_t length = ::readlink(next.c_str(), target.data(), target.size() - 1);
            if (length <= 0 || (size_t) length >= target.size() - 1)
            {
               why = next + " is a symbolic link that could not be read";
               return false;
            }

            const std::string to(target.data(), (size_t) length);
            if (to[0] == '/')
               walked.clear();
            add(to);
         }

         return true;
      }

      bool TreeIsRootsAlone(const std::string &path, std::string &why)
      {
         Walked walked;
         return WalkTree(path, walked, why);
      }

      bool ConfigurationIsRootsAlone(const std::string &path, std::string &why)
      {
         if (path.empty())
         {
            why = "which file it is could not be told";
            return false;
         }

         std::string way;
         if (!WayIsRootsOr(path, 0, 0, way))
         {
            why = "the way to it is not root's alone: " + way;
            return false;
         }

         std::string resolved;
         if (!Resolve(path, resolved, why))
            return false;

         struct stat held {};
         if (::lstat(resolved.c_str(), &held) != 0 || !S_ISREG(held.st_mode))
         {
            why = path + " is not a file";
            return false;
         }

         return EntryIsRoots(resolved, resolved == path ? path : path + ", which leads to " + resolved + ",", held, why);
      }

      void KeepWhatItMakesRoots()
      {
         const mode_t held = ::umask(022);
         (void) ::umask(held | 022);
      }

      Decision Decide(const std::string &verb, const std::string &dataDirectory, const std::vector<Folder> &others, const std::string &configuration)
      {
         Decision decision;

         struct stat directory {};
         if (dataDirectory.empty() || ::stat(dataDirectory.c_str(), &directory) != 0 || !S_ISDIR(directory.st_mode))
         {
            decision.message = verb + " run as root runs as the account the service runs as, the owner of the data directory, and the data directory " + dataDirectory +
               " could not be examined. Run it as that account (sudo -u hmailserver hmailserver ...). Nothing was changed.";
            return decision;
         }

         if (directory.st_uid != 0)
         {
            // The service's own account: the command becomes it, when nobody
            // else could have chosen which folder the configured path leads to
            // - a folder above it that another account may change could hold a
            // link of that account's to a folder some third account owns, which
            // root would then become.
            decision.uid = directory.st_uid;
            decision.gid = directory.st_gid;

            std::vector<char> buffer(16384);
            struct passwd entry {};
            struct passwd *found = nullptr;
            if (::getpwuid_r(decision.uid, &entry, buffer.data(), buffer.size(), &found) == 0 && found != nullptr)
            {
               decision.name = found->pw_name;
               decision.gid = found->pw_gid;
            }

            std::string unsafe;
            if (!WayIsRootsOr(dataDirectory, decision.uid, decision.gid, unsafe))
            {
               decision.message = verb + " run as root runs as the account the service runs as, the owner of the data directory " + dataDirectory +
                  ", and the way to it is not root's and that owner's alone: " + unsafe + ", so the owner does not tell the account. "
                  "Give the folders above the data directory to root, or run it as the service's account (sudo -u hmailserver hmailserver ...). Nothing was changed.";
               return decision;
            }

            decision.outcome = AsOwner;
            return decision;
         }

         // Root's: a service run as root. The command runs on as root where it
         // crosses no boundary - the configuration, the data directory,
         // everything under it, the other folders it uses and the ways to them
         // root's alone - and is refused where an account other than root could
         // have put a link, or chosen the folders.
         const std::string remedy = " Make what it names root's alone (chown root:root and chmod go-w a folder or file, setfacl -bk one with an access control list, "
            "remove a link another account owns), or name folders in the configuration that are, "
            "or give the data directory to the account the service runs as (the package makes it hmailserver's) to have the command run as that account, or run it as "
            "that account (sudo -u hmailserver hmailserver ...). Nothing was changed.";

         // First the file that names every folder below, which the command reads
         // again as it starts and writes its settings back to: an account that
         // could change it could point the folders at a tree of root's of its
         // choosing, or change them once they are judged.
         const std::string chosen = verb + " run as root, with the data directory " + dataDirectory + " root's, runs as root only where no account but root could have chosen the folders it goes to "
            "or put a symbolic link where it goes, and ";
         std::string way;
         if (!configuration.empty() && !WayIsRootsOr(configuration, 0, 0, way))
         {
            decision.message = chosen + "the way to the configuration " + configuration + ", which names them, is not root's alone: " + way + "." + remedy;
            return decision;
         }

         std::string unsettled;
         if (!ConfigurationIsRootsAlone(configuration, unsettled))
         {
            decision.message = chosen + "the configuration " + configuration + ", which names them, is not root's alone: " + unsettled + "." + remedy;
            return decision;
         }

         std::vector<Folder> folders;
         folders.push_back({ dataDirectory, "the data directory" });
         for (const Folder &other : others)
         {
            if (!other.path.empty())
               folders.push_back(other);
         }

         Walked walked;
         for (const Folder &folder : folders)
         {
            std::string why;
            if (!UsedFolderIsRoots(folder.path, folder.what, walked, why))
            {
               decision.message = verb + " run as root, with the data directory " + dataDirectory + " root's, runs as root only where no account but root could have put a symbolic link where it goes, and " +
                  why + "." + remedy;
               return decision;
            }
         }

         decision.outcome = AsRoot;
         decision.message = "Run as root: " + verb + " runs as root, the data directory " + dataDirectory +
            " and everything under it being root's alone, as are the way to it, the log, temporary, event and database folders that are set and the configuration " + configuration +
            ": no account but root could have put a symbolic link where it goes, or chosen where that is.";
         return decision;
      }
   }
}
