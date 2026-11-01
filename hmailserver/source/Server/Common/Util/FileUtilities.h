// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#pragma once

namespace HM
{
   class FileInfo;

   class FileUtilities
   {
   public:
      FileUtilities(void);
      ~FileUtilities(void);

      static const String PathSeparator;

      // The bytes a narrow call of the system is given for a file's name - fopen,
      // open, stat, link, a libstdc++ stream, an OpenSSL or Boost.Asio file call,
      // execvp. On POSIX, UTF-8 of the name's characters: the one encoding the
      // server names files in, the one Boost.Filesystem's paths are given too
      // (UseUtf8FileNames), and the one IniFile, DataProtector and libpq's root
      // certificate already wrote. Up to 6.3.7 most of these calls were handed
      // AnsiString(String)'s bytes instead - Latin-1, and '?' above U+00FF - so
      // that one name outside ASCII was one file to Boost and another, or none,
      // to them. Empty for a name UTF-8 cannot carry (half of a surrogate pair),
      // which no call then finds. On Windows AnsiString(String), as it has always
      // been: the narrow calls there read the ANSI code page - all but OpenSSL's,
      // which are handed OpenSslPath's bytes instead.
      static AnsiString NarrowPath(const String &path);

      // A file's name as the bytes a library that opens it with a narrow call is
      // handed, so that the file it opens is the one named: the database clients'
      // CA files (DALConnection::FileNameBytes) and OpenSSL's own file functions
      // (OpenSslPath). On POSIX, UTF-8: the bytes of the name on the file system,
      // which the server makes every file name in (NarrowPath). On Windows, the
      // ANSI code page, with no best-fit letter put in a character's place - or
      // UTF-8 when the code page is UTF-8 (the system-wide "Beta: Use Unicode
      // UTF-8" setting). OpenSSL's fopen on Windows (crypto/o_fopen.c,
      // openssl_fopen) reads a name as UTF-8 first and opens the file at that
      // reading when there is one, falling back to the code page only when there
      // is none, so a name whose bytes outside ASCII in the code page also read as
      // UTF-8 - on code page 1252 U+00C3 U+00A9 are C3 A9, which is U+00E9 - names
      // two files, and the one opened is another whenever a file is at the other
      // reading. False, with nothing written, for such a name, for one with a
      // character the code page cannot carry, for one with U+0000 (a narrow call
      // would cut it short) and for half of a surrogate pair.
      static bool FileNameBytes(const String &path, AnsiString &bytes);

      // The bytes OpenSSL's file functions are handed for a file's name -
      // BIO_new_file, SSL_load_client_CA_file, and Boost.Asio's ssl::context
      // use_certificate_file, use_certificate_chain_file, use_private_key_file,
      // use_tmp_dh_file and load_verify_file, which reach OpenSSL's fopen. On
      // Windows FileNameBytes', and false, with nothing written, for a name it
      // refuses: the caller reads nothing and accepts nothing from the file, and
      // says so (ReportOpenSslPathRefused). Up to 6.3.7 those calls were handed
      // AnsiString(String)'s bytes, and a name whose bytes also read as UTF-8 was
      // read from another file whenever one was at that reading. On POSIX
      // NarrowPath's UTF-8, which OpenSSL opens as it is, and always true.
      // build/check-openssl-file-names.py holds every such call in the server to
      // a name this filled.
      static bool OpenSslPath(const String &path, AnsiString &bytes);

      // HM7536 in the error log, for a name OpenSslPath refused: what - "The
      // certificate file of the listener on 0.0.0.0:465" - names the file, path is
      // its name, source the function reporting. once reports a name from that
      // source the first time alone, for a reader that runs again and again on
      // the same configuration (the expiry metric, the certificate alert).
      static void ReportOpenSslPathRefused(const String &what, const String &path, const String &source, bool once = false);

      // The way back, for a name the system gives as bytes: an argument of the
      // command line, /proc/self/exe, a directory of PATH. On POSIX the bytes
      // read as UTF-8, and false - with the bytes widened one to a character, so
      // that the name can at least be shown - for bytes that are not UTF-8,
      // which no name NarrowPath writes is. On Windows the bytes widened as
      // String(const char *) widens them, and true.
      static bool WidePath(const char *bytes, String &path);

#ifdef HM_PLATFORM_POSIX
      // Gives Boost.Filesystem's paths a UTF-8 conversion between a wide name and
      // the file system's bytes, in place of the locale's the process started
      // with: the "C" locale's when LANG is unset, and libstdc++'s on musl
      // whatever LANG says, both of which refuse - throw for - every character
      // outside ASCII. Up to 6.3.7 a UTF-8 LANG on glibc gave UTF-8, and any other
      // start an exception for such a name. Done once, before main, by this
      // file's own initializer, so that every program linked with the core - the
      // server and the testers - converts a name as NarrowPath does; calling it
      // again does nothing.
      static void UseUtf8FileNames();
#endif

      static String GetFilePath(const String &FileName);
      static String GetFileNameFromFullPath(const String & sFullPath);

      static bool DeleteFile(const String &FileName);

      //static bool ReadLine(HANDLE hFile, String &sLine);
      static bool Copy(const String &sFrom, const String &sTo, bool bCreateMissingDirectories = false);
      // Renames sFrom onto sTo, replacing sTo if it exists. There is deliberately no
      // overwrite flag: the rename replaces the destination as one operation and
      // there is no variant that does not, so a flag could only mislead. See the
      // definition for why the delete-then-rename version was removed.
      static bool Move(const String &sFrom, const String &sTo);
      static bool Exists(const String &sFilename);

      static void ReadFileToBuf(const String &sFilename, BYTE *Buf, int iStart = -1, int iCount = -1);
      static String ReadCompleteTextFile(const String &sFilename);

