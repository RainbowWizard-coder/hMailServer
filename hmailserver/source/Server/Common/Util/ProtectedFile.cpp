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
#include <winternl.h>
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

      // A new file at path with the DACL: SYSTEM, the local Administrators group,
      // and the account this process runs as. The last one matters: the service
      // does not have to be LocalSystem - there is a setting for running it as a
      // named account - and a DACL naming only SYSTEM would leave the server unable
      // to read back the file it had just written. CREATE_NEW: a file that exists is
      // refused. flush: FlushFileBuffers before the handle is closed.
      bool CreateNew_(const String &path, const AnsiString &data, bool flush, String &error)
      {
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

         HANDLE handle = CreateFileW(path, GENERIC_WRITE, 0, &attributes, CREATE_NEW,
                                     FILE_ATTRIBUTE_NORMAL, nullptr);
         const DWORD createError = GetLastError();
         LocalFree(descriptor);

         if (handle == INVALID_HANDLE_VALUE)
         {
            error = createError == ERROR_FILE_EXISTS
               ? Formatter::Format(_T("{0} already exists, and nothing is written over it."), path)
               : Formatter::Format(_T("{0} could not be written (error {1})."), path, (int) createError);
            return false;
         }

         DWORD written = 0;
         bool ok = WriteFile(handle, data.c_str(), (DWORD) data.GetLength(), &written, nullptr) != 0 &&
                   written == (DWORD) data.GetLength();
         if (ok && flush)
            ok = FlushFileBuffers(handle) != 0;
         CloseHandle(handle);

         if (!ok)
         {
            // The file this call made, and nobody else's: CREATE_NEW succeeded.
            DeleteFile(path);
            error = Formatter::Format(_T("{0} could not be written."), path);
            return false;
         }

         return true;
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
      // CREATE_ALWAYS on an existing file keeps the file's existing DACL, so a
      // file written earlier - before this code, or by anything else - is
      // removed first and the new one created with the DACL CreateNew_ gives it.
      DeleteFile(path);

      return CreateNew_(path, data, false, error);
#endif
   }

   bool
   ProtectedFile::WriteNew(const String &path, const AnsiString &data, String &error)
   {
#ifdef HM_PLATFORM_POSIX
      const std::string native = boost::filesystem::path(std::wstring(path)).string();

      // A temporary name beside the file. O_EXCL with O_CREAT never follows a
      // symbolic link and never opens a name that exists, so one left there by
      // anybody is not written through; another name is tried instead.
      std::string temporary;
      int descriptor = -1;
      int openError = 0;
      for (int attempt = 0; attempt < 16 && descriptor < 0; attempt++)
      {
         temporary = native + ".writing-" + std::to_string((long long) ::getpid()) + "-" +
                     std::to_string((long long) ::time(nullptr)) + "-" + std::to_string((long long) attempt);
         descriptor = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, S_IRUSR | S_IWUSR);
         openError = errno;
         if (descriptor < 0 && openError != EEXIST)
            break;
      }

      if (descriptor < 0)
      {
         error = Formatter::Format(_T("{0} could not be written: a file beside it could not be made (error {1})."), path, openError);
         return false;
      }

      // Everything through the descriptor: the bytes, the mode - the umask can only
      // have narrowed 0600, and this says it outright - and the flush.
      bool ok = true;
      size_t written = 0;
      const size_t length = (size_t) data.GetLength();
      while (ok && written < length)
      {
         const ssize_t count = ::write(descriptor, data.c_str() + written, length - written);
         if (count < 0 && errno == EINTR)
            continue;
         if (count <= 0)
            ok = false;
         else
            written += (size_t) count;
      }

      ok = ok && ::fchmod(descriptor, S_IRUSR | S_IWUSR) == 0 && ::fsync(descriptor) == 0;
      ::close(descriptor);

      if (!ok)
      {
         ::unlink(temporary.c_str());
         error = Formatter::Format(_T("{0} could not be written."), path);
         return false;
      }

      // link() gives the file its name only where the name is free: it replaces
      // nothing and follows nothing, where rename() would replace what is there.
      if (::link(temporary.c_str(), native.c_str()) != 0)
      {
         const int linkError = errno;
         ::unlink(temporary.c_str());
         error = linkError == EEXIST
            ? Formatter::Format(_T("{0} already exists, and nothing is written over it."), path)
            : Formatter::Format(_T("{0} could not be written (error {1})."), path, linkError);
         return false;
      }

      ::unlink(temporary.c_str());

      // The name flushed as well as the bytes.
      std::string directory = boost::filesystem::path(native).parent_path().string();
      if (directory.empty())
         directory = ".";
      const int directoryDescriptor = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
      if (directoryDescriptor >= 0)
      {
         ::fsync(directoryDescriptor);
         ::close(directoryDescriptor);
      }

      return true;
