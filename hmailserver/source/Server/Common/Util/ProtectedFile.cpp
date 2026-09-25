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

      // The service's own SID, NT SERVICE\hMailServer, in buffer: what the
      // low-privilege service account is granted by (ServiceAccount, docs/
      // ServiceAccount.md), and what its token carries whatever account the
      // service logs on as. Null when Windows does not know it - no service of
      // that name - and then the caller uses the process's own account.
      PSID ServiceSidBinary_(std::vector<unsigned char> &buffer)
      {
         DWORD sidSize = 0;
         DWORD domainSize = 0;
         SID_NAME_USE use;
         LookupAccountNameW(nullptr, L"NT SERVICE\\hMailServer", nullptr, &sidSize, nullptr, &domainSize, &use);
         if (sidSize == 0)
            return nullptr;

         buffer.resize(sidSize);
         std::vector<wchar_t> domain(domainSize + 1);
         if (!LookupAccountNameW(nullptr, L"NT SERVICE\\hMailServer", &buffer[0], &sidSize, &domain[0], &domainSize, &use))
            return nullptr;
         return (PSID) &buffer[0];
      }

      String SidText_(PSID sid)
      {
         String text;
         LPWSTR sidText = nullptr;
         if (sid != nullptr && ConvertSidToStringSidW(sid, &sidText))
         {
            text = sidText;
            LocalFree(sidText);
         }
         return text;
      }

      // SYSTEM, the local Administrators group, the account this process runs as,
      // or the service's own SID.
      bool TrustedSid_(PSID sid, PSID self, PSID service)
      {
         if (sid == nullptr || !IsValidSid(sid))
            return false;
         if (IsWellKnownSid(sid, WinLocalSystemSid) || IsWellKnownSid(sid, WinBuiltinAdministratorsSid))
            return true;
         return (self != nullptr && EqualSid(sid, self) != FALSE) || (service != nullptr && EqualSid(sid, service) != FALSE);
      }

      // "BUILTIN\Users (S-1-5-32-545)", or as much of it as can be had.
      String SidName_(PSID sid)
      {
         String text = SidText_(sid);

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

      // Modify (icacls M): read, write, execute and delete. What the service's
      // own SID is given, as ServiceAccount gives it on the data folder.
      const ACCESS_MASK MODIFY_RIGHTS_ = 0x001301BF;

      enum Verdict_
      {
         VerdictSafe_,
         VerdictOwner_,
         VerdictEntries_
      };

      // The directory held by handle: VerdictSafe_ when it is owned by one of the
      // trusted, nobody else can change it or anything made in it (inherit-only
      // entries count, since they are what a new file gets), its list is its own,
      // and - where service is given - the service's SID has Modify, inherited
      // by files and folders. VerdictOwner_ or VerdictEntries_ with why, a
      // clause, otherwise.
      Verdict_ Inspect_(HANDLE handle, PSID self, PSID service, String &why)
      {
         PSID owner = nullptr;
         PACL dacl = nullptr;
         PSECURITY_DESCRIPTOR descriptor = nullptr;
         const DWORD result = GetSecurityInfo(handle, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                              &owner, nullptr, &dacl, nullptr, &descriptor);
         if (result != ERROR_SUCCESS)
         {
            why = Formatter::Format(_T("its permissions could not be read (Windows error {0})"), (int) result);
            return VerdictEntries_;
         }

         Verdict_ verdict = VerdictSafe_;
         bool serviceMayWrite = service == nullptr;
         if (!TrustedSid_(owner, self, service))
         {
            why = Formatter::Format(_T("it is owned by {0}, who can change its permissions whatever they say"), SidName_(owner));
            verdict = VerdictOwner_;
         }
         else if (dacl == nullptr)
         {
            why = _T("it has no access list at all, which lets everyone change it");
            verdict = VerdictEntries_;
         }

         for (DWORD i = 0; verdict == VerdictSafe_ && i < dacl->AceCount; i++)
         {
            void *ace = nullptr;
            if (!GetAce(dacl, i, &ace) || ace == nullptr)
            {
               why = _T("an entry of its access list could not be read");
               verdict = VerdictEntries_;
               break;
            }

            const ACE_HEADER *header = (const ACE_HEADER *) ace;
            switch (header->AceType)
            {
            case ACCESS_ALLOWED_ACE_TYPE:
            {
               const ACCESS_ALLOWED_ACE *allowed = (const ACCESS_ALLOWED_ACE *) ace;
               PSID sid = (PSID) &allowed->SidStart;
               if (!TrustedSid_(sid, self, service) && (allowed->Mask & CHANGING_RIGHTS_) != 0)
               {
                  why = Formatter::Format(_T("it lets {0} change what is in it"), SidName_(sid));
                  verdict = VerdictEntries_;
               }
               const BYTE inherited = OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE;
               if (service != nullptr && EqualSid(sid, service) &&
                   ((allowed->Mask & MODIFY_RIGHTS_) == MODIFY_RIGHTS_ || (allowed->Mask & GENERIC_ALL) != 0) &&
                   (header->AceFlags & inherited) == inherited && (header->AceFlags & INHERIT_ONLY_ACE) == 0)
                  serviceMayWrite = true;
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
               verdict = VerdictEntries_;
               break;
            default:
               // Denials, and entries that grant nothing.
               break;
            }
         }

         // The service must be able to write there itself: a list with SYSTEM and
         // the Administrators alone is safe, and useless to a service that runs as
         // NT SERVICE\hMailServer.
         if (verdict == VerdictSafe_ && !serviceMayWrite)
         {
            why = Formatter::Format(_T("it does not give the service's own account, {0}, Modify on what is in it"), SidName_(service));
            verdict = VerdictEntries_;
         }

         // And a list of its own: one that takes its parent's entries gains
         // whatever is granted on the folder above it, later, with nothing here
         // changing.
         SECURITY_DESCRIPTOR_CONTROL control = 0;
         DWORD revision = 0;
         if (verdict == VerdictSafe_ && (!GetSecurityDescriptorControl(descriptor, &control, &revision) || (control & SE_DACL_PROTECTED) == 0))
         {
            why = _T("it takes its permissions from the folder above it, which anyone allowed to change that folder's can widen");
            verdict = VerdictEntries_;
         }

         LocalFree(descriptor);
         return verdict;
      }

      // The path Windows resolved an open handle to, \\?\ prefix and all, which
      // every call below takes; empty when it cannot say.
      std::wstring HeldPath_(HANDLE handle)
      {
         std::vector<wchar_t> buffer(32768);
         const DWORD length = GetFinalPathNameByHandleW(handle, &buffer[0], (DWORD) buffer.size(), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
         if (length == 0 || length >= buffer.size())
            return std::wstring();
         return std::wstring(&buffer[0], length);
      }

      // Whether an open handle is the folder path names, and not one a link in a
      // folder ABOVE it led to. Opening with FILE_FLAG_OPEN_REPARSE_POINT holds the
      // last part of the path as itself, and the held handle stops anything above
      // it from being swapped from then on - but a swap made before the open still
      // counts: where the data folder sits under one that others may change, the
      // folder opened could be some other folder of the same name, whose list a
      // repair would then rewrite and whose contents it would delete. The path
      // Windows resolved the handle to is compared with the path as named (made
      // full and long, so that a relative or 8.3 spelling is not a difference),
      // ignoring case.
      bool IsHeldAt_(HANDLE handle, const String &path, std::wstring &resolved)
      {
         resolved = HeldPath_(handle);
         if (resolved.empty())
            return false;

         std::wstring held = resolved;
         if (held.compare(0, 8, L"\\\\?\\UNC\\") == 0)
            held = L"\\\\" + held.substr(8);
         else if (held.compare(0, 4, L"\\\\?\\") == 0)
            held = held.substr(4);

         std::vector<wchar_t> full(32768);
         const DWORD fullLength = GetFullPathNameW(path.c_str(), (DWORD) full.size(), &full[0], nullptr);
         if (fullLength == 0 || fullLength >= full.size())
            return false;
         std::wstring named(&full[0], fullLength);

         std::vector<wchar_t> longer(32768);
         const DWORD longLength = GetLongPathNameW(named.c_str(), &longer[0], (DWORD) longer.size());
         if (longLength > 0 && longLength < longer.size())
            named.assign(&longer[0], longLength);

         while (named.size() > 3 && (named.back() == L'\\' || named.back() == L'/'))
            named.pop_back();
         while (held.size() > 3 && held.back() == L'\\')
            held.pop_back();

         return CompareStringOrdinal(held.c_str(), (int) held.size(), named.c_str(), (int) named.size(), TRUE) == CSTR_EQUAL;
      }

      // How deep DeleteTree_ goes before it gives up: the Updates folder holds a
      // rollback folder and files, and anything far deeper was planted.
      const int DELETE_DEPTH_ = 16;

      bool EmptyHeld_(HANDLE directory, int depth, String &why);

      // Deletes what path names - a file, a link, or a folder with everything in
      // it - without following a link anywhere. Each entry is opened as itself
      // (FILE_FLAG_OPEN_REPARSE_POINT) and deleted through that handle; a folder
      // is emptied while it is held, so nothing in it can be swapped for a link
      // meanwhile, and a link is deleted, never entered.
      bool DeleteTree_(const std::wstring &path, int depth, String &why)
      {
         HANDLE handle = CreateFileW(path.c_str(), DELETE | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | SYNCHRONIZE,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                     FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
         // An entry's own list can deny writing its attributes to everybody,
         // SYSTEM included, while the folder's list still lets it be deleted
         // (FILE_DELETE_CHILD): without the attributes, so that one planted entry
         // cannot refuse the folder for good. Only a read-only one then stays.
         if (handle == INVALID_HANDLE_VALUE && GetLastError() == ERROR_ACCESS_DENIED)
            handle = CreateFileW(path.c_str(), DELETE | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                 FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
         if (handle == INVALID_HANDLE_VALUE)
         {
            why = Formatter::Format(_T("{0} could not be opened to be deleted (Windows error {1})"), String(path.c_str()), (int) GetLastError());
            return false;
         }

         bool ok = true;
         FILE_ATTRIBUTE_TAG_INFO tag = {};
         if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tag, sizeof(tag)))
         {
            why = Formatter::Format(_T("{0} could not be examined (Windows error {1})"), String(path.c_str()), (int) GetLastError());
            ok = false;
         }
         else if ((tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 && (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0)
         {
            if (depth >= DELETE_DEPTH_)
            {
               why = Formatter::Format(_T("{0} is nested deeper than anything the server makes"), String(path.c_str()));
               ok = false;
            }
            else
               ok = EmptyHeld_(handle, depth + 1, why);
         }

         // A read-only file refuses to be deleted until the attribute is gone.
         if (ok && (tag.FileAttributes & FILE_ATTRIBUTE_READONLY) != 0)
         {
            FILE_BASIC_INFO basic = {};
            if (GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic)))
            {
               basic.FileAttributes &= ~FILE_ATTRIBUTE_READONLY;
               if (basic.FileAttributes == 0)
                  basic.FileAttributes = FILE_ATTRIBUTE_NORMAL;
               SetFileInformationByHandle(handle, FileBasicInfo, &basic, sizeof(basic));
            }
         }

         if (ok)
         {
            // Its one member is named DeleteFile, which windows.h also defines as a
            // macro; set by position rather than by that name.
            FILE_DISPOSITION_INFO disposition = { TRUE };
            if (!SetFileInformationByHandle(handle, FileDispositionInfo, &disposition, sizeof(disposition)))
            {
               why = Formatter::Format(_T("{0} could not be deleted (Windows error {1})"), String(path.c_str()), (int) GetLastError());
               ok = false;
            }
         }

         CloseHandle(handle);
         return ok;
      }

      // Deletes everything in the folder held by directory, by the path Windows
      // resolved the handle to: the folder cannot be renamed while it is held,
      // nor any folder above it, so that path is the folder.
      bool EmptyHeld_(HANDLE directory, int depth, String &why)
      {
         const std::wstring path = HeldPath_(directory);
         if (path.empty())
         {
            why = Formatter::Format(_T("Windows could not say where a folder to be emptied is (error {0})"), (int) GetLastError());
            return false;
         }

         std::vector<std::wstring> names;
         WIN32_FIND_DATAW entry;
         HANDLE find = FindFirstFileExW((path + L"\\*").c_str(), FindExInfoBasic, &entry, FindExSearchNameMatch, nullptr, 0);
         if (find == INVALID_HANDLE_VALUE)
         {
            why = Formatter::Format(_T("{0} could not be listed (Windows error {1})"), String(path.c_str()), (int) GetLastError());
            return false;
         }
         do
         {
            const std::wstring name = entry.cFileName;
            if (name != L"." && name != L"..")
               names.push_back(name);
         } while (FindNextFileW(find, &entry));
         FindClose(find);

         for (size_t i = 0; i < names.size(); i++)
         {
            if (!DeleteTree_(path + L"\\" + names[i], depth, why))
               return false;
         }
         return true;
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

   HeldDirectory::HeldDirectory() :
      handle_(nullptr)
   {
   }

   HeldDirectory::~HeldDirectory()
   {
      Release();
   }

   void
   HeldDirectory::Release()
   {
#ifdef _WIN32
      if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE)
         CloseHandle((HANDLE) handle_);
#endif
      handle_ = nullptr;
   }

   bool
   ProtectedFile::EnsureDirectory(const String &path, HeldDirectory &held, String &repaired, String &error)
   {
      held.Release();
      repaired.Empty();

#ifndef _WIN32
      if (FileUtilities::DirectoryExists(path) || FileUtilities::CreateDirectory(path))
         return true;
      error = Formatter::Format(_T("{0} could not be created."), path);
      return false;
#else
      std::vector<unsigned char> selfBuffer;
      PSID self = CurrentUserSidBinary_(selfBuffer);

      // The service's own SID, NT SERVICE\hMailServer, whenever Windows knows it -
      // under SYSTEM too: a server that downloaded once as LocalSystem and was
      // then moved to the virtual account (ServiceAccount, /GrantServiceAccess)
      // must still be able to write here, and this folder takes nothing from the
      // data folder's grant. Where Windows does not know the name, the account
      // this runs as when that is not SYSTEM.
      std::vector<unsigned char> serviceBuffer;
      PSID service = ServiceSidBinary_(serviceBuffer);
      if (service == nullptr && (self == nullptr || !IsWellKnownSid(self, WinLocalSystemSid)))
         service = self;

      // SYSTEM and the Administrators, full control; the service, Modify; all of
      // it inherited by files (OI) and folders (CI) made inside, and protected (P)
      // from whatever the parent grants.
      String sddl = _T("D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)");
      if (service != nullptr)
         sddl += Formatter::Format(_T("(A;OICI;0x1301bf;;;{0})"), SidText_(service));

      PSECURITY_DESCRIPTOR descriptor = nullptr;
      if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr))
      {
         error = Formatter::Format(_T("The permissions of {0} could not be built (Windows error {1})."), path, (int) GetLastError());
         return false;
      }

      // Made with those permissions from its first moment, when it is missing;
      // whatever is there already - a folder, a file, a link - is judged below
      // through the handle, never by the path again.
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

      // Held from here: the link itself when it is one (OPEN_REPARSE_POINT), and
      // shared for reading and writing but not deleting, so that neither it nor a
      // folder above it can be renamed or swapped while it is held. Listing is
      // asked for so that the share mode counts; WRITE_DAC where the process may
      // have it, which a repair needs.
      const DWORD access = FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES | READ_CONTROL | SYNCHRONIZE;
      const DWORD flags = FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT;
      bool mayRepair = true;
      HANDLE handle = CreateFileW(path.c_str(), access | WRITE_DAC, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, flags, nullptr);
      if (handle == INVALID_HANDLE_VALUE && GetLastError() == ERROR_ACCESS_DENIED)
      {
         mayRepair = false;
         handle = CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, flags, nullptr);
      }
      if (handle == INVALID_HANDLE_VALUE)
      {
         const DWORD code = GetLastError();
         LocalFree(descriptor);
         error = Formatter::Format(_T("{0} could not be opened to be checked (Windows error {1}). Delete the folder, and the server makes it again."), path, (int) code);
         return false;
      }
      held.handle_ = handle;

      FILE_ATTRIBUTE_TAG_INFO tag = {};
      if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tag, sizeof(tag)))
      {
         const DWORD code = GetLastError();
         held.Release();
         LocalFree(descriptor);
         error = Formatter::Format(_T("{0} could not be examined (Windows error {1})."), path, (int) code);
         return false;
      }

      // A junction or a symbolic link: whoever made it can point it somewhere else
      // at any moment, whatever the permissions of where it points now.
      if ((tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
      {
         held.Release();
         LocalFree(descriptor);
         error = Formatter::Format(_T("{0} is a link to another folder, which whoever made it could point anywhere between the server's check and its run. Delete the link, and the server makes the folder itself."), path);
         return false;
      }
      if ((tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
      {
         held.Release();
         LocalFree(descriptor);
         error = Formatter::Format(_T("{0} is not a folder the server can use."), path);
         return false;
      }

      // And the folder the path names, not one a link above it led to (IsHeldAt_):
      // nothing is judged, repaired or emptied until that is known.
      std::wstring resolved;
      if (!IsHeldAt_(handle, path, resolved))
      {
         held.Release();
         LocalFree(descriptor);
         error = Formatter::Format(_T("{0} was opened as {1}: a folder above it is a link, or was changed while it was opened, and whoever may change that folder could point it anywhere. Give the data folder's real path, where nobody but the administrators may change the folders above it."),
            path, resolved.empty() ? String(_T("a path Windows could not name")) : String(resolved.c_str()));
         return false;
      }

      String why;
      const Verdict_ verdict = Inspect_(handle, self, service, why);
      if (verdict == VerdictOwner_)
      {
         // Its owner may rewrite whatever the list says, and may have left
         // anything inside: nothing in it is taken over and kept.
         held.Release();
         LocalFree(descriptor);
         error = Formatter::Format(_T("{0} cannot be trusted: {1}. An installer kept there could be replaced between the server's check and its run. Delete the folder, and the server makes it again."),
            path, why);
         return false;
      }

      if (verdict == VerdictEntries_)
      {
         // The list first, which shuts everybody else out of the folder; then
         // everything in it, since each file there keeps its own owner and its
         // own entries, and one planted while others could write here - a DLL
         // beside the helper, an installer beside the real one - would outlive
         // the folder's new list. Anything that cannot be deleted refuses the
         // folder.
         BOOL present = FALSE;
         BOOL defaulted = FALSE;
         PACL dacl = nullptr;
         GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted);

         DWORD result = ERROR_ACCESS_DENIED;
         if (mayRepair)
            result = SetSecurityInfo(handle, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, dacl, nullptr);

         String still;
         String emptied;
         if (result != ERROR_SUCCESS)
            still = Formatter::Format(_T("its permissions could not be set (Windows error {0})"), (int) result);
         else if (!EmptyHeld_(handle, 0, emptied))
            still = emptied;
         else if (Inspect_(handle, self, service, still) == VerdictSafe_)
            still.Empty();
         if (!still.IsEmpty())
         {
            held.Release();
            LocalFree(descriptor);
            error = Formatter::Format(_T("{0} could be changed by others - {1} - and could not be made safe: {2}. An installer kept there could be replaced between the server's check and its run. Delete the folder, and the server makes it again."),
               path, why, still);
            return false;
         }

         repaired = why + _T("; everything that was in it has been deleted");
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