      static bool WriteToFile(const String &sFilename, const String &sData, bool bUnicode);
      static bool WriteToFile(const String &sFilename, const AnsiString &sData);

      // Writes to a temporary name beside the file and renames it into place, so
      // the file on disk is always either the old content or the new, never a
      // truncated middle - and so a file that has more than one name (a hard
      // link) is replaced under this name alone, the other names keeping the old
      // content. This is what makes one message file with N recipients' names
      // safe to share: a rewrite of one recipient's copy is a copy-on-write by
      // construction. The temporary name is unique per call.
      static bool WriteToFileAtomically(const String &sFilename, const AnsiString &sData);

      // The same, for a message whose body is not held in memory (MimeBody::
      // LoadHeaderFromFile): sHead, then the bytes of sBodySource from
      // bodyOffset up to bodyEnd, copied in chunks and never held whole.
      // sBodySource may be sFilename itself; it is closed before the rename.
      // False, sFilename as it was and nothing left beside it, when sBodySource
      // is shorter than bodyEnd, or a read, a write or the rename fails.
      static bool WriteToFileAtomically(const String &sFilename, const AnsiString &sHead, const String &sBodySource, unsigned __int64 bodyOffset, unsigned __int64 bodyEnd);

      // Copy's counterpart of WriteToFileAtomically: sFrom is copied to a temporary
      // name beside sTo and renamed over it, so sTo holds its old content or the
      // whole of the new, never a part. Copy itself, onto a file that exists,
      // empties it first and then writes it, and a disk that fills part-way leaves
      // it short with nothing to give back. False, and sTo as it was, when the
      // copy or the rename fails.
      static bool CopyOver(const String &sFrom, const String &sTo);

      // Move's counterpart of CopyOver, for a file that may be on another volume
      // than sTo: sFrom is moved to a temporary name beside sTo - a rename where it
      // can be one, a copy to that new name where it cannot, sFrom removed only once
      // the copy is whole - and renamed over sTo, in one directory and one step.
      // A move between volumes is a copy, and Move's onto an existing sTo empties
      // it first (MoveFileExW with MOVEFILE_COPY_ALLOWED), so a disk that filled
      // part-way left sTo short. False, sTo as it was and nothing left beside it,
      // when either step fails; sFrom is then still there unless the first step
      // was a rename.
      static bool MoveOver(const String &sFrom, const String &sTo);

      // The size of a file, or 0 when it cannot be read. long is 32 bits here, so a
      // size over LONG_MAX is SATURATED rather than truncated - see the definition
      // for why the previous truncation turned three "> maximum" guards into
      // guards that admitted the oversized file. Use FileSize64 where the true
      // size matters.
      static long FileSize(const String &sFileName);

      // The true 64-bit size. Returns false when the file cannot be read, which is
      // the case FileSize cannot distinguish from an empty file.
      static bool FileSize64(const String &sFileName, unsigned __int64 &size);

      static String GetTempFileName();
      static bool CreateDirectory(const String &sName);

      // Matches the precondition CopyDirectory enforces by throwing, so a caller
      // can check first rather than catch.
      static bool DirectoryExists(const String &sDirName);

      static bool CopyDirectory(String sFrom, String sTo, String &errorMessage);
      static bool DeleteDirectory(const String &sDirName, bool force);
      static bool DeleteFilesInDirectory(const String &sDirName);
      static bool DeleteDirectoriesInDirectory(const String &sDirName);

      static std::vector<FileInfo> GetFilesInDirectory(const String &sDirectoryName, const String &regularExpressionTest);
      static bool GetDirectoryContainsFileRecursive(const String &sDirectoryName);
      static bool IsUNCPath(const String &sPath);
      static bool IsValidUNCFolder(const String &sPath);
      static bool IsFullPath(const String &sPath);

      static String Combine(const String &path1, const String &path2);

   private:

      enum FileEncoding
      {
         ANSI = 1,
         UTF8 = 2,
         UTF16 = 3
      };

      
   };

   class FileUtilitiesTester
   {
   private:

   public:

      FileUtilitiesTester() {};
      virtual ~FileUtilitiesTester() {} ;

      void Test();

      // File::Close against a file whose last bytes cannot be written - /dev/full
      // on Linux, a named pipe with no reader on Windows - and WriteToFile with
      // it: the close is the only report of the tail a write left buffered. Public
      // because file-utilities-tests (platform/file_utilities_tests_main.cpp) runs
      // it on Linux, where ClassTester does not run; Test() runs it on Windows.
      void TestFailedClose();

      // A name with characters outside ASCII is one file to every call: Boost.
      // Filesystem's paths, File::Open, NarrowPath's bytes for a narrow call, and
      // the name read back from a directory's listing - on POSIX in UTF-8 whatever
      // LANG says. And the bytes OpenSslPath hands OpenSSL: NarrowPath's on
      // POSIX; on Windows a name in ASCII as it is, a name with U+00C3 U+00A9
      // refused but under the UTF-8 code page, and on code page 1252 U+00E9 alone
      // in it. Public for file-utilities-tests, as TestFailedClose is.
      void TestFileNameEncoding();
   private:

      void TestDeleteFile_();
      void TestFileSize_();
      void TestReadFileToBuf_();

      // ByteBuffer and File have no tester of their own, and adding one would mean
      // editing ClassTester::DoTests to reach it. They are the layer this class is
      // built on, so their coverage lives here.
      void TestByteBuffer_();

      // The on-disk UTF-16LE format: the codec in Unicode, and File::Write /
      // ReadCompleteTextFile on either side of it, against the bytes a Windows
      // server writes. Here for the same reason as TestByteBuffer_.
      void TestUtf16Files_();
   };
}