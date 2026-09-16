// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

namespace HM
{
   class Compression
   {
   public:
      Compression();
      ~Compression(void);

      // A passphrase, when one is given, encrypts what is added with 7-Zip's
      // AES-256 and encrypts the archive's headers with it too (-mhe=on), so that
      // not even the names of the files in it can be listed without it. It reaches
      // 7-Zip on its standard input and never on its command line, where every
      // user of the machine could read it in the process list; see LaunchCommand_.
      // An archive is encrypted from its first file or not at all, and adding to
      // an encrypted one needs its passphrase.
      bool AddDirectory(const String &zipFile, const String &directoryToAdd, const String &passphrase = String());
      bool AddFile(const String &zipFile, const String &fileToAdd, const String &passphrase = String());

      // The passphrase opens an encrypted archive; an archive that is not
      // encrypted ignores it. Without one, an encrypted archive does not open.
      //
      // TWO NAMES RATHER THAN TWO ARITIES. These were one overloaded Uncompress,
      // whose three-argument form used to take a wildcard and now takes a
      // passphrase. Nothing in the tree calls it wrongly - but a three-argument
      // call written against the old meaning still compiled, and quietly extracted
      // the whole archive instead of one member while handing the wildcard to
      // 7-Zip as a password. The compiler cannot diagnose that; separate names
      // mean it can never be written.
      bool UncompressAll(const String &zipFile, const String &targetDirectory, const String &passphrase = String());
      bool UncompressMember(const String &zipFile, const String &targetDirectory, const String &wildCard, const String &passphrase = String());

      // A ceiling on how long one archiver run may take, in milliseconds. Zero -
      // the default - is no ceiling, which is what writing a backup needs: 7za
      // legitimately runs for hours over a large message store and killing it
      // would truncate the archive.
      //
      // It exists for the callers that are NOT a backup: work done on a thread
      // something is waiting behind, where an archive on a dead network mount
      // would otherwise hold that thread for ever. It is the caller's own bound
      // and is deliberately not ExternalProcessTimeout, which an administrator
      // may set to zero to mean "wait for the virus scanner however long it
      // takes" without also meaning it about an HTTP worker thread.
      void SetMaximumRunTime(unsigned int milliseconds);

      // Whether the last run was stopped by that ceiling rather than failing on
      // its own account. The two need different sentences: one says the archive
      // could not be opened in the time allowed, the other that it could not be
      // opened at all.
      bool GetLastRunTimedOut() const;

      enum ArchiveEncryption
      {
         // Not a 7-Zip archive whose header could be read: not one at all, or one
         // cut short before the index at its end.
         EncryptionUnknown = 0,
         // A 7-Zip archive whose headers are in the clear.
         EncryptionNone = 1,
         // A 7-Zip archive whose headers - so the names of the files in it too -
         // are encrypted with AES, which is how every encrypted backup is written.
         EncryptionHeaders = 2
      };

      // Read from the archive's own header, without running 7-Zip and without the
      // passphrase: whether it can be opened without one.
      static ArchiveEncryption GetArchiveEncryption(const String &zipFile);

   private:

      bool LaunchCommand_(const String &commandLine, const String &passphrase);
      String GetExecutableFullPath_();

      unsigned int maximum_run_time_ms_;
      bool last_run_timed_out_;
   };
}