#else
      // No delete first: a file at path is left as it is, and CREATE_NEW refuses it.
      return CreateNew_(path, data, true, error);
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

#ifndef HM_PLATFORM_POSIX
   namespace
   {
      // NtCreateFile and the conversion of its status, from ntdll by name, so that
      // nothing has to link ntdll.lib for them.
      typedef LONG (NTAPI *NtCreateFileFunction_)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, PLARGE_INTEGER,
                                                  ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
      typedef ULONG (NTAPI *StatusToErrorFunction_)(LONG);

      const LONG StatusObjectNameNotFound_ = (LONG) 0xC0000034;

      NtCreateFileFunction_ NtCreateFileFunction()
      {
         static NtCreateFileFunction_ function = (NtCreateFileFunction_) GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtCreateFile");
         return function;
      }

      DWORD WindowsErrorOf_(LONG status)
      {
         static StatusToErrorFunction_ convert = (StatusToErrorFunction_) GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlNtStatusToDosError");
         return convert != nullptr ? (DWORD) convert(status) : (DWORD) status;
      }

      // name - one component, no separator - opened relative to the folder
      // parent, and never through a link of that name (FILE_OPEN_REPARSE_POINT):
      // a link is opened itself. The parent is not looked up by its path again,
      // so nothing done to that path since it was opened changes where this goes.
      LONG OpenBelow_(HANDLE parent, const String &name, ACCESS_MASK access, ULONG share, ULONG disposition, ULONG options,
                      PSECURITY_DESCRIPTOR descriptor, HANDLE &opened)
      {
         opened = INVALID_HANDLE_VALUE;
         NtCreateFileFunction_ create = NtCreateFileFunction();
         if (create == nullptr)
            return (LONG) 0xC0000002; // STATUS_NOT_IMPLEMENTED

         UNICODE_STRING unicode;
         unicode.Buffer = const_cast<PWSTR>(name.c_str());
         unicode.Length = (USHORT) (name.GetLength() * sizeof(wchar_t));
         unicode.MaximumLength = unicode.Length;

         OBJECT_ATTRIBUTES attributes;
         InitializeObjectAttributes(&attributes, &unicode, OBJ_CASE_INSENSITIVE, parent, descriptor);

         IO_STATUS_BLOCK status = {};
         HANDLE handle = nullptr;
         const LONG result = create(&handle, access | SYNCHRONIZE, &attributes, &status, nullptr, FILE_ATTRIBUTE_NORMAL, share,
                                    disposition, options | FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_REPARSE_POINT, nullptr, 0);
         if (result >= 0)
            opened = handle;
         return result;
      }

      // A folder name - one component - opened relative to the folder parent and
      // THROUGH it, a link of that name followed: only for a link
      // OnlyAdministratorsControl has found the administrators' alone, and only
      // to be held on from where it leads (HeldFolder::HoldThrough_).
      LONG OpenThrough_(HANDLE parent, const String &name, ACCESS_MASK access, ULONG share, HANDLE &opened)
      {
         opened = INVALID_HANDLE_VALUE;
         NtCreateFileFunction_ create = NtCreateFileFunction();
         if (create == nullptr)
            return (LONG) 0xC0000002; // STATUS_NOT_IMPLEMENTED

         UNICODE_STRING unicode;
         unicode.Buffer = const_cast<PWSTR>(name.c_str());
         unicode.Length = (USHORT) (name.GetLength() * sizeof(wchar_t));
         unicode.MaximumLength = unicode.Length;

         OBJECT_ATTRIBUTES attributes;
         InitializeObjectAttributes(&attributes, &unicode, OBJ_CASE_INSENSITIVE, parent, nullptr);

         IO_STATUS_BLOCK status = {};
         HANDLE handle = nullptr;
         const LONG result = create(&handle, access | SYNCHRONIZE, &attributes, &status, nullptr, FILE_ATTRIBUTE_NORMAL, share,
                                    FILE_OPEN, FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT, nullptr, 0);
         if (result >= 0)
            opened = handle;
         return result;
      }

      // Whether what the handle holds is a reparse point - a junction, a symbolic
      // link, a mount point or anything else Windows redirects through - or could
      // not be told.
      bool IsReparsePoint_(HANDLE handle)
      {
         FILE_ATTRIBUTE_TAG_INFO tag;
         if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tag, sizeof(tag)))
            return true;
         return (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
      }

      // The path Windows gives what a handle holds, without \\?\ and in lower
      // case, to compare where a file is with where it was meant to be.
      String FinalPath_(HANDLE handle)
      {
         std::vector<wchar_t> buffer(4096);
         const DWORD length = GetFinalPathNameByHandleW(handle, &buffer[0], (DWORD) buffer.size(), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
         if (length == 0 || length >= buffer.size())
            return String();

         String path(&buffer[0]);
         if (path.Left(8).CompareNoCase(_T("\\\\?\\UNC\\")) == 0)
            path = _T("\\\\") + path.Mid(8);
         else if (path.Left(4) == _T("\\\\?\\"))
            path = path.Mid(4);
         path.MakeLower();
         return path;
      }

      bool DeleteByHandle_(HANDLE handle)
      {
         FILE_DISPOSITION_INFO disposition;
         disposition.DeleteFile = TRUE;
         return SetFileInformationByHandle(handle, FileDispositionInfo, &disposition, sizeof(disposition)) != 0;
      }

      bool IsOneName_(const String &name)
      {
         return !name.IsEmpty() && name != _T(".") && name != _T("..") && name.Find(_T('\\')) < 0 && name.Find(_T('/')) < 0 && name.Find(_T(':')) < 0;
      }

      // The path Windows gives what a handle holds, by its volume's name -
      // \\?\Volume{...}\Data - which no mount point or drive letter stands in,
      // or empty when it cannot be had.
      String VolumePath_(HANDLE handle)
      {
         std::vector<wchar_t> buffer(4096);
         const DWORD length = GetFinalPathNameByHandleW(handle, &buffer[0], (DWORD) buffer.size(), FILE_NAME_NORMALIZED | VOLUME_NAME_GUID);
         if (length == 0 || length >= buffer.size())
            return String();
         return String(&buffer[0]);
      }

      // Whether two handles hold the same file or folder: the volume and the
      // file's id on it.
      bool SameObject_(HANDLE first, HANDLE second)
      {
         FILE_ID_INFO one;
         FILE_ID_INFO two;
         if (!GetFileInformationByHandleEx(first, FileIdInfo, &one, sizeof(one)) ||
             !GetFileInformationByHandleEx(second, FileIdInfo, &two, sizeof(two)))
            return false;
         return one.VolumeSerialNumber == two.VolumeSerialNumber && memcmp(&one.FileId, &two.FileId, sizeof(one.FileId)) == 0;
      }

      // The administrators of the machine: BUILTIN\Administrators, SYSTEM and
      // TrustedInstaller (NT SERVICE\TrustedInstaller, whose SID Windows derives
      // from the name as it derives every service's).
      bool AdministratorSid_(PSID sid)
      {
         static const String trustedInstaller = _T("S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464");
         return sid != nullptr && IsValidSid(sid) &&
            (IsWellKnownSid(sid, WinBuiltinAdministratorsSid) || IsWellKnownSid(sid, WinLocalSystemSid) || SidText_(sid).CompareNoCase(trustedInstaller) == 0);
      }

      // Who but the administrators the access list of what handle holds lets do
      // any of rights to it, by name, or empty when nobody; whether its owner is
      // one of them is said in why. The entries that apply to it alone count -
      // one inherit-only is for what is made in it later - with their generic
      // rights mapped as the file system maps them; OWNER RIGHTS counts as its
      // owner. False, with why, when it cannot be read.
      bool OthersMay_(HANDLE handle, ACCESS_MASK rights, bool ownerToo, String &others, String &why)
      {
         others.Empty();

         // READ_CONTROL on the same file or folder, opened again by the handle
         // and never through it.
         HANDLE readable = INVALID_HANDLE_VALUE;
         const LONG status = OpenBelow_(handle, String(), READ_CONTROL | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                        FILE_OPEN, 0, nullptr, readable);
         if (status < 0)
         {
            why = Formatter::Format(_T("its permissions could not be read (Windows error {0})"), (int) WindowsErrorOf_(status));
            return false;
         }

         PSID owner = nullptr;
         PACL dacl = nullptr;
         PSECURITY_DESCRIPTOR descriptor = nullptr;
         const DWORD result = GetSecurityInfo(readable, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                              &owner, nullptr, &dacl, nullptr, &descriptor);
         CloseHandle(readable);
         if (result != ERROR_SUCCESS)
         {
            why = Formatter::Format(_T("its permissions could not be read (Windows error {0})"), (int) result);
            return false;
         }

         bool ok = true;
         const bool ownerTrusted = AdministratorSid_(owner);
         if (ownerToo && !ownerTrusted)
         {
            why = Formatter::Format(_T("it is owned by {0}"), SidName_(owner));
            ok = false;
         }
         else if (dacl == nullptr)
         {
            others = _T("everyone (it has no access list at all)");
         }
         else
         {
            GENERIC_MAPPING mapping = { FILE_GENERIC_READ, FILE_GENERIC_WRITE, FILE_GENERIC_EXECUTE, FILE_ALL_ACCESS };
            for (DWORD i = 0; i < dacl->AceCount; i++)
            {
               void *ace = nullptr;
               if (!GetAce(dacl, i, &ace) || ace == nullptr)
               {
                  others = _T("an account in an entry that could not be read");
                  break;
               }

               const ACE_HEADER *header = (const ACE_HEADER *) ace;
               if ((header->AceFlags & INHERIT_ONLY_ACE) != 0)
                  continue;

               if (header->AceType == ACCESS_ALLOWED_ACE_TYPE)
               {
                  const ACCESS_ALLOWED_ACE *allowed = (const ACCESS_ALLOWED_ACE *) ace;
                  PSID sid = (PSID) &allowed->SidStart;
                  DWORD mask = allowed->Mask;
                  MapGenericMask(&mask, &mapping);

                  SID_IDENTIFIER_AUTHORITY creatorAuthority = SECURITY_CREATOR_SID_AUTHORITY;
                  const bool ownerRights = IsValidSid(sid) && memcmp(GetSidIdentifierAuthority(sid), &creatorAuthority, sizeof(creatorAuthority)) == 0 &&
                                           *GetSidSubAuthorityCount(sid) == 1 && *GetSidSubAuthority(sid, 0) == SECURITY_CREATOR_OWNER_RIGHTS_RID;
                  if ((mask & rights) != 0 && !AdministratorSid_(sid) && !(ownerRights && ownerTrusted))
                  {
                     others = SidName_(sid);
                     break;
                  }
               }
               else if (header->AceType == ACCESS_ALLOWED_COMPOUND_ACE_TYPE || header->AceType == ACCESS_ALLOWED_OBJECT_ACE_TYPE ||
                        header->AceType == ACCESS_ALLOWED_CALLBACK_ACE_TYPE || header->AceType == ACCESS_ALLOWED_CALLBACK_OBJECT_ACE_TYPE)
               {
                  // Grants of a shape a file system does not use: not read, so
                  // not trusted either.
                  others = _T("an account in an entry of a form the server does not read");
                  break;
               }
            }
         }

         LocalFree(descriptor);
         return ok;
      }
   }

   bool
   OnlyAdministratorsControl(void *object, void *parent, String &why)
   {
      // On it: every right that changes it, what it points to or who may - its
      // data and attributes are how a link is turned elsewhere - or adds to it.
      // On its folder: removing it, or taking that folder's permissions; adding
      // a name beside it changes nothing it leads to.
      const ACCESS_MASK onIt = FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES | FILE_DELETE_CHILD | DELETE | WRITE_DAC | WRITE_OWNER;
      const ACCESS_MASK onItsFolder = FILE_DELETE_CHILD | WRITE_DAC | WRITE_OWNER;

      String others;
      if (!OthersMay_((HANDLE) object, onIt, true, others, why))
         return false;
      if (!others.IsEmpty())
      {
         why = Formatter::Format(_T("{0} may change it"), others);
         return false;
      }

      if (!OthersMay_((HANDLE) parent, onItsFolder, false, others, why))
      {
         why = _T("the permissions of the folder it is in could not be read: ") + why;
         return false;
      }
      if (!others.IsEmpty())
      {
         why = Formatter::Format(_T("{0} may remove it from the folder it is in"), others);
         return false;
      }

      return true;
   }

   HeldFolder::HeldFolder() :
      last_(nullptr)
   {

   }

   HeldFolder::~HeldFolder()
   {
      Release();
   }

   void
   HeldFolder::Release()
   {
      for (void *handle : handles_)
         CloseHandle((HANDLE) handle);
      handles_.clear();
      last_ = nullptr;
      path_.Empty();
   }

   bool
   HeldFolder::Hold(const String &requested, bool createLast, String &error)
   {
      Release();

      void *last = nullptr;
      if (!HoldChain_(requested, createLast, 0, last, error))
      {
         Release();
         return false;
      }

      last_ = last;
      return true;
   }

   bool
   HeldFolder::HoldAlso(void *opened, String &error)
   {
      if (last_ == nullptr)
      {
         error = _T("No folder is held.");
         return false;
      }

      // The handle is the caller's: a copy of it is what this holds.
      HANDLE copy = nullptr;
      if (!DuplicateHandle(GetCurrentProcess(), (HANDLE) opened, GetCurrentProcess(), &copy, 0, FALSE, DUPLICATE_SAME_ACCESS))
      {
         error = Formatter::Format(_T("A folder could not be held (Windows error {0})."), (int) GetLastError());
         return false;
      }
      handles_.push_back(copy);

      void *ignored = nullptr;
      return HoldThrough_(copy, FinalPath_(copy), 0, ignored, error);
   }

   // through: a folder opened through a link the administrators alone control,
   // already in handles_. Where it is, by its volume's name, held from that
   // volume's root as Hold holds a path; the folder so reached must be the one
   // through holds. last: that folder's handle.
   bool
   HeldFolder::HoldThrough_(void *through, const String &walked, int depth, void *&last, String &error)
   {
      // A link to a link to a link: eight is more than any layout needs, and a
      // chain that loops ends here.
      if (depth >= 8)
      {
         error = Formatter::Format(_T("{0} leads through more than eight links, and nothing is written through it."), walked);
         return false;
      }

      const String target = VolumePath_((HANDLE) through);
      if (target.IsEmpty())
      {
         error = Formatter::Format(_T("Where {0} leads could not be told (Windows error {1}), and nothing is written through it."), walked, (int) GetLastError());
         return false;
      }

      String why;
      void *reached = nullptr;
      if (!HoldChain_(target, false, depth + 1, reached, why))
      {
         error = Formatter::Format(_T("{0} leads to {1}, where: {2}"), walked, FinalPath_((HANDLE) through), why);
         return false;
      }

      if (!SameObject_((HANDLE) reached, (HANDLE) through))
      {
         error = Formatter::Format(_T("{0} led to another folder while it was being held, and nothing is written through it."), walked);
         return false;
      }

      last = reached;
      return true;
   }

   bool
   HeldFolder::HoldChain_(const String &requested, bool createLast, int depth, void *&last, String &error)
   {
      String path = requested;
      path.Replace(_T("/"), _T("\\"));

      // A drive's root as the configuration may spell it, C: for C:\.
      if (path.GetLength() == 2 && path[1] == ':')
         path += _T("\\");

      // The volume's root - C:\, \\server\share\ or \\?\Volume{...}\ - is the one
      // name looked up by its path; every folder below it is opened relative to
      // the one above.
      String root;
      String rest;
      const bool volume = path.Left(11).CompareNoCase(_T("\\\\?\\Volume{")) == 0;
      if (!volume && path.Left(8).CompareNoCase(_T("\\\\?\\UNC\\")) == 0)
         path = _T("\\\\") + path.Mid(8);
      else if (!volume && path.Left(4) == _T("\\\\?\\"))
         path = path.Mid(4);

      if (volume)
      {
         const int close = path.Find(_T("}\\"));
         if (close > 11)
         {
            root = path.Left(close + 2);
            rest = path.Mid(close + 2);
         }
      }
      else if (path.GetLength() >= 3 && path[1] == ':' && path[2] == '\\')
      {
         root = path.Left(3);
         rest = path.Mid(3);
      }
      else if (path.Left(2) == _T("\\\\"))
      {
         const int server = path.Find(_T('\\'), 2);
         const int share = server < 0 ? -1 : path.Find(_T('\\'), server + 1);
         if (server > 2 && share > server + 1)
         {
            root = path.Left(share + 1);
            rest = path.Mid(share + 1);
         }
      }

      if (root.IsEmpty())
      {
         error = Formatter::Format(_T("{0} is not a full path on a drive or a share."), requested);
         return false;
      }

      std::vector<String> names;
      for (const String &part : StringParser::SplitString(rest, _T("\\")))
      {
         if (part.IsEmpty() || part == _T("."))
            continue;
         if (!IsOneName_(part))
         {
            error = Formatter::Format(_T("{0} is not a plain path from its root, and nothing is written through it."), requested);
            return false;
         }
         names.push_back(part);
      }

      HANDLE parent = CreateFileW(root.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | FILE_TRAVERSE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
      if (parent == INVALID_HANDLE_VALUE)
      {
         error = Formatter::Format(_T("{0} could not be opened (Windows error {1})."), root, (int) GetLastError());
         return false;
      }
      handles_.push_back(parent);

      String walked = root.Left(root.GetLength() - 1);
      for (size_t index = 0; index < names.size(); index++)
      {
         const String &name = names[index];
         walked += _T("\\") + name;

         // FILE_LIST_DIRECTORY as well as the attributes: a handle that asks for
         // attributes alone takes no part in the share-access check, and its
         // refusal to share deleting would bind nobody (ServiceAccount's PathPin_
         // says the same).
         const bool last = index + 1 == names.size();
         HANDLE folder = INVALID_HANDLE_VALUE;
         const LONG status = OpenBelow_(parent, name, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | FILE_TRAVERSE,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE, last && createLast ? FILE_OPEN_IF : FILE_OPEN,
                                        FILE_DIRECTORY_FILE, nullptr, folder);
         if (status < 0)
         {
            error = status == StatusObjectNameNotFound_
               ? Formatter::Format(_T("{0} does not exist."), walked)
               : Formatter::Format(_T("{0} could not be opened (Windows error {1})."), walked, (int) WindowsErrorOf_(status));
            return false;
         }
         handles_.push_back(folder);

         if (IsReparsePoint_(folder))
         {
            // Gone through only when the administrators alone control it - a
            // junction or a mount point an administrator made to keep the data on
            // another drive - and then held on from where it leads (HoldThrough_).
            // One the service's account made, or may turn, is refused.
            String why;
            if (!OnlyAdministratorsControl(folder, parent, why))
            {
               error = Formatter::Format(_T("{0} is a junction, a symbolic link or a mount point, and {1}, so nothing is written through it. Nothing in a folder the service may write is a link but one only the administrators control."), walked, why);
               return false;
            }

            HANDLE through = INVALID_HANDLE_VALUE;
            const LONG followed = OpenThrough_(parent, name, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | FILE_TRAVERSE,
                                               FILE_SHARE_READ | FILE_SHARE_WRITE, through);
            if (followed < 0)
            {
               error = Formatter::Format(_T("Where {0} leads could not be opened (Windows error {1})."), walked, (int) WindowsErrorOf_(followed));
               return false;
            }
            handles_.push_back(through);

            void *reached = nullptr;
            if (!HoldThrough_(through, walked, depth, reached, error))
               return false;

            parent = (HANDLE) reached;
            continue;
         }

         parent = folder;
      }

      last = parent;
      if (depth == 0)
         path_ = walked;
      return true;
   }

   bool
   HeldFolder::RemoveFile(const String &name)
   {
      if (last_ == nullptr || !IsOneName_(name))
         return false;

      HANDLE file = INVALID_HANDLE_VALUE;
      const LONG status = OpenBelow_((HANDLE) last_, name, DELETE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                     FILE_OPEN, FILE_NON_DIRECTORY_FILE, nullptr, file);
      if (status == StatusObjectNameNotFound_)
         return true;
      if (status < 0)
      {
         SetLastError(WindowsErrorOf_(status));
         return false;
      }

      const bool removed = DeleteByHandle_(file);
      const DWORD removeError = GetLastError();
      CloseHandle(file);
      SetLastError(removeError);
      return removed;
   }

   bool
   HeldFolder::WriteNewFile(const String &name, const AnsiString &data, bool secret, const String &readerAccount, String &error)
   {
      if (last_ == nullptr || !IsOneName_(name))
      {
         error = _T("No folder is held to write in.");
         return false;
      }

      const String target = path_ + _T("\\") + name;

      // A file written earlier - by this program before, or put there by anyone -
      // goes first, by its name below the held folder: a file made anew keeps
      // nothing of it, and a link of that name is removed itself.
      if (!RemoveFile(name))
      {
         error = Formatter::Format(_T("{0} is there and could not be removed (Windows error {1})."), target, (int) GetLastError());
         return false;
      }

      PSECURITY_DESCRIPTOR descriptor = nullptr;
      if (secret)
      {
         String sddl = _T("D:P(A;;FA;;;SY)(A;;FA;;;BA)");
         const String self = CurrentUserSid_();
         if (!self.IsEmpty() && self != _T("S-1-5-18"))
            sddl += Formatter::Format(_T("(A;;FA;;;{0})"), self);

         if (!readerAccount.IsEmpty())
         {
            DWORD sidSize = 0;
            DWORD domainSize = 0;
            SID_NAME_USE use = SidTypeUnknown;
            LookupAccountNameW(nullptr, readerAccount.c_str(), nullptr, &sidSize, nullptr, &domainSize, &use);
            std::vector<BYTE> sid(sidSize > 0 ? sidSize : 1);
            std::vector<wchar_t> domain(domainSize + 1);
            if (sidSize == 0 || !LookupAccountNameW(nullptr, readerAccount.c_str(), &sid[0], &sidSize, &domain[0], &domainSize, &use))
            {
               error = Formatter::Format(_T("The account {0} could not be resolved (Windows error {1}), so {2} was not written for it to read."), readerAccount, (int) GetLastError(), target);
               return false;
            }
            sddl += Formatter::Format(_T("(A;;FR;;;{0})"), SidText_((PSID) &sid[0]));
         }

         if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &descriptor, nullptr))
         {
            error = Formatter::Format(_T("The permissions of {0} could not be built (Windows error {1})."), target, (int) GetLastError());
            return false;
         }
      }

      HANDLE file = INVALID_HANDLE_VALUE;
      const LONG status = OpenBelow_((HANDLE) last_, name, GENERIC_WRITE | DELETE | FILE_READ_ATTRIBUTES, 0,
                                     FILE_CREATE, FILE_NON_DIRECTORY_FILE, descriptor, file);
      if (descriptor != nullptr)
         LocalFree(descriptor);

      if (status < 0)
      {
         error = Formatter::Format(_T("{0} could not be made (Windows error {1})."), target, (int) WindowsErrorOf_(status));
         return false;
      }

      // Where the file is, asked of its handle, against where the held folder is,
      // asked of its own: the same folder, or nothing is written and the file goes
      // again.
      const String folder = FinalPath_((HANDLE) last_);
      const String actual = FinalPath_(file);
      String expected = name;
      expected.MakeLower();
      expected = folder + _T("\\") + expected;
      if (folder.IsEmpty() || actual.IsEmpty() || expected != actual)
      {
         DeleteByHandle_(file);
         CloseHandle(file);
         error = Formatter::Format(_T("{0} was made somewhere other than {1} ({2}), so nothing was written to it and it was removed."), name, path_, actual);
         return false;
      }

      DWORD written = 0;
      const bool ok = WriteFile(file, data.c_str(), (DWORD) data.GetLength(), &written, nullptr) != 0 &&
                      written == (DWORD) data.GetLength() && FlushFileBuffers(file) != 0;
      if (!ok)
      {
         DeleteByHandle_(file);
         CloseHandle(file);
         error = Formatter::Format(_T("{0} could not be written."), target);
         return false;
      }

      CloseHandle(file);
      return true;
   }
#endif

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
