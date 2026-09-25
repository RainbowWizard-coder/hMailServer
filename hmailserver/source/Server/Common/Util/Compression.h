// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <iosfwd>

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

      // Exactly the named members - each a path inside the archive, as
      // "7za l" prints it - and nothing beside them, handed to the archiver as
      // a list file (@file, one member per line, UTF-8 with a byte-order mark
      // and -scsUTF-8, so that a domain or account directory named outside
      // ASCII is read as written). A member the archive does not hold is not
      // an error to the archiver and is not one here: what came out is the
      // caller's to judge. Measured with the bundled 7za 19.00, which matches
      // the names without regard to case on Windows. An empty list extracts
      // nothing and answers true.
      bool UncompressMembers(const String &zipFile, const String &targetDirectory, const std::vector<String> &members, const String &passphrase = String());

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

      enum EntryCheck
      {
         // Listed, and no entry would be written anywhere but under the directory
         // it is extracted to.
         EntriesSound = 0,
         // An entry names an absolute path, climbs out with a ".." segment, or is
         // a link; unsafeEntry names it.
         EntriesUnsafe = 1,
         // The archiver could not list the archive: not an archive, damaged, the
         // wrong passphrase, or stopped by SetMaximumRunTime (GetLastRunTimedOut
         // says which of those it was not).
         EntriesUnlisted = 2,
         // The listing had nowhere to go: the file for it could not be created in
         // the scratch directory (HM7541 says why). Nothing about the archive.
         EntriesListingNotWritten = 3
      };

      // Lists every entry of the archive ("7za l -slt", which reads only its
      // headers) and says whether extracting it could write anywhere but under
      // the target: an absolute path, a ".." segment, or a link - symbolic, hard,
      // or a Windows reparse point, which is a link or a junction - that has a
      // target to link to (FindUnsafeEntry says which do). The restore
      // asks this before it extracts anything (BackupRestorer), whatever the
      // archiver would itself do with such an entry: 7-Zip 26.03 drops the ".."
      // and the drive and checks where a link points, older ones and the p7zip
      // some Linux machines have did less, and none of that is this server's to
      // rely on.
      //
      // The listing is written to a file in scratchDirectory - the caller's own
      // temporary directory - and removed once read. The passphrase opens an
      // archive whose headers are encrypted, as it does for extracting.
      EntryCheck CheckEntries(const String &zipFile, const String &scratchDirectory, const String &passphrase, String &unsafeEntry);

      // The rule CheckEntries applies, over "7za l -slt" output: true when an
      // entry must not be extracted, with the entry - its path, and what is wrong
      // with it - in unsafeEntry. Only what follows the "----------" line is
      // entries; the archive's own path, before it, is not one. A link named
      // outright ("Symbolic Link = ", "Hard Link = ") is refused. A POSIX link
      // (a mode starting l) is refused whenever it has data or no size is listed:
      // p7zip makes a link of the data's first line, however long the data. A
      // Windows reparse point (the L attribute) is refused when it has data
      // under 16 KiB - its target - or no size at all, and not when its size is
      // 0, which is what 7za 19.00 wrote for a directory it reached through a
      // junction. And a listing that cannot be read one way only is refused:
      // p7zip 16.02 prints a name or a comment as it is, newlines and all, so a
      // property that comes twice in one entry, or a line starting "{" or a value
      // that is "{" (a value on several lines), is taken for one written into the
      // listing by the archive. Public for the in-process tester.
      static bool FindUnsafeEntry(std::istream &listing, AnsiString &unsafeEntry);

      // One path inside an archive: absolute (a leading separator, a drive letter,
      // a UNC or \\?\ prefix), or holding a segment that is ".." - or only dots
      // and spaces with two dots or more, which Windows trims down to "..".
      static bool IsUnsafeEntryPath(const AnsiString &path);

   private:

      bool LaunchCommand_(const String &commandLine, const String &passphrase, const String &outputFile = String());
      String GetExecutableFullPath_();

      unsigned int maximum_run_time_ms_;
      bool last_run_timed_out_;
      bool last_run_output_failed_;
   };

   // Compression::FindUnsafeEntry and IsUnsafeEntryPath on listings written out
   // as 7-Zip prints them: the entries a backup has, and each shape of entry a
   // restore refuses. Run by ClassTester (Utilities.RunTestSuite, and
   // --test-internals on Linux).
   class CompressionTester
   {
   public:
      void Test();
   };
}