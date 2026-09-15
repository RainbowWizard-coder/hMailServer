// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"
#include "ProtectedFile.h"

#ifdef HM_PLATFORM_POSIX

// sddl.h has no counterpart here and needs none: the rule it is used to state -
// this file is readable by the account the server runs as and by nobody else - is
// a creation mode on this platform rather than a security descriptor. What the
// POSIX branches below need instead is open(2), stat(2) and unlink(2), and
// boost::filesystem to turn the wide path the server carries into the narrow one
// those take.
#include <boost/filesystem.hpp>

#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <ctime>

#else
#include <sddl.h>
#include <boost/filesystem.hpp>
#endif

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
#ifndef HM_PLATFORM_POSIX
      // The SID of the account this process runs as, as a string, or empty when
      // it cannot be read - in which case the DACL simply does not name it, and a
      // service running as something other than SYSTEM fails to write the file
      // rather than writing one it cannot read back.
      String CurrentUserSid_()
      {
         HANDLE token = nullptr;
         if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
            return String();

         DWORD needed = 0;
         GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
         if (needed == 0)
         {
            CloseHandle(token);
            return String();
         }

         std::vector<unsigned char> buffer(needed);
         String result;
         if (GetTokenInformation(token, TokenUser, &buffer[0], needed, &needed))
         {
            LPWSTR text = nullptr;
            if (ConvertSidToStringSidW(((TOKEN_USER *) &buffer[0])->User.Sid, &text))
            {
               result = text;
               LocalFree(text);
            }
         }

         CloseHandle(token);
         return result;
      }
#endif
   }

   bool
   ProtectedFile::Write(const String &path, const AnsiString &data, String &error)
   {
#ifdef HM_PLATFORM_POSIX

      // The POSIX statement of exactly the rule the DACL below states: this file
      // is readable by the account the server runs as and by nobody else.
      //
      // 0600 is given to open(2) AT CREATION rather than applied with chmod
      // afterwards, because a file created 0644 and narrowed a moment later is
      // world-readable for that moment, and one moment is all a local user needs
      // to read a secret.
      //
      // The unlink-then-O_EXCL pair is the counterpart of the DeleteFile and
      // CREATE_NEW in the Windows branch, and is there for the same reason:
      // opening an existing file keeps whatever permissions that file already
      // had, which is precisely the case this code exists to prevent.
      const std::string native = boost::filesystem::path(std::wstring(path)).string();

      ::unlink(native.c_str());

      const int descriptor = ::open(native.c_str(), O_WRONLY | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);

      if (descriptor < 0)
      {
         error = Formatter::Format(_T("{0} could not be written (error {1})."), path, (int) errno);
         return false;
      }

      const ssize_t written = ::write(descriptor, data.c_str(), (size_t) data.GetLength());

      ::close(descriptor);

      // A short write leaves a truncated secret on disk, which nothing can use and
      // which would look like a complete one to the next reader, so the file goes
      // rather than being left behind.
      if (written != (ssize_t) data.GetLength())
      {
         ::unlink(native.c_str());
         error = Formatter::Format(_T("{0} could not be written."), path);
         return false;
      }

      return true;

#else
      // SYSTEM, the local Administrators group, and the account this process
      // runs as. The last one matters: the service does not have to be
      // LocalSystem - there is a setting for running it as a named account -
      // and a DACL naming only SYSTEM would leave the server unable to read
      // back the file it had just written.
      String sddl = _T("D:P(A;;FA;;;SY)(A;;FA;;;BA)");
      String owner = CurrentUserSid_();
      if (!owner.IsEmpty() && owner != _T("S-1-5-18"))
         sddl += Formatter::Format(_T("(A;;FA;;;{0})"), owner);

      PSECURITY_DESCRIPTOR descriptor = nullptr;
      if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
             sddl, SDDL_REVISION_1, &descriptor, nullptr))
      {
         error = Formatter::Format(_T("The permissions of {0} could not be built (error {1})."), path, (int) GetLastError());
         return false;
      }

      SECURITY_ATTRIBUTES attributes = {};
      attributes.nLength = sizeof(attributes);
      attributes.lpSecurityDescriptor = descriptor;
      attributes.bInheritHandle = FALSE;

      // CREATE_ALWAYS on an existing file keeps the file's existing DACL, so a
      // file written earlier - before this code, or by anything else - is
      // removed first and the new one created with the DACL above.
      DeleteFile(path);

      HANDLE handle = CreateFileW(path, GENERIC_WRITE, 0, &attributes, CREATE_NEW,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
      LocalFree(descriptor);

      if (handle == INVALID_HANDLE_VALUE)
      {
         error = Formatter::Format(_T("{0} could not be written (error {1})."), path, (int) GetLastError());
         return false;
      }

      DWORD written = 0;
      bool ok = WriteFile(handle, data.c_str(), (DWORD) data.GetLength(), &written, nullptr) != 0 &&
                written == (DWORD) data.GetLength();
      CloseHandle(handle);

      if (!ok)
      {
         DeleteFile(path);
         error = Formatter::Format(_T("{0} could not be written."), path);
         return false;
      }

      return true;
#endif
   }

   bool
   ProtectedFile::ModifiedAt(const String &path, __int64 &seconds)
   {
      boost::system::error_code error_code;
      const std::time_t lastWrite = boost::filesystem::last_write_time(boost::filesystem::path(std::wstring(path)), error_code);

      if (error_code)
         return false;

      seconds = (__int64) lastWrite;
      return true;
   }
}
