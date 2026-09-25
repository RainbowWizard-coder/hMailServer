// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

namespace HM
{
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
      // the account the process runs as can change: on Windows a protected DACL
      // naming those three, inherited by everything made inside, on a directory
      // that is not a link and is owned by one of them. The update's installer, its
      // rollback image and its helper wait in one; anyone else who could write
      // there could swap a file between the server's check and its run.
      //
      // Made when missing. One that is already there with other permissions - an
      // earlier version made it, and it inherited its parent's - is given these,
      // and repaired then says what it allowed before. False, with a sentence in
      // error, when it cannot be made so: a link, an owner the server cannot
      // replace, permissions it cannot set. On POSIX the directory is only made
      // when missing: the Linux helper stops the unit and checks what it installs
      // again as root.
      static bool EnsureDirectory(const String &path, String &repaired, String &error);

      // The last modification time of a file, in seconds since the epoch (UTC).
      // False when the file cannot be examined.
      static bool ModifiedAt(const String &path, __int64 &seconds);
   };
}
