// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

namespace HM
{
   // A directory ProtectedFile::EnsureDirectory has made safe, held open until
   // this is destroyed or released. On Windows the handle shares reading and
   // writing but not deleting, so the directory cannot be renamed, removed or
   // swapped for a link while it is held - and NTFS refuses to rename a
   // directory while anything below it is open, so neither can any folder above
   // it. What is checked by path while it is held is what was made safe. Nothing
   // is held on POSIX.
   class HeldDirectory
   {
   public:
      HeldDirectory();
      ~HeldDirectory();

      void Release();

   private:
      HeldDirectory(const HeldDirectory &);
      HeldDirectory &operator=(const HeldDirectory &);

      friend class ProtectedFile;
      void *handle_;
   };

   // A secret the server writes to disk for itself: readable by the account the
   // server runs as and by the machine's administrators, and by nobody else.
   //
   // Two things need this, and the rule is the same for both. The update apply
   // token is the administrator password for an hour; a DKIM private key signs
   // as a domain for as long as its record is published. A default Windows
   // installation puts the data directory under Program Files, where BUILTIN\Users
   // can read, so a file that merely inherits the directory's permissions hands
   // the secret to every local user. Moved here out of UpdateApplyToken on
   // 15 September 2026, when DKIM key rotation became the second writer, and
   // unchanged in what it does.
   class ProtectedFile
   {
   public:
      // Writes data to path as a new file with those permissions: on Windows a
      // protected DACL naming SYSTEM, BUILTIN\Administrators and the account the
      // process runs as; on POSIX mode 0600 given at creation. An existing file at
      // path is removed first, because opening one keeps whatever permissions it
      // already had. False, with a sentence in error, when the file could not be
      // written that way - a secret written where anyone can read it is worse than
      // one not written.
      static bool Write(const String &path, const AnsiString &data, String &error);

      // A directory whose contents nobody but SYSTEM, BUILTIN\Administrators and
      // the service can change: on Windows a protected DACL giving SYSTEM and the
      // Administrators full control and - when the process does not run as
      // SYSTEM - the service's own SID (NT SERVICE\<service>, which its token
      // carries whatever account it logs on as) Modify, all inherited by
      // everything made inside, on a directory that is not a link and is owned by
      // one of them. The update's installer, its rollback image and its helper
      // wait in one; anyone else who could write there could swap a file between
      // the server's check and its run.
      //
      // Made when missing, and held (HeldDirectory) from before it is checked. One
      // that is already there with other permissions - an earlier version made
      // it, and it inherited its parent's - is given these, and then EMPTIED:
      // every file in it keeps its own owner and entries, so anything planted
      // while others could write there would outlive new permissions on the
      // folder. repaired then says what it allowed before. False, with a sentence
      // in error, when it cannot be made so: a link, an owner that is not one of
      // the three, permissions the process may not set, an entry it cannot
      // delete. The check and the repair go through the held handle, never the
      // path again. On POSIX the directory is only made when missing: the Linux
      // helper stops the unit and checks what it installs again as root.
      static bool EnsureDirectory(const String &path, HeldDirectory &held, String &repaired, String &error);

      // The same permissions, for a file that must NEVER replace one: a key a verb
      // exports to a name an administrator gave. Nothing is deleted first, so a
      // file at path - made a moment before, by anyone - is left and the write
      // refused. On Windows CREATE_NEW, flushed to the disk. On POSIX the bytes go
      // to a new temporary name beside it (O_CREAT|O_EXCL|O_NOFOLLOW, mode 0600),
      // are given their mode and flushed through that descriptor, and reach path
      // through link(), which refuses a name that exists as anything - a file, a
      // directory, a symbolic link - and never follows one; nothing is done to the
      // file by its path after it is made.
      static bool WriteNew(const String &path, const AnsiString &data, String &error);

      // The last modification time of a file, in seconds since the epoch (UTC).
      // False when the file cannot be examined.
      static bool ModifiedAt(const String &path, __int64 &seconds);
   };
}
