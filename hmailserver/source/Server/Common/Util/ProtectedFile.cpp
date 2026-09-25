// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"
#include "ProtectedFile.h"
#include "FileUtilities.h"

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

#ifdef _WIN32
#include <aclapi.h>
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

#ifdef _WIN32
      // The same account as a SID, in buffer, or null when it cannot be read.
      PSID CurrentUserSidBinary_(std::vector<unsigned char> &buffer)
      {
         HANDLE token = nullptr;
         if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
            return nullptr;

         PSID sid = nullptr;
         DWORD needed = 0;
         GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
         if (needed > 0)
         {
            buffer.resize(needed);
            if (GetTokenInformation(token, TokenUser, &buffer[0], needed, &needed))
               sid = ((TOKEN_USER *) &buffer[0])->User.Sid;
         }

         CloseHandle(token);
         return sid;
      }

      // SYSTEM, the local Administrators group, or the account this process runs as.
      bool TrustedSid_(PSID sid, PSID self)
      {
         if (sid == nullptr || !IsValidSid(sid))
            return false;
         if (IsWellKnownSid(sid, WinLocalSystemSid) || IsWellKnownSid(sid, WinBuiltinAdministratorsSid))
            return true;
         return self != nullptr && EqualSid(sid, self) != FALSE;
      }

      // "BUILTIN\Users (S-1-5-32-545)", or as much of it as can be had.
      String SidName_(PSID sid)
      {
         String text;
         LPWSTR sidText = nullptr;
         if (sid != nullptr && ConvertSidToStringSidW(sid, &sidText))
         {
            text = sidText;
            LocalFree(sidText);
         }

         wchar_t name[256];
         wchar_t domain[256];
         DWORD nameLength = 256;
         DWORD domainLength = 256;
         SID_NAME_USE use;
         if (sid == nullptr || !LookupAccountSidW(nullptr, sid, name, &nameLength, domain, &domainLength, &use))
            return text.IsEmpty() ? String(_T("an unknown account")) : text;

         String account = domain[0] ? String(domain) + _T("\\") + String(name) : String(name);
         return text.IsEmpty() ? account : account + _T(" (") + text + _T(")");
      }

      // Every right that changes the directory, what is in it or who may: an
      // entry for anyone else that carries one of these lets that account swap a
      // file. Reading, listing and traversing are not among them.
      const ACCESS_MASK CHANGING_RIGHTS_ = FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | FILE_DELETE_CHILD |
                                           FILE_WRITE_ATTRIBUTES | DELETE | WRITE_DAC | WRITE_OWNER | GENERIC_WRITE | GENERIC_ALL;

      // True when nobody but SYSTEM, the Administrators and self can change the
      // directory or anything made in it - inherit-only entries count, since they
      // are what a new file gets. False with why, a clause, otherwise.
      bool OnlyTrustedMayChange_(const String &path, PSID self, String &why)
      {
         PSID owner = nullptr;
         PACL dacl = nullptr;
         PSECURITY_DESCRIPTOR descriptor = nullptr;
         const DWORD result = GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                                    &owner, nullptr, &dacl, nullptr, &descriptor);
         if (result != ERROR_SUCCESS)
         {
            why = Formatter::Format(_T("its permissions could not be read (Windows error {0})"), (int) result);
            return false;
         }

         bool only = true;
         if (!TrustedSid_(owner, self))
         {
            why = Formatter::Format(_T("it is owned by {0}, who can change its permissions"), SidName_(owner));
            only = false;
         }
         else if (dacl == nullptr)
         {
            why = _T("it has no access list at all, which lets everyone change it");
            only = false;
         }

         for (DWORD i = 0; only && i < dacl->AceCount; i++)
         {
            void *ace = nullptr;
            if (!GetAce(dacl, i, &ace) || ace == nullptr)
            {
               why = _T("an entry of its access list could not be read");
               only = false;
               break;
            }

            const ACE_HEADER *header = (const ACE_HEADER *) ace;
            switch (header->AceType)
            {
            case ACCESS_ALLOWED_ACE_TYPE:
            {
               const ACCESS_ALLOWED_ACE *allowed = (const ACCESS_ALLOWED_ACE *) ace;
               PSID sid = (PSID) &allowed->SidStart;
               if (!TrustedSid_(sid, self) && (allowed->Mask & CHANGING_RIGHTS_) != 0)
               {
                  why = Formatter::Format(_T("it lets {0} change what is in it"), SidName_(sid));
                  only = false;
               }
               break;
            }
            case ACCESS_ALLOWED_COMPOUND_ACE_TYPE:
            case ACCESS_ALLOWED_OBJECT_ACE_TYPE:
            case ACCESS_ALLOWED_CALLBACK_ACE_TYPE:
            case ACCESS_ALLOWED_CALLBACK_OBJECT_ACE_TYPE:
               // Grants of a shape a file system does not use, whose account and
               // rights are not where the plain kind keeps them; not read, so not
               // trusted either.
               why = _T("its access list grants rights in a form the server does not read");
               only = false;
               break;
            default:
               // Denials, and entries that grant nothing.
               break;
            }
         }

         // And a list of its own: one that takes its parent's entries gains
         // whatever is granted on the folder above it, later, with nothing here
         // changing.
         SECURITY_DESCRIPTOR_CONTROL control = 0;
         DWORD revision = 0;
         if (only && (!GetSecurityDescriptorControl(descriptor, &control, &revision) || (control & SE_DACL_PROTECTED) == 0))
         {
            why = _T("it takes its permissions from the folder above it, which anyone allowed to change that folder's can widen");
            only = false;
         }

         LocalFree(descriptor);
         return only;
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
   ProtectedFile::EnsureDirectory(const String &path, String &repaired, String &error)
   {
      repaired.Empty();

#ifndef _WIN32
      if (FileUtilities::DirectoryExists(path) || FileUtilities::CreateDirectory(path))
         return true;
      error = Formatter::Format(_T("{0} could not be created."), path);
      return false;
#else
      std::vector<unsigned char> selfBuffer;
      PSID self = CurrentUserSidBinary_(selfBuffer);

      // Write's three, inherited by files (OI) and folders (CI) made inside, and
      // protected (P) from whatever the parent grants.
      String sddl = _T("D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)");
      String owner = CurrentUserSid_();
      if (!owner.IsEmpty() && owner != _T("S-1-5-18"))
         sddl += Formatter::Format(_T("(A;OICI;FA;;;{0})"), owner);

      PSECURITY_DESCRIPTOR descriptor = nullptr;
      if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr))
      {
         error = Formatter::Format(_T("The permissions of {0} could not be built (Windows error {1})."), path, (int) GetLastError());
         return false;
      }

      DWORD attributes = GetFileAttributesW(path.c_str());
      if (attributes == INVALID_FILE_ATTRIBUTES)
      {
         SECURITY_ATTRIBUTES security = {};
         security.nLength = sizeof(security);
         security.lpSecurityDescriptor = descriptor;
         security.bInheritHandle = FALSE;
         if (!CreateDirectoryW(path.c_str(), &security))
         {
            const DWORD code = GetLastError();
            if (code != ERROR_ALREADY_EXISTS)
            {
               LocalFree(descriptor);
               error = Formatter::Format(_T("{0} could not be created (Windows error {1})."), path, (int) code);
               return false;
            }
         }
         attributes = GetFileAttributesW(path.c_str());
      }

      if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
      {
         LocalFree(descriptor);
         error = Formatter::Format(_T("{0} is not a folder the server can use."), path);
         return false;
      }

      // A junction or a symbolic link: whoever made it can point it somewhere else
      // at any moment, whatever the permissions of where it points now.
      if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
      {
         LocalFree(descriptor);
         error = Formatter::Format(_T("{0} is a link to another folder, which whoever made it could point anywhere between the server's check and its run. Delete the link, and the server makes the folder itself."), path);
         return false;
      }

      String why;
      if (!OnlyTrustedMayChange_(path, self, why))
      {
         // The DACL first, which needs WRITE_DAC; then, where the owner is not one
         // of the three, the owner - an owner can always rewrite the DACL, so one
         // left in place could put its own entry straight back. SetNamedSecurityInfo
         // carries the new inheritable entries down to what is already inside.
         BOOL present = FALSE;
         BOOL defaulted = FALSE;
         PACL dacl = nullptr;
         GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted);

         std::vector<wchar_t> name(path.c_str(), path.c_str() + path.GetLength());
         name.push_back(L'\0');
         DWORD result = SetNamedSecurityInfoW(&name[0], SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                              nullptr, nullptr, dacl, nullptr);
         if (result == ERROR_SUCCESS && self != nullptr)
         {
            PSID currentOwner = nullptr;
            PSECURITY_DESCRIPTOR current = nullptr;
            if (GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &currentOwner, nullptr, nullptr, nullptr, &current) == ERROR_SUCCESS)
            {
               if (!TrustedSid_(currentOwner, self))
                  result = SetNamedSecurityInfoW(&name[0], SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, self, nullptr, nullptr, nullptr);
               LocalFree(current);
            }
         }

         String still;
         if (!OnlyTrustedMayChange_(path, self, still))
         {
            LocalFree(descriptor);
            error = Formatter::Format(_T("{0} could be changed by others - {1} - and its permissions could not be set to SYSTEM and the Administrators alone (Windows error {2}). An installer kept there could be replaced between the server's check and its run. Delete the folder, and the server makes it again."),
               path, still, (int) result);
            return false;
         }

         repaired = why;
      }

      LocalFree(descriptor);
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
